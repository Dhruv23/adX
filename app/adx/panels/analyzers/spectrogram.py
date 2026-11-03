"""The spectrogram waterfall (phase_10.md 4.4): the newest row at the top, frequency across.

The engine keeps the waterfall as one RGBA image in a fixed ring of rows
(SpectrogramGeometry); this panel wraps that memory in a QImage once and paints it as two
blits around the ring's head - a texture upload, not geometry, and no copy.
"""

from __future__ import annotations

import ctypes

from PySide6.QtCore import QRectF, Qt, QTimer
from PySide6.QtGui import QHideEvent, QImage, QPainter, QPaintEvent, QShowEvent
from PySide6.QtWidgets import QComboBox, QHBoxLayout, QLabel, QVBoxLayout, QWidget

from adx.engine_bridge import AnalyzerFrame
from adx.panels.analyzers.host import AnalyzerHost, on_screen
from adx.panels.base import DockPanel

RATES: tuple[int, ...] = (30, 60, 120)


class Waterfall(QWidget):
    """Paints the ring; knows nothing about the engine but an address and a shape."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("spectrogram.view")
        self.setMinimumHeight(64)
        self.image: QImage | None = None
        self._memory: ctypes.Array[ctypes.c_ubyte] | None = None
        self._shape = (0, 0, 0)
        self.head = 0
        self.pushed = 0
        self.paints = 0

    def show_image(self, address: int, rows: int, columns: int, head: int, pushed: int) -> None:
        if address == 0 or rows == 0 or columns == 0:
            return
        if self._shape != (address, rows, columns):
            # The engine's buffer, unowned: it lives as long as the Analyzers do and is
            # sized once per configure, so the address only changes with the shape.
            self._memory = (ctypes.c_ubyte * (rows * columns * 4)).from_address(address)
            # Any buffer works at runtime; the stubs list bytes and memoryview only.
            self.image = QImage(
                self._memory,  # type: ignore[call-overload]
                columns,
                rows,
                columns * 4,
                QImage.Format.Format_RGBA8888,
            )
            self._shape = (address, rows, columns)
        if pushed != self.pushed:
            self.head, self.pushed = head, pushed
            self.update()

    def paintEvent(self, _event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        width, height = float(self.width()), float(self.height())
        painter.fillRect(self.rect(), self.palette().window())
        if self.image is None:
            return
        rows = self.image.height()
        columns = self.image.width()
        h = height / rows
        head = self.head
        # Flip y so the image's rows run upward: then rows 0..head end at the top with the
        # newest, and head+1..rows-1 (older) fill in below them.
        painter.translate(0.0, height)
        painter.scale(1.0, -1.0)
        painter.drawImage(
            QRectF(0.0, height - (head + 1) * h, width, (head + 1) * h),
            self.image,
            QRectF(0.0, 0.0, columns, head + 1),
        )
        if head + 1 < rows:
            painter.drawImage(
                QRectF(0.0, 0.0, width, (rows - head - 1) * h),
                self.image,
                QRectF(0.0, head + 1, columns, rows - head - 1),
            )
        painter.end()
        self.paints += 1


class SpectrogramPanel(DockPanel):
    panel_id = "spectrogram"
    title = "Spectrogram"
    kind = "spectrogram"
    default_area = Qt.DockWidgetArea.BottomDockWidgetArea
    scrolls = True

    def __init__(self, host: AnalyzerHost, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.host = host
        body = QWidget()
        layout = QVBoxLayout(body)
        layout.setContentsMargins(4, 4, 4, 4)
        options = QHBoxLayout()
        self.rate = QComboBox()
        self.rate.setObjectName("spectrogram.rate")
        self.rate.addItems([f"{r} rows/s" for r in RATES])
        self.rate.setCurrentText("60 rows/s")
        options.addWidget(QLabel("Speed"))
        options.addWidget(self.rate)
        options.addStretch(1)
        layout.addLayout(options)
        self.waterfall = Waterfall(body)
        layout.addWidget(self.waterfall, 1)
        self.setWidget(body)
        self.uploads = 0
        self.visibilityChanged.connect(lambda _v: QTimer.singleShot(0, self._recheck))
        self.rate.currentTextChanged.connect(lambda _t: self.apply())

    def settings(self) -> dict[str, object]:
        return {"spectrogram_rate": float(self.rate.currentText().split()[0])}

    # On screen, not merely "visible": a dock tabbed behind another reports isVisible()
    # but has an empty visible region. Rechecked after every show, hide and tab switch
    # (visibilityChanged), a beat later, once the layout has settled.
    def showEvent(self, event: QShowEvent) -> None:  # noqa: N802 - Qt's name
        super().showEvent(event)
        QTimer.singleShot(0, self._recheck)

    def hideEvent(self, event: QHideEvent) -> None:  # noqa: N802 - Qt's name
        super().hideEvent(event)
        QTimer.singleShot(0, self._recheck)

    def _recheck(self) -> None:
        try:
            shown = on_screen(self)
        except RuntimeError:
            return  # deleted meanwhile
        if shown != (self.kind in self.host.open):
            self.host.set_open(self.kind, shown, **(self.settings() if shown else {}))

    def apply(self) -> None:
        if self.kind in self.host.open:
            self.host.configure(**self.settings())

    def frame(self, frame: AnalyzerFrame | None) -> None:
        if frame is None or not self.isVisible():
            return
        self.waterfall.show_image(*frame.spectrogram)
        self.uploads += 1
