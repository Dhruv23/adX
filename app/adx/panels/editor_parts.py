"""Parts the hand-built instrument and effect editors share (phase_6.md 4.4).

- :class:`TabbedEditor` - a hand-built overview beside the generic editor's full grid,
  so a hand-built editor only adds what a grid cannot show and never hides a parameter;
- :class:`EnvelopeSection` - an ADSR curve editor for every group that has attack, decay,
  sustain and release ("Envelope/ADSR anywhere: needs the shared curve_editor widget");
- :class:`WaveView` - a sample's waveform from the engine's peak cache, with markers.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import numpy as np
from PySide6.QtCore import QPointF, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPainterPath, QPaintEvent, QPen
from PySide6.QtWidgets import QGroupBox, QHBoxLayout, QTabWidget, QVBoxLayout, QWidget

from adx.engine_bridge import PeakCache, Peaks, WaveformGeometry
from adx.panels.params import GenericEditor, ParamTarget
from adx.theme.tokens import DARK
from adx.widgets.curve_editor import CurveEditor, adsr_shape, adsr_values

ADSR = ("attack", "decay", "sustain", "release")


class TabbedEditor(QTabWidget):
    """An overview tab (hand-built) and an "All parameters" tab (generic)."""

    def __init__(
        self, overview: QWidget, generic: GenericEditor, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.overview = overview
        self.generic = generic
        self.addTab(overview, "Overview")
        self.addTab(generic, "All parameters")

    def refresh(self) -> None:
        """Both tabs show the model's values."""
        refresh: Callable[[], None] | None = getattr(self.overview, "refresh", None)
        if refresh is not None:
            refresh()
        self.generic.refresh()


def adsr_groups(descriptors: dict[str, dict[str, Any]]) -> list[str]:
    """Every prefix with all four of attack, decay, sustain and release."""
    prefixes = {name.rsplit(".", 1)[0] for name in descriptors if "." in name}
    return sorted(p for p in prefixes if all(f"{p}.{s}" in descriptors for s in ADSR))


class EnvelopeSection(QWidget):
    """One curve editor per ADSR group; dragging it turns the four parameters."""

    def __init__(
        self,
        descriptors: dict[str, dict[str, Any]],
        target: ParamTarget,
        set_curve: Callable[[str, float, int, float], None] | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.target = target
        self.set_curve = set_curve
        self.editors: dict[str, CurveEditor] = {}
        layout = QHBoxLayout(self)
        for group in adsr_groups(descriptors):
            box = QGroupBox(f"{group} envelope")
            inner = QVBoxLayout(box)
            editor = CurveEditor()
            editor.setToolTip("Drag a point; alt-drag a segment to bend it")
            editor.edited.connect(lambda shape, g=group: self._edited(g, shape))
            inner.addWidget(editor)
            layout.addWidget(box)
            self.editors[group] = editor
        self._curves: dict[str, tuple[int, float]] = {}
        self.refresh()

    def refresh(self) -> None:
        """Re-shape every editor from one read of the values."""
        values = self.target.read_values()
        for group, editor in self.editors.items():
            a, d, s, r = (values.get(f"{group}.{stage}", 0.0) for stage in ADSR)
            curves = [
                self._curves.get(f"{group}.{stage}", (0, 0.0))
                for stage in ("attack", "decay", "release")
            ]
            shape = adsr_shape(max(a, 0.001), max(d, 0.001), s, max(r, 0.001), curves)
            editor.set_shape(shape, x_max=max(shape.xs[-1], 0.5))

    def _edited(self, group: str, shape: object) -> None:
        from adx.widgets.curve_editor import Shape

        assert isinstance(shape, Shape)
        attack, decay, sustain, release = adsr_values(shape)
        for stage, value in zip(ADSR, (attack, decay, sustain, release), strict=True):
            self.target.turn(f"{group}.{stage}", float(value))
        if self.set_curve is not None:
            for index, stage in ((0, "attack"), (1, "decay"), (3, "release")):
                name = f"{group}.{stage}"
                curve = (shape.kinds[index], shape.tensions[index])
                if curve != self._curves.get(name, (0, 0.0)):
                    self._curves[name] = curve
                    value = {"attack": attack, "decay": decay, "release": release}[stage]
                    self.set_curve(name, float(value), curve[0], curve[1])


class WaveView(QWidget):
    """A sample's waveform, drawn from the engine's peaks (never blocks on them), with
    vertical markers the owner can drag: zone start and end, loop points."""

    marker_moved = Signal(str, float)

    def __init__(self, cache: PeakCache, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.cache = cache
        self.geometry_builder = WaveformGeometry()
        self.peaks: Peaks | None = None
        self.frames = 0
        self.markers: dict[str, float] = {}
        self._line: list[QPointF] = []
        self._grab: str | None = None
        self._timer = QTimer(self)
        self._timer.setInterval(100)
        self._timer.timeout.connect(self._poll)
        self.setMinimumHeight(110)

    def open(self, path: str) -> None:
        """Show a file; its peaks fill in on the engine's worker."""
        self.peaks = self.cache.request_file(path) if path else None
        self._line = []
        self._timer.start()
        self._poll()

    def _poll(self) -> None:
        if self.peaks is None:
            self._timer.stop()
            return
        complete, failed, _progress, frames, _rate = self.peaks.status()
        self.frames = frames
        if frames:
            self._build()
        if complete or failed:
            self._timer.stop()
        self.update()

    def _build(self) -> None:
        assert self.peaks is not None
        columns = max(8, self.width())
        if self.geometry_builder.build(self.peaks, 0.0, float(self.frames), columns) < 0:
            return
        with self.geometry_builder.view() as view:
            vertices = np.array(view.array, copy=True)
        h = self.height() / 2.0
        self._line = [QPointF(i / 2.0, h - float(v[1]) * h) for i, v in enumerate(vertices)]

    def x_of(self, frame: float) -> float:
        """The pixel column of a sample frame."""
        return frame / max(1, self.frames) * self.width()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        theme = DARK
        painter.fillRect(self.rect(), QColor(theme.raised))
        if self._line:
            path = QPainterPath(self._line[0])
            for point in self._line[1:]:
                path.lineTo(point)
            painter.setPen(QPen(QColor(theme.roles["wave_fill"]), 1))
            painter.drawPath(path)
        for name, frame in self.markers.items():
            x = self.x_of(frame)
            painter.setPen(QPen(QColor(theme.accent if "loop" in name else theme.text), 2))
            painter.drawLine(QPointF(x, 0), QPointF(x, self.height()))
            painter.drawText(QRectF(x + 3, 2, 80, 14), Qt.AlignmentFlag.AlignLeft, name)
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        x = event.position().x()
        nearest = min(self.markers.items(), key=lambda m: abs(self.x_of(m[1]) - x), default=None)
        if nearest is not None and abs(self.x_of(nearest[1]) - x) < 6:
            self._grab = nearest[0]

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._grab is not None:
            frame = max(
                0.0,
                min(float(self.frames), event.position().x() / max(1, self.width()) * self.frames),
            )
            self.markers[self._grab] = frame
            self.update()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._grab is not None:
            self.marker_moved.emit(self._grab, self.markers[self._grab])
            self._grab = None
