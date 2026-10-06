"""A vertical piano keyboard beside the piano roll, following its pitch scroll."""

from __future__ import annotations

from PySide6.QtCore import QRectF, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent
from PySide6.QtWidgets import QWidget

from adx.theme.tokens import DARK, Theme

_BLACK = {1, 3, 6, 8, 10}


class PianoKeyboard(QWidget):
    """Keys for the visible pitch range; C keys are labelled; the scale is marked."""

    key_clicked = Signal(int)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._pitch_top = 84.0
        self._pixels_per_semitone = 14.0
        self._scale_root = 0
        self._scale_mask = 0
        self._theme: Theme = DARK
        self.setFixedWidth(DARK.metrics.keyboard_width)

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self._theme = theme
        self.update()

    def set_view(self, pitch_top: float, pixels_per_semitone: float) -> None:
        """Follow the roll's vertical scroll and zoom."""
        self._pitch_top = pitch_top
        self._pixels_per_semitone = max(1.0, pixels_per_semitone)
        self.update()

    def set_scale(self, root: int, mask: int) -> None:
        """Mark the scale's pitches."""
        self._scale_root = root
        self._scale_mask = mask
        self.update()

    def pitch_at(self, y: float) -> int:
        """The key under pixel row y."""
        return max(0, min(127, int((self._pitch_top - y / self._pixels_per_semitone) // 1)))

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        theme = self._theme
        painter.fillRect(self.rect(), QColor(theme.panel))
        pps = self._pixels_per_semitone
        low = max(0, int(self._pitch_top - self.height() / pps) - 1)
        high = min(127, int(self._pitch_top) + 1)
        for pitch in range(low, high + 1):
            y = (self._pitch_top - pitch - 1) * pps
            black = pitch % 12 in _BLACK
            rect = QRectF(0, y, self.width() * (0.62 if black else 1.0), pps)
            painter.fillRect(rect, QColor("#15171a" if black else "#d9dce1"))
            painter.setPen(QColor(theme.border))
            painter.drawLine(rect.bottomLeft(), rect.bottomRight())
            in_scale = self._scale_mask and self._scale_mask & (
                1 << ((pitch - self._scale_root) % 12)
            )
            if in_scale:
                painter.fillRect(QRectF(self.width() - 4, y + 1, 3, pps - 2), QColor(theme.accent))
            if pitch % 12 == 0 and pps >= 9:
                painter.setPen(QColor("#2a2d33"))
                painter.drawText(
                    QRectF(0, y, self.width() - 6, pps),
                    Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
                    f"C{pitch // 12 - 1}",
                )
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self.key_clicked.emit(self.pitch_at(event.position().y()))
