"""An oscilloscope trace of a block of samples. Phase 10 feeds it from a tap; here it
draws whatever array it is given, one polyline, decimated to the widget's width."""

from __future__ import annotations

import numpy as np
import numpy.typing as npt
from PySide6.QtCore import QPointF
from PySide6.QtGui import QColor, QPainter, QPaintEvent, QPen, QPolygonF
from PySide6.QtWidgets import QWidget

from adx.theme.tokens import DARK, Theme


class Scope(QWidget):
    """Draws samples in -1..1 across the width."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._samples: npt.NDArray[np.float32] = np.zeros(0, dtype=np.float32)
        self._theme: Theme = DARK
        self.setMinimumSize(120, 60)

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self._theme = theme
        self.update()

    def set_samples(self, samples: npt.NDArray[np.float32]) -> None:
        """A new block to draw."""
        self._samples = samples
        self.update()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(self._theme.panel))
        mid = self.height() / 2
        painter.setPen(QPen(QColor(self._theme.border)))
        painter.drawLine(QPointF(0, mid), QPointF(self.width(), mid))
        if len(self._samples) > 1:
            columns = max(2, self.width())
            index = np.linspace(0, len(self._samples) - 1, columns).astype(np.int64)
            values = np.clip(self._samples[index], -1.0, 1.0)
            polygon = QPolygonF(
                [QPointF(float(x), float(mid - v * mid * 0.95)) for x, v in enumerate(values)]
            )
            painter.setPen(QPen(QColor(self._theme.accent), 1.2))
            painter.drawPolyline(polygon)
        painter.end()
