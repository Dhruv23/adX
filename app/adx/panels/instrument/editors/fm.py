"""FM: the algorithm matrix (phase_6.md 4.4's table).

The six operators and who modulates whom, for the chosen algorithm, as the engine runs
it (fm_algorithm reads the instrument's own routing table): carriers along the bottom,
modulators stacked above what they modulate, the feedback loop drawn as a ring. Each
operator box shows its level.
"""

from __future__ import annotations

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPaintEvent, QPen
from PySide6.QtWidgets import QWidget

from adx import engine_bridge
from adx.panels.editor_parts import TabbedEditor
from adx.panels.instrument.editors.common import tabbed
from adx.panels.instrument.generic_editor import channel_target
from adx.panels.params import ParamTarget
from adx.session import Session
from adx.theme.tokens import DARK

OPERATORS = 6


class AlgorithmView(QWidget):
    """The operator graph of the current algorithm."""

    def __init__(self, target: ParamTarget) -> None:
        super().__init__()
        self.target = target
        self.algorithm = 1
        self.levels = [1.0] * OPERATORS
        self.setMinimumHeight(170)
        self.refresh()

    def refresh(self) -> None:
        """One read of the algorithm and the levels."""
        values = self.target.read_values()
        self.algorithm = int(values.get("algorithm", 1.0))
        self.levels = [values.get(f"op{i + 1}.level", 1.0) for i in range(OPERATORS)]
        self.update()

    def layout_boxes(self) -> dict[int, QRectF]:
        """Each operator's box: carriers on the bottom row, modulators above their target."""
        modulators, carriers, _src, _dst = engine_bridge.fm_algorithm(self.algorithm)
        depth = [0] * OPERATORS
        for op in range(OPERATORS):
            # How many modulation hops the operator is from a carrier: its row.
            hops, frontier = 0, {op}
            while frontier and not any(carriers & (1 << f) for f in frontier) and hops < OPERATORS:
                frontier = {
                    t for t in range(OPERATORS) for f in frontier if modulators[t] & (1 << f)
                }
                hops += 1
            depth[op] = hops
        rows: dict[int, list[int]] = {}
        for op in range(OPERATORS):
            rows.setdefault(depth[op], []).append(op)
        boxes: dict[int, QRectF] = {}
        size = 34.0
        for row, ops in rows.items():
            for index, op in enumerate(ops):
                x = (index + 0.5) * self.width() / len(ops) - size / 2
                y = self.height() - 12 - (row + 1) * (size + 14)
                boxes[op] = QRectF(x, y, size, size)
        return boxes

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor(DARK.raised))
        modulators, carriers, source, target = engine_bridge.fm_algorithm(self.algorithm)
        boxes = self.layout_boxes()
        painter.setPen(QPen(QColor(DARK.text_dim), 1.5))
        for op, box in boxes.items():
            for m in range(OPERATORS):
                if modulators[op] & (1 << m) and m in boxes:
                    painter.drawLine(
                        boxes[m].center() + QPointF(0, 17), box.center() - QPointF(0, 17)
                    )
        if source in boxes and target in boxes:
            # The feedback loop: a ring round its source, a line to where it feeds.
            painter.setPen(QPen(QColor(DARK.roles["curve"]), 1.5))
            painter.drawEllipse(boxes[source].adjusted(-6, -6, 6, 6))
            if target != source:
                painter.drawLine(boxes[source].center(), boxes[target].center())
        for op, box in boxes.items():
            carrier = bool(carriers & (1 << op))
            painter.fillRect(box, QColor(DARK.accent if carrier else DARK.border))
            fill = QRectF(
                box.left(),
                box.bottom() - box.height() * self.levels[op],
                4,
                box.height() * self.levels[op],
            )
            painter.fillRect(fill, QColor(DARK.text))
            painter.setPen(QColor(DARK.window if carrier else DARK.text))
            painter.drawText(box, Qt.AlignmentFlag.AlignCenter, str(op + 1))
        painter.setPen(QColor(DARK.text_dim))
        painter.drawText(
            QRectF(4, 2, 200, 14), Qt.AlignmentFlag.AlignLeft, f"Algorithm {self.algorithm}"
        )
        painter.end()


def fm_editor(session: Session, channel: int) -> TabbedEditor:
    """The algorithm and each operator's envelope, beside every parameter."""
    return tabbed(session, channel, "fm", AlgorithmView(channel_target(session, channel)))
