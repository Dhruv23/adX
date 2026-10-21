"""Additive: the harmonic bar editor (phase_6.md 4.4's table).

One bar per ``harmonic.N``. Draw across them with the mouse to set many at once; the
whole stroke is one undo step (a gesture group), however many bars it crosses.
"""

from __future__ import annotations

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent
from PySide6.QtWidgets import QWidget

from adx.panels.editor_parts import TabbedEditor
from adx.panels.instrument.editors.common import tabbed
from adx.panels.instrument.generic_editor import channel_target, descriptors_of
from adx.panels.params import ParamTarget
from adx.session import Session
from adx.theme.tokens import DARK


class HarmonicBars(QWidget):
    """Bars for harmonic.1 .. harmonic.N, drawn with the mouse."""

    def __init__(self, session: Session, target: ParamTarget, count: int) -> None:
        super().__init__()
        self.session = session
        self.target = target
        self.count = count
        self.values = [0.0] * count
        self._drawing = False
        self.setMinimumHeight(120)
        self.setToolTip("Draw across the bars to set the harmonics' levels")
        self.refresh()

    def refresh(self) -> None:
        """One read of every level."""
        values = self.target.read_values()
        self.values = [
            values.get(f"harmonic.{i + 1}", 1.0 if i == 0 else 0.0) for i in range(self.count)
        ]
        self.update()

    def _bar_at(self, x: float) -> int:
        return max(0, min(self.count - 1, int(x / max(1, self.width()) * self.count)))

    def _set_from(self, event: QMouseEvent) -> None:
        bar = self._bar_at(event.position().x())
        level = max(0.0, min(1.0, 1.0 - event.position().y() / max(1, self.height())))
        if abs(self.values[bar] - level) > 1e-3:
            self.values[bar] = level
            self.target.turn(f"harmonic.{bar + 1}", level)
            self.update()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(DARK.raised))
        width = self.width() / max(1, self.count)
        # Each bar's slot, faintly, so the empty ones read as bars waiting to be drawn.
        painter.setPen(QColor(DARK.border))
        for index in range(self.count):
            painter.drawRect(QRectF(index * width + 1, 0, width - 2, self.height() - 1))
        for index, value in enumerate(self.values):
            height = value * self.height()
            painter.fillRect(
                QRectF(index * width + 1, self.height() - height, width - 2, height),
                QColor(DARK.accent),
            )
        painter.setPen(QColor(DARK.text_dim))
        painter.drawText(
            QRectF(8, 4, self.width() - 16, 18),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            f"Harmonics 1-{self.count}: drag across the bars to set each one's level",
        )
        for index in (0, 7, 15, 31, 63):
            if index < self.count:
                painter.drawText(
                    QRectF(index * width, self.height() - 16, max(width, 18), 14),
                    Qt.AlignmentFlag.AlignLeft,
                    str(index + 1),
                )
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self._drawing = True
        self.session.begin_gesture("Draw harmonics")
        self._set_from(event)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._drawing:
            self._set_from(event)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._drawing:
            self._drawing = False
            self.session.end_gesture()


def additive_editor(session: Session, channel: int) -> TabbedEditor:
    """Harmonic bars and envelopes, beside every parameter."""
    names = [
        str(d["name"]) for d in descriptors_of("additive") if str(d["name"]).startswith("harmonic.")
    ]
    count = len({n for n in names if n.split(".")[1].isdigit()})
    bars = HarmonicBars(session, channel_target(session, channel), max(1, count))
    return tabbed(session, channel, "additive", bars)
