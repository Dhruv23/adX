"""GrossBeat: its time map over the bar grid (phase_6.md 4.4's table: "needs a time/volume
curve editor over a bar grid").

GrossBeat plays one of seven patterns; the curve drawn is the engine's own map f(p) for
the chosen one (grossbeat_curve), the playback point against the cycle, with the cycle's
beats as grid lines. Click the curve to step to the next pattern; the gate and depth are
the generic grid's. Free-drawn time curves are not something the effect runs - its
patterns are built in - and drawing one here would be a curve the engine cannot play.
"""

from __future__ import annotations

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPainterPath, QPaintEvent, QPen
from PySide6.QtWidgets import QWidget

from adx import engine_bridge
from adx.panels.effect.generic_editor import slot_target
from adx.session import Session
from adx.theme.tokens import DARK

PATTERNS = (
    "Off",
    "Half speed",
    "Reverse half",
    "Stutter 1/8",
    "Tape stop",
    "Repeat beat",
    "Two thirds",
)


class TimeMap(QWidget):
    """f(p) over one cycle."""

    def __init__(self, session: Session, insert: int, slot: int) -> None:
        super().__init__()
        self.target = slot_target(session, insert, slot)
        self.pattern = 0
        self.beats = 4
        self.setMinimumHeight(140)
        self.setToolTip("Click to step to the next pattern")
        self.refresh()

    def refresh(self) -> None:
        """One read."""
        values = self.target.read_values()
        self.pattern = round(values.get("pattern", 0.0))
        self.beats = (1, 2, 4, 8)[max(0, min(3, round(values.get("length", 2.0))))]
        self.update()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor(DARK.raised))
        painter.setPen(QColor(DARK.border))
        for beat in range(1, self.beats):
            x = beat / self.beats * self.width()
            painter.drawLine(QPointF(x, 0), QPointF(x, self.height()))
        curve = engine_bridge.grossbeat_curve(self.pattern, 160)
        path = QPainterPath()
        for index, value in enumerate(curve):
            point = QPointF(
                index / (len(curve) - 1) * self.width(),
                self.height() - float(value) * (self.height() - 4) - 2,
            )
            if index == 0:
                path.moveTo(point)
            else:
                path.lineTo(point)
        painter.setPen(QPen(QColor(DARK.accent), 2))
        painter.drawPath(path)
        painter.setPen(QColor(DARK.text_dim))
        painter.drawText(
            QRectF(4, 2, 300, 14),
            Qt.AlignmentFlag.AlignLeft,
            f"{PATTERNS[self.pattern % len(PATTERNS)]} · {self.beats} beats",
        )
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self.target.turn("pattern", float((self.pattern + 1) % len(PATTERNS)))
