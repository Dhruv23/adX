"""Dynamics: the transfer curve and the gain-reduction meter (phase_6.md 4.4's table:
"Compressor / Multiband / Limiter: needs a transfer curve + gain-reduction meter").

The curve is the compressor's static gain computer - threshold, ratio and a quadratic soft
knee, the same law Dynamics.cpp runs - drawn input dB against output dB. The meter reads
the running effect's own gain reduction (Engine.gain_reduction: one relaxed atomic the
audio thread writes each block) at 30 Hz while visible.
"""

from __future__ import annotations

from PySide6.QtCore import QPointF, QRectF, Qt, QTimer
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPaintEvent, QPen
from PySide6.QtWidgets import QWidget

from adx.panels.effect.generic_editor import slot_target
from adx.session import Session
from adx.theme.tokens import DARK

FLOOR = -60.0


def static_gain(level: float, threshold: float, ratio: float, knee: float) -> float:
    """The compressor's gain computer (Giannoulis et al.), as Dynamics.cpp computes it:
    the change in dB for an input at ``level`` dB."""
    over = level - threshold
    slope = 1.0 / max(1.0, ratio) - 1.0
    if 2 * over <= -knee:
        return 0.0
    if knee > 0 and 2 * abs(over) <= knee:
        x = over + knee / 2
        return slope * x * x / (2 * knee)
    return slope * over


class DynamicsView(QWidget):
    """Transfer curve (left) and GR meter (right)."""

    def __init__(self, session: Session, insert: int, slot: int, effect: str) -> None:
        super().__init__()
        self.session = session
        self.slot = slot
        self.effect = effect
        self.target = slot_target(session, insert, slot)
        self.reduction = 0.0
        self.setMinimumHeight(160)
        self._timer = QTimer(self)
        self._timer.setInterval(33)
        self._timer.timeout.connect(self._meter_tick)
        self.values: dict[str, float] = {}
        self.refresh()

    def refresh(self) -> None:
        """One read of the settings."""
        self.values = self.target.read_values()
        self.update()

    def _law(self) -> tuple[float, float, float]:
        v = self.values
        if self.effect == "Limiter":
            return v.get("ceiling", -1.0), 1000.0, 0.0
        if self.effect == "MultibandComp":
            return v.get("band1.threshold", -18.0), v.get("band1.ratio", 4.0), 6.0
        return v.get("threshold", -18.0), v.get("ratio", 4.0), v.get("knee", 6.0)

    def _meter_tick(self) -> None:
        if self.isVisible():
            self.reduction = self.session.engine.gain_reduction(self.slot)
            self.update()

    def showEvent(self, event: object) -> None:  # noqa: N802 - Qt's name
        self._timer.start()

    def hideEvent(self, event: object) -> None:  # noqa: N802 - Qt's name
        self._timer.stop()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor(DARK.raised))
        side = min(self.width() - 40, self.height()) - 8
        box = QRectF(4, 4, side, side)
        threshold, ratio, knee = self._law()

        def point(level: float, out: float) -> QPointF:
            return QPointF(
                box.left() + (level - FLOOR) / -FLOOR * box.width(),
                box.bottom() - (out - FLOOR) / -FLOOR * box.height(),
            )

        painter.setPen(QColor(DARK.border))
        painter.drawLine(point(FLOOR, FLOOR), point(0.0, 0.0))
        path = QPainterPath(point(FLOOR, FLOOR))
        for step in range(121):
            level = FLOOR + step * (-FLOOR) / 120
            path.lineTo(point(level, level + static_gain(level, threshold, ratio, knee)))
        painter.setPen(QPen(QColor(DARK.accent), 2))
        painter.drawPath(path)
        meter = QRectF(box.right() + 12, 4, 18, box.height())
        painter.fillRect(meter, QColor(DARK.window))
        depth = min(1.0, -self.reduction / 24.0)
        painter.fillRect(
            QRectF(meter.left(), meter.top(), meter.width(), meter.height() * depth),
            QColor(DARK.roles["meter_hot"]),
        )
        painter.setPen(QColor(DARK.text_dim))
        painter.drawText(
            QRectF(meter.left() - 10, meter.bottom() - 14, 40, 14),
            Qt.AlignmentFlag.AlignCenter,
            f"{self.reduction:.1f}",
        )
        painter.end()
