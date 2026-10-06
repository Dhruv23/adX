"""The reusable zoomable waveform view (phase_5.md 4.5).

Peaks come from the engine's ClipPeakCache, computed on a worker thread, coarsest tier
first: opening a file never blocks, and the view draws the coarse tier while the fine
ones arrive. Geometry is a min/max strip two vertices per column, built for the
visible range plus one width either side at the current zoom; pan and zoom inside that
move the item's transform and make no engine call. Phase 6's playlist clips are this
same widget at a different scale.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from PySide6.QtCore import QObject, Qt, QTimer, QUrl, Slot
from PySide6.QtGui import QColor
from PySide6.QtQuick import QQuickItem
from PySide6.QtQuickWidgets import QQuickWidget
from PySide6.QtWidgets import QLabel, QVBoxLayout, QWidget

from adx.engine_bridge import PeakCache, Peaks, WaveformGeometry
from adx.panels.base import DockPanel
from adx.quick import ULong, call
from adx.theme.tokens import DARK, Theme, qml_palette, role_colors

VIEW_QML = Path(__file__).with_name("view.qml")


@dataclass(slots=True)
class WaveView:
    """What the waveform shows: seconds at the left edge and pixels per second."""

    seconds_start: float = 0.0
    pixels_per_second: float = 100.0
    width: float = 800.0
    duration: float = 0.0

    @property
    def seconds_end(self) -> float:
        """Seconds at the right edge."""
        return self.seconds_start + self.width / self.pixels_per_second

    def fit(self) -> None:
        """Show the whole file."""
        if self.duration > 0:
            self.pixels_per_second = self.width / self.duration
            self.seconds_start = 0.0

    def zoom(self, factor: float, anchor_x: float) -> None:
        """Zoom about a pixel column, down to a sample per pixel and out to the file."""
        anchor = self.seconds_start + anchor_x / self.pixels_per_second
        floor = self.width / max(self.duration, 1e-3) * 0.5
        self.pixels_per_second = max(floor, min(48000.0, self.pixels_per_second * factor))
        self.seconds_start = anchor - anchor_x / self.pixels_per_second

    def pan(self, dx: float) -> None:
        """Scroll by pixels."""
        self.seconds_start = max(-1.0, self.seconds_start + dx / self.pixels_per_second)


@dataclass(frozen=True, slots=True)
class WaveBuilt:
    """What the last build covered."""

    seconds_start: float
    seconds_end: float
    pixels_per_second: float
    progress: int


class WaveInput(QObject):
    """Where the waveform QML forwards the item's input."""

    def __init__(self, view: WaveformView) -> None:
        super().__init__(view)
        self._view = view

    @Slot(float, float, float, float, int)
    def wheeled(self, x: float, y: float, dx: float, dy: float, modifiers: int) -> None:
        self._view.on_wheel(x, dx, dy, modifiers)

    @Slot(float, float, int, int)
    def pressed(self, x: float, y: float, button: int, modifiers: int) -> None:
        self._view.on_press(x)

    @Slot(float, float, int, int)
    def moved(self, x: float, y: float, buttons: int, modifiers: int) -> None:
        self._view.on_drag(x)

    @Slot(float, float)
    def resized(self, width: float, height: float) -> None:
        self._view.on_resize(width)


class WaveformView(QWidget):
    """A waveform of one audio file: wheel zooms (ctrl) or scrolls; drag scrolls."""

    def __init__(self, cache: PeakCache | None = None, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.cache = cache or PeakCache()
        self.builder = WaveformGeometry()
        self.view = WaveView()
        self.peaks: Peaks | None = None
        self.built: WaveBuilt | None = None
        self.tier = -1
        self._drag_x: float | None = None
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self.status = QLabel("No file")
        layout.addWidget(self.status)
        self.quick = QQuickWidget()
        self.quick.setResizeMode(QQuickWidget.ResizeMode.SizeRootObjectToView)
        self.input = WaveInput(self)
        self.quick.rootContext().setContextProperty("waveInput", self.input)
        self.quick.rootContext().setContextProperty("adxTheme", qml_palette(DARK))
        self.quick.setSource(QUrl.fromLocalFile(str(VIEW_QML)))
        root = self.quick.rootObject()
        item = root.findChild(QQuickItem, "wave") if root is not None else None
        if item is None:
            raise RuntimeError("waveform QML has no WaveformItem: " + str(self.quick.errors()))
        self.item: QQuickItem = item
        layout.addWidget(self.quick, 1)
        # Peaks arrive on a worker; a low-rate poll notices the next tier, and stops
        # once the file is complete.
        self._poll = QTimer(self)
        self._poll.setInterval(100)
        self._poll.timeout.connect(self._poll_peaks)
        self.apply_theme(DARK)

    def open(self, path: str) -> None:
        """Show a file. Returns at once; peaks fill in."""
        self.peaks = self.cache.request_file(path)
        self.built = None
        self.view.duration = 0.0
        self.status.setText(f"{Path(path).name}: analysing…")
        self._poll.start()
        self._poll_peaks()

    def open_samples(self, name: str, peaks: Peaks) -> None:
        """Show peaks requested elsewhere (a test, a clip already in memory)."""
        self.peaks = peaks
        self.built = None
        self.status.setText(name)
        self._poll.start()
        self._poll_peaks()

    def _poll_peaks(self) -> None:
        if self.peaks is None:
            return
        complete, failed, progress, frames, rate = self.peaks.status()
        if failed:
            self.status.setText(f"cannot read: {self.peaks.error()}")
            self._poll.stop()
            return
        if rate and not self.view.duration:
            self.view.duration = frames / rate
            self.view.fit()
        if rate and (self.built is None or self.built.progress != progress):
            self.rebuild(force=True, progress=progress)
        if complete:
            self._poll.stop()
            if rate:
                self.status.setText(f"{frames / rate:.1f} s · {rate} Hz · tier {self.tier}")

    def needs_rebuild(self) -> bool:
        """Whether the view left the built range or zoomed past 2x of it."""
        b = self.built
        if b is None:
            return True
        ratio = self.view.pixels_per_second / b.pixels_per_second
        return (
            ratio > 2.0
            or ratio < 0.5
            or self.view.seconds_start < b.seconds_start
            or self.view.seconds_end > b.seconds_end
        )

    def rebuild(self, force: bool = False, progress: int | None = None) -> None:
        """Build the strip for the view plus a width either side, and upload it."""
        if self.peaks is None or (not force and not self.needs_rebuild()):
            return
        _complete, _failed, prog, _frames, rate = self.peaks.status()
        if not rate:
            return
        span = self.view.seconds_end - self.view.seconds_start
        start = self.view.seconds_start - span
        end = self.view.seconds_end + span
        columns = max(2, round(self.view.width * 3))
        self.tier = self.builder.build(self.peaks, start * rate, end * rate, columns)
        with self.builder.view() as v:
            call(self.item, "upload", ULong(v.address), v.vertex_count, ULong(v.revision))
        self.built = WaveBuilt(
            start, end, self.view.pixels_per_second, prog if progress is None else progress
        )

    def apply_view(self) -> None:
        """Move the transform; rebuild only when needed."""
        call(self.item, "setView", self.view.seconds_start, self.view.pixels_per_second)
        self.rebuild()

    def set_playhead(self, seconds: float) -> None:
        """Draw the playhead (negative hides it)."""
        self.item.setProperty("playheadSeconds", seconds)

    def on_wheel(self, x: float, dx: float, dy: float, modifiers: int) -> None:
        if modifiers & int(Qt.KeyboardModifier.ControlModifier.value):
            self.view.zoom(1.2 ** (dy / 120.0), x)
        else:
            self.view.pan(-(dy or dx) / 120.0 * 60.0)
        self.apply_view()

    def on_press(self, x: float) -> None:
        self._drag_x = x

    def on_drag(self, x: float) -> None:
        if self._drag_x is not None:
            self.view.pan(self._drag_x - x)
            self._drag_x = x
            self.apply_view()

    def on_resize(self, width: float) -> None:
        self.view.width = max(1.0, width)
        if self.built is None:
            self.view.fit()
        self.apply_view()

    def apply_theme(self, theme: Theme) -> None:
        """Recolour without rebuilding."""
        self.quick.rootContext().setContextProperty("adxTheme", qml_palette(theme))
        self.quick.setClearColor(QColor(theme.panel))
        call(self.item, "setPalette", list(role_colors(theme)))


class WaveformPanel(DockPanel):
    """A dock holding one :class:`WaveformView` (the browser opens audio into it)."""

    panel_id = "waveform"
    title = "Waveform"
    default_area = Qt.DockWidgetArea.BottomDockWidgetArea

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.waveform = WaveformView()
        self.setWidget(self.waveform)
