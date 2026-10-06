"""A magnitude spectrum on a log-frequency axis. Phase 10 computes the spectrum in the
engine; this draws the bins it is handed, as a filled curve."""

from __future__ import annotations

import numpy as np
import numpy.typing as npt
from PySide6.QtCore import QPointF
from PySide6.QtGui import QColor, QPainter, QPaintEvent, QPolygonF
from PySide6.QtWidgets import QWidget

from adx.theme.tokens import DARK, Theme

MIN_HZ, MAX_HZ = 20.0, 20000.0
FLOOR_DB = -90.0


class Spectrum(QWidget):
    """Draws dB magnitudes over linearly spaced bins up to Nyquist."""

    def __init__(self, sample_rate: int = 48000, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.sample_rate = sample_rate
        self._db: npt.NDArray[np.float32] = np.zeros(0, dtype=np.float32)
        self._theme: Theme = DARK
        self.setMinimumSize(160, 80)

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self._theme = theme
        self.update()

    def set_magnitudes(self, db: npt.NDArray[np.float32]) -> None:
        """Bin magnitudes in dB, bin 0 at DC and the last at Nyquist."""
        self._db = db
        self.update()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(self._theme.panel))
        bins = len(self._db)
        if bins > 2:
            width, height = self.width(), self.height()
            x = np.arange(width, dtype=np.float64)
            hz = MIN_HZ * (MAX_HZ / MIN_HZ) ** (x / max(1, width - 1))
            index = np.clip(
                (hz / (self.sample_rate / 2) * (bins - 1)).astype(np.int64), 0, bins - 1
            )
            level = np.clip((self._db[index] - FLOOR_DB) / -FLOOR_DB, 0.0, 1.0)
            points = [QPointF(0.0, float(height))]
            points += [
                QPointF(float(px), float(height - lv * height))
                for px, lv in zip(x, level, strict=True)
            ]
            points.append(QPointF(float(width), float(height)))
            fill = QColor(self._theme.accent)
            fill.setAlpha(110)
            painter.setBrush(fill)
            painter.setPen(QColor(self._theme.accent))
            painter.drawPolygon(QPolygonF(points))
        painter.end()
