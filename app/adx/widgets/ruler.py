"""A time ruler: bar numbers and beat ticks above a timeline view; click or drag to move
the playhead, snapped to the owner's grid (Alt: no snap).

With markers (the playlist's): each is a flag with its name. Drag a flag to move the
marker, double-click empty ruler to add one, right-click a flag to rename, delete or
jump to it (phase_6.md 4.1). The ruler holds the markers it was last given to draw -
view state - and every change is a signal the owner turns into a command.
"""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import (
    QColor,
    QContextMenuEvent,
    QMouseEvent,
    QPainter,
    QPaintEvent,
    QPen,
    QPolygonF,
)
from PySide6.QtWidgets import QInputDialog, QMenu, QWidget

from adx.theme.tokens import DARK, Theme

#: A flag is grabbed within this many pixels of its line.
FLAG_PIXELS = 6.0


class Ruler(QWidget):
    """Draws bars and beats for a view given as (tick_start, pixels_per_tick)."""

    seek_requested = Signal(float)
    marker_added = Signal(int)
    marker_moved = Signal(int, int, str)
    marker_removed = Signal(int)

    def __init__(self, ppq: int, parent: QWidget | None = None, markers: bool = False) -> None:
        super().__init__(parent)
        self._ppq = ppq
        self._tick_start = 0.0
        self._pixels_per_tick = 0.01
        self._numerator = 4
        self._denominator = 4
        self._playhead = -1.0
        self._theme: Theme = DARK
        self.markers_enabled = markers
        self._markers: list[tuple[int, int, str]] = []
        self._drag: tuple[int, str] | None = None
        self._drag_tick = 0
        self._scrubbing = False
        #: The grid a seek snaps to, in ticks (0: none) - the owner's snap setting.
        self.snap_ticks: Callable[[], int] = lambda: 0
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setToolTip("Click or drag to move the playhead (snaps to the grid; Alt: free)")
        self.setFixedHeight(DARK.metrics.ruler_height + (14 if markers else 0))

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self._theme = theme
        self.update()

    def set_view(self, tick_start: float, pixels_per_tick: float) -> None:
        """Follow the view (a repaint, no engine call)."""
        self._tick_start = tick_start
        self._pixels_per_tick = max(pixels_per_tick, 1e-9)
        self.update()

    def set_meter(self, numerator: int, denominator: int) -> None:
        """The time signature bars are counted in."""
        self._numerator = max(1, numerator)
        self._denominator = max(1, denominator)
        self.update()

    def set_playhead(self, tick: float) -> None:
        """Where to draw the playhead marker; negative hides it."""
        if tick != self._playhead:
            self._playhead = tick
            self.update()

    def set_markers(self, markers: list[tuple[int, int, str]]) -> None:
        """(id, tick, name) per marker, as just read from the project."""
        if markers != self._markers:
            self._markers = list(markers)
            self.update()

    def tick_at(self, x: float) -> float:
        """The tick under a pixel column."""
        return self._tick_start + x / self._pixels_per_tick

    def x_of(self, tick: float) -> float:
        """The pixel column of a tick."""
        return (tick - self._tick_start) * self._pixels_per_tick

    def seek_tick(self, x: float, free: bool = False) -> float:
        """The tick a click at ``x`` seeks to: snapped to the grid unless ``free``."""
        tick = max(0.0, self.tick_at(x))
        grid = 0 if free else self.snap_ticks()
        return float(round(tick / grid) * grid) if grid > 0 else tick

    def marker_at(self, x: float) -> tuple[int, int, str] | None:
        """The marker whose flag is under a pixel column."""
        for marker in reversed(self._markers):
            if abs(self.x_of(marker[1]) - x) <= FLAG_PIXELS:
                return marker
        return None

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        theme = self._theme
        painter.fillRect(self.rect(), QColor(theme.panel))
        flags = 14 if self.markers_enabled else 0
        beat = self._ppq * 4 // self._denominator
        bar = beat * self._numerator
        # Label every bar that leaves 40 px of room, every 2^n bars otherwise.
        every = 1
        while bar * every * self._pixels_per_tick < 40:
            every *= 2
        first = int(self._tick_start // bar)
        last = int(self.tick_at(self.width()) // bar) + 1
        painter.setPen(QPen(QColor(theme.text_dim)))
        for index in range(max(0, first), last + 1):
            x = (index * bar - self._tick_start) * self._pixels_per_tick
            if index % every == 0:
                painter.drawLine(QPointF(x, flags), QPointF(x, self.height()))
                painter.drawText(
                    QRectF(x + 3, flags, 60, self.height() - flags),
                    Qt.AlignmentFlag.AlignVCenter,
                    str(index + 1),
                )
            if beat * self._pixels_per_tick >= 8 and index % every == 0:
                for b in range(1, self._numerator):
                    bx = x + b * beat * self._pixels_per_tick
                    painter.drawLine(QPointF(bx, self.height() * 0.75), QPointF(bx, self.height()))
        for _marker_id, tick, name in self._markers:
            x = self.x_of(tick)
            painter.fillRect(QRectF(x, 0, 2, self.height()), QColor(theme.accent))
            painter.fillRect(QRectF(x, 0, 8 + 6 * len(name), 13), QColor(theme.raised))
            painter.setPen(QColor(theme.text))
            painter.drawText(QRectF(x + 4, 0, 200, 13), Qt.AlignmentFlag.AlignVCenter, name)
        if self._playhead >= 0:
            x = (self._playhead - self._tick_start) * self._pixels_per_tick
            color = QColor(theme.roles["playhead"])
            painter.setPen(QPen(color, 2))
            painter.drawLine(QPointF(x, 0), QPointF(x, self.height()))
            # A handle to grab: the playhead's head, as in every DAW.
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(color)
            top = float(flags)
            painter.drawPolygon(
                QPolygonF([QPointF(x - 6, top), QPointF(x + 6, top), QPointF(x, top + 9)])
            )
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if event.button() != Qt.MouseButton.LeftButton:
            return
        x = event.position().x()
        marker = self.marker_at(x) if self.markers_enabled else None
        if marker is not None:
            self._drag = (marker[0], marker[2])
            self._drag_tick = marker[1]
            return
        self._scrubbing = True
        self._scrub(event)

    def _scrub(self, event: QMouseEvent) -> None:
        free = bool(event.modifiers() & Qt.KeyboardModifier.AltModifier)
        self.seek_requested.emit(self.seek_tick(event.position().x(), free))

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._scrubbing:
            self._scrub(event)
            return
        if self._drag is not None:
            beat = self._ppq * 4 // self._denominator
            tick = max(0, round(self.tick_at(event.position().x()) / beat) * beat)
            self._drag_tick = tick
            marker_id, _name = self._drag
            self._markers = [(i, tick if i == marker_id else t, n) for i, t, n in self._markers]
            self.update()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self._scrubbing = False
        if self._drag is not None:
            marker_id, name = self._drag
            self._drag = None
            self.marker_moved.emit(marker_id, self._drag_tick, name)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self.markers_enabled and self.marker_at(event.position().x()) is None:
            beat = self._ppq * 4 // self._denominator
            self.marker_added.emit(max(0, round(self.tick_at(event.position().x()) / beat) * beat))

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:  # noqa: N802 - Qt's name
        marker = self.marker_at(event.pos().x()) if self.markers_enabled else None
        if marker is None:
            return
        marker_id, tick, name = marker
        menu = QMenu(self)
        jump = menu.addAction("Jump here")
        rename = menu.addAction("Rename…")
        remove = menu.addAction("Delete marker")
        chosen = menu.exec(event.globalPos())
        if chosen is jump:
            self.seek_requested.emit(float(tick))
        elif chosen is rename:
            text, ok = QInputDialog.getText(self, "Rename marker", "Name", text=name)
            if ok and text.strip():
                self.marker_moved.emit(marker_id, tick, text.strip())
        elif chosen is remove:
            self.marker_removed.emit(marker_id)
