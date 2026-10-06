"""An overview strip: the whole length, the visible window over it, the playhead.
Drag the window to scroll the view it belongs to."""

from __future__ import annotations

from PySide6.QtCore import QRectF, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent
from PySide6.QtWidgets import QWidget

from adx.theme.tokens import DARK, Theme


class Timeline(QWidget):
    """Emits ``scroll_requested(tick_start)`` as the window is dragged."""

    scroll_requested = Signal(float)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._length = 1.0
        self._window = (0.0, 1.0)
        self._playhead = -1.0
        self._grab: float | None = None
        self._theme: Theme = DARK
        self.setFixedHeight(12)

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self._theme = theme
        self.update()

    def set_length(self, ticks: float) -> None:
        """The whole span the strip represents."""
        self._length = max(1.0, ticks)
        self.update()

    def set_window(self, start: float, end: float) -> None:
        """The visible range."""
        self._window = (start, end)
        self.update()

    def set_playhead(self, tick: float) -> None:
        """The playhead, or negative for none."""
        if tick != self._playhead:
            self._playhead = tick
            self.update()

    def _x(self, tick: float) -> float:
        return tick / self._length * self.width()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(self._theme.panel))
        start, end = self._window
        window = QColor(self._theme.accent)
        window.setAlpha(90)
        painter.fillRect(
            QRectF(self._x(start), 1, max(3.0, self._x(end) - self._x(start)), self.height() - 2),
            window,
        )
        if self._playhead >= 0:
            painter.fillRect(
                QRectF(self._x(self._playhead), 0, 2, self.height()),
                QColor(self._theme.roles["playhead"]),
            )
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        tick = event.position().x() / max(1, self.width()) * self._length
        start, end = self._window
        self._grab = tick - start if start <= tick <= end else (end - start) / 2
        self.scroll_requested.emit(max(0.0, tick - self._grab))

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._grab is not None:
            tick = event.position().x() / max(1, self.width()) * self._length
            self.scroll_requested.emit(max(0.0, tick - self._grab))

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self._grab = None
