"""Wavetable: the 3D table view (phase_6.md 4.4's table).

The chosen built-in table's frames, read from the engine's own tables (wavetable_frames),
drawn back to front in perspective, the frame the position knob points at highlighted. A
table imported from a .wav is the channel's file; it is named rather than drawn.
"""

from __future__ import annotations

import numpy as np
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPaintEvent, QPen
from PySide6.QtWidgets import QWidget

from adx import engine_bridge
from adx.panels.editor_parts import TabbedEditor
from adx.panels.instrument.editors.common import tabbed
from adx.panels.instrument.generic_editor import channel_target
from adx.panels.params import ParamTarget
from adx.session import Session
from adx.theme.tokens import DARK

#: Built-in banks; bank 4 is the channel's own .wav.
BANKS = ("Classic", "Pulse", "Harmonic", "Formant", "User")


class TableView(QWidget):
    """Frames of a wavetable in perspective."""

    def __init__(self, target: ParamTarget) -> None:
        super().__init__()
        self.target = target
        self.bank = 0
        self.position = 0.0
        self._frames = np.zeros((0, 0), dtype=np.float32)
        self._loaded = -1
        self.setMinimumHeight(170)
        self.refresh()

    def refresh(self) -> None:
        """One read of the table and position; the table's frames only when it changed."""
        values = self.target.read_values()
        self.bank = int(values.get("table", 0.0))
        self.position = float(values.get("position", 0.0))
        if self.bank != self._loaded:
            self._loaded = self.bank
            self._frames = (
                engine_bridge.wavetable_frames(self.bank, 96)
                if self.bank < 4
                else np.zeros((0, 0), dtype=np.float32)
            )
        self.update()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor(DARK.raised))
        painter.setPen(QColor(DARK.text_dim))
        painter.drawText(
            QRectF(4, 2, 300, 14), Qt.AlignmentFlag.AlignLeft, f"{BANKS[min(self.bank, 4)]} table"
        )
        frames = self._frames
        if frames.size == 0:
            painter.drawText(
                self.rect(), Qt.AlignmentFlag.AlignCenter, "The channel's own .wav table"
            )
            painter.end()
            return
        count = frames.shape[0]
        current = round(self.position * (count - 1))
        depth_x, depth_y = self.width() * 0.25, self.height() * 0.45
        width, amplitude = self.width() * 0.7, self.height() * 0.18
        for frame in range(count - 1, -1, -1):
            t = frame / max(1, count - 1)
            ox = 8 + t * depth_x
            oy = self.height() * 0.78 - t * depth_y
            path = QPainterPath()
            samples = frames[frame]
            for i, value in enumerate(samples):
                point = QPointF(ox + i / (len(samples) - 1) * width, oy - float(value) * amplitude)
                if i == 0:
                    path.moveTo(point)
                else:
                    path.lineTo(point)
            chosen = frame == current
            painter.setPen(
                QPen(QColor(DARK.accent if chosen else DARK.text_dim), 2 if chosen else 1)
            )
            painter.drawPath(path)
        painter.end()


def wavetable_editor(session: Session, channel: int) -> TabbedEditor:
    """The table and the envelope, beside every parameter."""
    return tabbed(session, channel, "wavetable", TableView(channel_target(session, channel)))
