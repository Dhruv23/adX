"""Small pictures for the time effects: the delay's repeats and the reverb's decay. The
generic grid already edits them; these show what the numbers mean."""

from __future__ import annotations

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPaintEvent
from PySide6.QtWidgets import QWidget

from adx.panels.effect.generic_editor import slot_target
from adx.session import Session
from adx.theme.tokens import DARK


class DelayTaps(QWidget):
    """Each repeat as a bar: spaced by the delay time, falling by the feedback."""

    def __init__(self, session: Session, insert: int, slot: int) -> None:
        super().__init__()
        self.target = slot_target(session, insert, slot)
        self.values: dict[str, float] = {}
        self.setMinimumHeight(90)
        self.refresh()

    def refresh(self) -> None:
        """One read."""
        self.values = self.target.read_values()
        self.update()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(DARK.raised))
        time_ms = max(1.0, self.values.get("timeMs", 375.0))
        feedback = max(0.0, min(0.99, self.values.get("feedback", 0.4)))
        span_ms = 2000.0
        level, at = 1.0, 0.0
        while at <= span_ms and level > 0.01:
            x = at / span_ms * self.width()
            height = level * (self.height() - 16)
            painter.fillRect(QRectF(x, self.height() - height, 4, height), QColor(DARK.accent))
            at += time_ms
            level *= feedback
        painter.setPen(QColor(DARK.text_dim))
        painter.drawText(
            QRectF(4, 2, 200, 14),
            Qt.AlignmentFlag.AlignLeft,
            f"{time_ms:.0f} ms · feedback {feedback:.2f}",
        )
        painter.end()


class ReverbDecay(QWidget):
    """The tail's envelope: longer for a bigger room, darker for more damping."""

    def __init__(self, session: Session, insert: int, slot: int) -> None:
        super().__init__()
        self.target = slot_target(session, insert, slot)
        self.values: dict[str, float] = {}
        self.setMinimumHeight(90)
        self.refresh()

    def refresh(self) -> None:
        """One read."""
        self.values = self.target.read_values()
        self.update()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(DARK.raised))
        room = max(0.05, self.values.get("room", 0.5))
        damp = max(0.0, min(1.0, self.values.get("damp", 0.5)))
        color = QColor(DARK.accent)
        color.setAlpha(int(255 * (1.0 - 0.6 * damp)))
        columns = max(1, self.width() // 3)
        for column in range(columns):
            t = column / columns
            height = (self.height() - 16) * (1.0 - t) ** (1.0 / room)
            painter.fillRect(QRectF(column * 3, self.height() - height, 2, height), color)
        painter.end()
