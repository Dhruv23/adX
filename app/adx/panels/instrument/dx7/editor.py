"""The dx7 editor (phase_11.5.md 3, 4.3): an overview beside the generic editor, which
already gives every one of the voice's parameters a control, automation and MIDI learn.

The overview draws what a grid of numbers cannot: the algorithm, the chosen operator's
four-rate/four-level envelope and its keyboard level scaling, and the pitch envelope;
holds the voice's 10-character name; exports the voice as `.syx`; sends edits to a
connected FM-1 while editing; and converts the voice, approximately, to the FM
instrument with a report of what was lost.
"""

from __future__ import annotations

from collections.abc import Callable
from itertools import pairwise

from PySide6.QtCore import QPointF, QRectF
from PySide6.QtGui import QColor, QPainter, QPaintEvent, QPen
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from adx.hardware import dx7
from adx.panels.editor_parts import TabbedEditor
from adx.panels.instrument.editors.fm import AlgorithmView
from adx.panels.instrument.generic_editor import channel_target, generic_for
from adx.panels.params import ParamTarget
from adx.session import Session
from adx.theme.tokens import DARK

#: Set by the main window: (channel id, on) turns "Send to FM-1 while editing" on or off.
LIVE_EDIT_HOOK: list[Callable[[int, bool], None]] = []


class Dx7AlgorithmView(AlgorithmView):
    """The FM editor's algorithm graph, with the DX7's 0-99 output levels."""

    def refresh(self) -> None:
        values = self.target.read_values()
        self.algorithm = int(values.get("algorithm", 1.0))
        self.levels = [values.get(f"op{i + 1}.level", 0.0) / 99.0 for i in range(6)]
        self.update()


def eg_points(rates: list[float], levels: list[float]) -> list[tuple[float, float]]:
    """The envelope's corners as (seconds, level 0..1): stages 1-3 from L4, a held
    sustain, then stage 4. A rough time scale, for the picture only."""

    def seconds(rate: float, a: float, b: float) -> float:
        return float(41.0 * 2.0 ** (-rate / 7.0)) * max(0.02, abs(b - a) / 99.0)

    t = 0.0
    out = [(0.0, levels[3] / 99.0)]
    previous = levels[3]
    for stage in range(3):
        t += seconds(rates[stage], previous, levels[stage])
        out.append((t, levels[stage] / 99.0))
        previous = levels[stage]
    t += max(0.3, t * 0.3)  # key held
    out.append((t, levels[2] / 99.0))
    t += seconds(rates[3], levels[2], levels[3])
    out.append((t, levels[3] / 99.0))
    return out


def scaling_points(values: dict[str, float], op: int) -> list[tuple[int, float]]:
    """Keyboard level scaling across the keys (0..127) in dB, roughly as the DX7 does:
    depth per curve either side of the break point (break 0 = A-1, 39 = C3)."""
    key = f"op{op}.kls."
    brk = int(values.get(key + "break", 39)) + 21
    ldepth, rdepth = values.get(key + "ldepth", 0.0), values.get(key + "rdepth", 0.0)
    lcurve, rcurve = int(values.get(key + "lcurve", 0)), int(values.get(key + "rcurve", 0))
    out = []
    for note in range(0, 128, 3):
        distance = (note - brk) / 3.0
        depth, curve = (rdepth, rcurve) if distance >= 0 else (ldepth, lcurve)
        amount = abs(distance) / 36.0
        shaped = amount if curve in (0, 3) else amount**2
        sign = 1.0 if curve >= 2 else -1.0
        out.append((note, sign * depth / 99.0 * min(1.0, shaped) * 24.0))
    return out


class CurveView(QWidget):
    """The chosen operator's EG (or the pitch EG) and keyboard scaling, redrawn from the
    values on every refresh."""

    def __init__(self, target: ParamTarget) -> None:
        super().__init__()
        self.target = target
        self.op = 1
        self.eg: list[tuple[float, float]] = []
        self.pitch: list[tuple[float, float]] = []
        self.scaling: list[tuple[int, float]] = []
        self.setMinimumHeight(150)

    def refresh(self) -> None:
        v = self.target.read_values()
        p = f"op{self.op}.eg."
        self.eg = eg_points(
            [v.get(f"{p}r{i}", 99.0) for i in range(1, 5)],
            [v.get(f"{p}l{i}", 99.0 if i < 4 else 0.0) for i in range(1, 5)],
        )
        self.pitch = eg_points(
            [v.get(f"peg.r{i}", 99.0) for i in range(1, 5)],
            [v.get(f"peg.l{i}", 50.0) for i in range(1, 5)],
        )
        self.scaling = scaling_points(v, self.op)
        self.update()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor(DARK.raised))
        w, h = self.width(), self.height()
        half = QRectF(4, 16, w / 2 - 8, h - 22)
        right = QRectF(w / 2 + 4, 16, w / 2 - 8, h - 22)
        for points, colour in ((self.pitch, DARK.text_dim), (self.eg, DARK.accent)):
            if len(points) < 2:
                continue
            end = max(p[0] for p in points) or 1.0
            painter.setPen(QPen(QColor(colour), 1.6))
            for a, b in pairwise(points):
                painter.drawLine(
                    QPointF(
                        half.left() + a[0] / end * half.width(),
                        half.bottom() - a[1] * half.height(),
                    ),
                    QPointF(
                        half.left() + b[0] / end * half.width(),
                        half.bottom() - b[1] * half.height(),
                    ),
                )
        if self.scaling:
            painter.setPen(QPen(QColor(DARK.roles["curve"]), 1.6))
            mid = right.center().y()
            for a, b in pairwise(self.scaling):
                painter.drawLine(
                    QPointF(
                        right.left() + a[0] / 127 * right.width(),
                        mid - a[1] / 24 * right.height() / 2,
                    ),
                    QPointF(
                        right.left() + b[0] / 127 * right.width(),
                        mid - b[1] / 24 * right.height() / 2,
                    ),
                )
        painter.setPen(QColor(DARK.text_dim))
        painter.drawText(QRectF(4, 0, w / 2, 14), 0, f"OP{self.op} EG (pitch EG dim)")
        painter.drawText(QRectF(w / 2 + 4, 0, w / 2, 14), 0, f"OP{self.op} key scaling")
        painter.end()


class Dx7Overview(QWidget):
    def __init__(self, session: Session, channel: int) -> None:
        super().__init__()
        self.session = session
        self.channel = channel
        self.target = channel_target(session, channel)
        layout = QVBoxLayout(self)
        top = QHBoxLayout()
        top.addWidget(QLabel("Name"))
        self.name = QLineEdit()
        self.name.setObjectName("dx7.name")
        self.name.setMaxLength(10)
        self.name.editingFinished.connect(self.commit_name)
        top.addWidget(self.name)
        self.op_box = QComboBox()
        self.op_box.setObjectName("dx7.operator")
        self.op_box.addItems([f"OP{i}" for i in range(1, 7)])
        self.op_box.currentIndexChanged.connect(self._op_chosen)
        top.addWidget(self.op_box)
        layout.addLayout(top)
        self.algorithm = Dx7AlgorithmView(self.target)
        layout.addWidget(self.algorithm)
        self.curves = CurveView(self.target)
        layout.addWidget(self.curves, 1)
        row = QHBoxLayout()
        self.live = QCheckBox("Send to FM-1 while editing")
        self.live.setObjectName("dx7.live")
        self.live.toggled.connect(self._live)
        row.addWidget(self.live)
        export = QPushButton("Export .syx…")
        export.setObjectName("dx7.export")
        export.clicked.connect(self.export_dialog)
        row.addWidget(export)
        save = QPushButton("Save preset…")
        save.setObjectName("dx7.save_preset")
        save.clicked.connect(self.save_preset_dialog)
        row.addWidget(save)
        convert = QPushButton("Convert to FM (approximate)")
        convert.setObjectName("dx7.convert")
        convert.clicked.connect(self.convert)
        row.addWidget(convert)
        layout.addLayout(row)
        self.report = QLabel("")
        self.report.setObjectName("dx7.report")
        self.report.setWordWrap(True)
        layout.addWidget(self.report)
        self.refresh()

    def voice(self) -> list[int]:
        return dx7.channel_voice(self.session, self.channel)

    def refresh(self) -> None:
        if not self.name.hasFocus():
            self.name.setText(dx7.name_of(self.voice()).rstrip())
        self.algorithm.refresh()
        self.curves.refresh()

    def _op_chosen(self, index: int) -> None:
        self.curves.op = index + 1
        self.curves.refresh()

    def commit_name(self) -> None:
        """The name's ten bytes, as ten parameter turns in one undo step."""
        named = dx7.with_name(self.voice(), self.name.text()[:10])
        with self.session.group("Rename voice"):
            for i in range(dx7.NAME_BYTES):
                self.target.turn(f"name.{i + 1}", float(named[dx7.NAME_OFFSET + i]))

    def _live(self, on: bool) -> None:
        for hook in LIVE_EDIT_HOOK:
            hook(self.channel, on)

    def export(self, path: str) -> None:
        dx7.write_voice(path, self.voice())

    def export_dialog(self) -> None:
        path, _filter = QFileDialog.getSaveFileName(self, "Export DX7 voice", "", "SysEx (*.syx)")
        if path:
            self.export(path)

    def save_preset(self, path: str, name: str = "") -> None:
        """The voice as an .adxpreset; the preset browser lists it with the others."""
        voice = self.voice()
        self.session.require().save_channel_preset(
            self.channel, path, name or dx7.name_of(voice).strip() or "DX7", "FM-1"
        )

    def save_preset_dialog(self) -> None:
        path, _filter = QFileDialog.getSaveFileName(
            self,
            "Save dx7 preset",
            f"{dx7.name_of(self.voice()).strip()}.adxpreset",
            "adX preset (*.adxpreset)",
        )
        if path:
            self.save_preset(path)

    def convert(self) -> list[str]:
        name, report = dx7.convert_channel_to_fm(self.session, self.channel)
        lines = report or ["nothing was lost"]
        self.report.setText(f"{name}: " + "; ".join(lines))
        return report


def dx7_editor(session: Session, channel: int) -> TabbedEditor:
    """The overview, and every parameter."""
    return TabbedEditor(Dx7Overview(session, channel), generic_for(session, channel, "dx7"))
