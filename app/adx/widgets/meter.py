"""A stereo level meter: RMS bars, peak lines, a falling peak hold.

Fed from the 60 Hz frame (engine_bridge.FrameClock.levels): it reads numbers it is
given and never calls the engine itself.
"""

from __future__ import annotations

import math

from PySide6.QtCore import QRectF
from PySide6.QtGui import QColor, QPainter, QPaintEvent
from PySide6.QtWidgets import QWidget

from adx.theme.tokens import DARK, Theme

FLOOR_DB = -60.0


def to_db(linear: float) -> float:
    """Linear amplitude to dBFS, floored."""
    return FLOOR_DB if linear <= 1e-6 else max(FLOOR_DB, 20.0 * math.log10(linear))


def fraction(db: float) -> float:
    """dB to the meter's 0..1 height (0 dBFS is the top)."""
    return max(0.0, min(1.0, (db - FLOOR_DB) / -FLOOR_DB))


class LevelMeter(QWidget):
    """Two vertical bars."""

    def __init__(self, label: str = "", parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.label = label
        self._rms = (0.0, 0.0)
        self._peak = (0.0, 0.0)
        self._hold = [FLOOR_DB, FLOOR_DB]
        self._theme: Theme = DARK
        self.setMinimumSize(18, 80)
        self.setToolTip(label)

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self._theme = theme
        self.update()

    def set_levels(self, peak_l: float, peak_r: float, rms_l: float, rms_r: float) -> None:
        """New readings, linear."""
        self._peak = (peak_l, peak_r)
        self._rms = (rms_l, rms_r)
        for i, peak in enumerate(self._peak):
            self._hold[i] = max(to_db(peak), self._hold[i] - 0.6)
        self.update()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        theme = self._theme
        painter.fillRect(self.rect(), QColor(theme.panel))
        width = (self.width() - 3) / 2
        height = self.height()
        for i in range(2):
            x = 1 + i * (width + 1)
            painter.fillRect(QRectF(x, 0, width, height), QColor(theme.raised))
            rms = fraction(to_db(self._rms[i])) * height
            peak = fraction(to_db(self._peak[i])) * height
            color = (
                QColor(theme.roles["wave_fill"])
                if to_db(self._peak[i]) < -3
                else QColor(theme.danger)
            )
            painter.fillRect(QRectF(x, height - rms, width, rms), color)
            painter.fillRect(QRectF(x, height - peak, width, 1), QColor(theme.text))
            hold = fraction(self._hold[i]) * height
            painter.fillRect(QRectF(x, height - hold, width, 2), QColor(theme.accent))
        painter.end()
