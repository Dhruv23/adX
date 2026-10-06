"""A time ruler: bar numbers and beat ticks above a timeline view; click to seek."""

from __future__ import annotations

from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent, QPen
from PySide6.QtWidgets import QWidget

from adx.theme.tokens import DARK, Theme


class Ruler(QWidget):
    """Draws bars and beats for a view given as (tick_start, pixels_per_tick)."""

    seek_requested = Signal(float)

    def __init__(self, ppq: int, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._ppq = ppq
        self._tick_start = 0.0
        self._pixels_per_tick = 0.01
        self._numerator = 4
        self._denominator = 4
        self._playhead = -1.0
        self._theme: Theme = DARK
        self.setFixedHeight(DARK.metrics.ruler_height)

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

    def tick_at(self, x: float) -> float:
        """The tick under a pixel column."""
        return self._tick_start + x / self._pixels_per_tick

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        theme = self._theme
        painter.fillRect(self.rect(), QColor(theme.panel))
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
                painter.drawLine(QPointF(x, 0), QPointF(x, self.height()))
                painter.drawText(
                    QRectF(x + 3, 0, 60, self.height()),
                    Qt.AlignmentFlag.AlignVCenter,
                    str(index + 1),
                )
            if beat * self._pixels_per_tick >= 8 and index % every == 0:
                for b in range(1, self._numerator):
                    bx = x + b * beat * self._pixels_per_tick
                    painter.drawLine(QPointF(bx, self.height() * 0.65), QPointF(bx, self.height()))
        if self._playhead >= 0:
            x = (self._playhead - self._tick_start) * self._pixels_per_tick
            painter.setPen(QPen(QColor(theme.roles["playhead"]), 2))
            painter.drawLine(QPointF(x, 0), QPointF(x, self.height()))
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if event.button() == Qt.MouseButton.LeftButton:
            self.seek_requested.emit(max(0.0, self.tick_at(event.position().x())))
