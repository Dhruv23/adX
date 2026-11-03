"""The clip editor (phase_8.md 3.2, 3.3): one audio clip's waveform, fades, stretch and
pitch, its destructive edits and its warp markers.

Every control is one SetPlaylistItem on the selected clip, so each change is an undo
step and the text file shows it. "Destructive" edits append to the clip's edit list:
the source file is never touched, and removing the edit (or undo) gets the original
back.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from adx.engine_bridge import PPQ, detect_onsets, estimate_bpm
from adx.panels.base import DockPanel
from adx.panels.waveform.widget import WaveformView
from adx.selection import Kind, Selection
from adx.session import Session

CURVES = ("linear", "exponential", "logarithmic", "smooth")
#: Destructive edits: (label, edit kind, amount).
EDITS = (
    ("Reverse", "reverse", 0.0),
    ("Normalize", "normalize", -0.3),
    ("Remove DC", "dc", 0.0),
    ("+3 dB", "gain", 3.0),
    ("-3 dB", "gain", -3.0),
    ("Trim silence", "trim", -60.0),
)


class ClipEditorPanel(DockPanel):
    """The selected audio clip. Shows nothing when the selection is not one audio clip."""

    panel_id = "clip_editor"
    title = "Clip editor"
    default_area = Qt.DockWidgetArea.BottomDockWidgetArea
    scrolls = True

    def __init__(
        self, session: Session, selection: Selection, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.selection = selection
        #: (track, item) of the clip shown, or None.
        self.target: tuple[int, int] | None = None
        self.info: dict[str, Any] = {}
        self.file = ""
        self._loading = False

        body = QWidget()
        layout = QVBoxLayout(body)
        self.heading = QLabel("Select an audio clip on the playlist")
        layout.addWidget(self.heading)
        self.waveform = WaveformView()
        self.waveform.setMinimumHeight(120)
        layout.addWidget(self.waveform, 1)

        row = QHBoxLayout()
        layout.addLayout(row)
        row.addWidget(self._clip_box())
        row.addWidget(self._edit_box())
        row.addWidget(self._warp_box())
        self.setWidget(body)
        selection.changed.connect(self.refresh)
        session.changed.connect(self.refresh)
        self._set_enabled(False)

    # --- construction ---------------------------------------------------------------

    def _clip_box(self) -> QGroupBox:
        box = QGroupBox("Clip")
        form = QFormLayout(box)
        self.fade_in = self._ticks_box("fade_in")
        self.fade_out = self._ticks_box("fade_out")
        self.fade_in_curve = self._curve_box("fade_in_curve")
        self.fade_out_curve = self._curve_box("fade_out_curve")
        form.addRow("Fade in", self._pair(self.fade_in, self.fade_in_curve))
        form.addRow("Fade out", self._pair(self.fade_out, self.fade_out_curve))
        self.crossfade = QComboBox()
        self.crossfade.addItem("Equal power", "equal")
        self.crossfade.addItem("Linear", "linear")
        self.crossfade.setToolTip("How this clip crossfades with one it overlaps on its lane")
        self.crossfade.currentIndexChanged.connect(
            lambda: self._set(crossfade=str(self.crossfade.currentData()))
        )
        form.addRow("Crossfade", self.crossfade)
        self.stretch = QDoubleSpinBox()
        self.stretch.setRange(0.05, 20.0)
        self.stretch.setDecimals(3)
        self.stretch.setSingleStep(0.05)
        self.stretch.setSuffix("x")
        self.stretch.setToolTip("Time stretch, pitch unchanged (RubberBand, offline)")
        self.stretch.editingFinished.connect(lambda: self._set(stretch=self.stretch.value()))
        form.addRow("Stretch", self.stretch)
        self.pitch = QDoubleSpinBox()
        self.pitch.setRange(-24.0, 24.0)
        self.pitch.setSingleStep(1.0)
        self.pitch.setSuffix(" st")
        self.pitch.setToolTip("Pitch shift, length unchanged")
        self.pitch.editingFinished.connect(lambda: self._set(pitch=self.pitch.value()))
        form.addRow("Pitch", self.pitch)
        self.reverse = QCheckBox("Play reversed")
        self.reverse.toggled.connect(lambda on: self._set(reverse=1.0 if on else 0.0))
        form.addRow("", self.reverse)
        return box

    def _edit_box(self) -> QGroupBox:
        box = QGroupBox("Edits")
        layout = QVBoxLayout(box)
        self.edits = QListWidget()
        self.edits.setToolTip("Applied in order to a copy of the audio; the file is untouched")
        layout.addWidget(self.edits)
        grid = QHBoxLayout()
        self.edit_buttons: list[QPushButton] = []
        for label, kind, amount in EDITS:
            button = QPushButton(label)
            button.clicked.connect(lambda _=False, k=kind, a=amount: self.add_edit(k, a))
            grid.addWidget(button)
            self.edit_buttons.append(button)
        layout.addLayout(grid)
        self.remove_edit = QPushButton("Remove last edit")
        self.remove_edit.clicked.connect(self.pop_edit)
        layout.addWidget(self.remove_edit)
        return box

    def _warp_box(self) -> QGroupBox:
        box = QGroupBox("Warp")
        layout = QVBoxLayout(box)
        self.bpm = QLabel("")
        layout.addWidget(self.bpm)
        self.warps = QListWidget()
        self.warps.setToolTip("Source seconds -> clip position. Two or more warp the clip")
        layout.addWidget(self.warps)
        detect = QPushButton("Detect from onsets")
        detect.setToolTip("Seed markers at the clip's transients, at its current timing")
        detect.clicked.connect(self.detect_warps)
        layout.addWidget(detect)
        clear = QPushButton("Clear markers")
        clear.clicked.connect(lambda: self._set(warps=[]))
        layout.addWidget(clear)
        self.warp_buttons = [detect, clear]
        return box

    def _ticks_box(self, key: str) -> QSpinBox:
        box = QSpinBox()
        box.setRange(0, PPQ * 64)
        box.setSingleStep(PPQ // 4)
        box.setSuffix(" ticks")
        box.setToolTip(f"{PPQ} ticks to a beat")
        box.editingFinished.connect(lambda: self._set(**{key: float(box.value())}))
        return box

    def _curve_box(self, key: str) -> QComboBox:
        box = QComboBox()
        box.addItems(CURVES)
        box.currentTextChanged.connect(lambda text: self._set(**{key: text}))
        return box

    @staticmethod
    def _pair(left: QWidget, right: QWidget) -> QWidget:
        holder = QWidget()
        row = QHBoxLayout(holder)
        row.setContentsMargins(0, 0, 0, 0)
        row.addWidget(left)
        row.addWidget(right)
        return holder

    # --- the clip -------------------------------------------------------------------

    def _set_enabled(self, on: bool) -> None:
        for widget in (
            self.fade_in,
            self.fade_out,
            self.fade_in_curve,
            self.fade_out_curve,
            self.crossfade,
            self.stretch,
            self.pitch,
            self.reverse,
            self.edits,
            self.remove_edit,
            self.warps,
            *self.edit_buttons,
            *self.warp_buttons,
        ):
            widget.setEnabled(on)

    def _selected_clip(self) -> tuple[int, int] | None:
        project = self.session.project
        ids = self.selection.ids(Kind.CLIPS)
        if project is None or len(ids) != 1:
            return None
        items = project.playlist_items()
        for row in items:
            if int(row["item"]) == ids[0] and int(row["kind"]) == 1:
                return int(row["track"]), int(row["item"])
        return None

    def refresh(self) -> None:
        """Show the selected audio clip, or nothing."""
        project = self.session.project
        target = self._selected_clip()
        info = project.audio_item(*target) if project is not None and target else None
        if target is None or info is None or project is None:
            self.target, self.info = None, {}
            self.heading.setText("Select an audio clip on the playlist")
            self._set_enabled(False)
            return
        first = target != self.target
        self.target, self.info = target, info
        paths = {sample: resolved for sample, _written, resolved in project.samples()}
        self.file = paths.get(int(info["sample"]), "")
        self.heading.setText(
            f"{Path(self.file).name or '(missing file)'} - item {target[1]}, lane {info['lane']}"
        )
        self._loading = True
        try:
            self.fade_in.setValue(int(info["fade_in"]))
            self.fade_out.setValue(int(info["fade_out"]))
            self._select_curve(self.fade_in_curve, str(info["fade_in_curve"]))
            self._select_curve(self.fade_out_curve, str(info["fade_out_curve"]))
            self.crossfade.setCurrentIndex(0 if info["crossfade"] == "equal" else 1)
            self.stretch.setValue(float(info["stretch"]))
            self.pitch.setValue(float(info["pitch"]))
            self.reverse.setChecked(bool(info["reverse"]))
            self.edits.clear()
            for kind, amount in info["edits"]:
                self.edits.addItem(
                    f"{kind} {amount:g}" if kind in ("normalize", "gain", "trim") else kind
                )
            self.warps.clear()
            for source, at in info["warps"]:
                self.warps.addItem(f"{source:.3f} s -> {at / PPQ:.3f} beats")
        finally:
            self._loading = False
        self._set_enabled(True)
        if first and self.file and Path(self.file).is_file():
            self.waveform.open(self.file)
            bpm = estimate_bpm(self.file)
            self.bpm.setText(f"Detected tempo: {bpm:.1f} bpm" if bpm else "No steady tempo found")

    @staticmethod
    def _select_curve(box: QComboBox, curve: str) -> None:
        name = curve.split("(", 1)[0]
        if box.findText(name) < 0:
            box.addItem(name)
        box.setCurrentText(name)

    def _set(self, **fields: object) -> None:
        if self._loading or self.target is None:
            return
        self.session.build_and_execute("SetPlaylistItem", *self.target, fields)

    def add_edit(self, kind: str, amount: float) -> None:
        """Append one destructive edit: a step, undone like any other."""
        edits = [(str(k), float(a)) for k, a in self.info.get("edits", [])]
        self._set(edits=[*edits, (kind, amount)])

    def pop_edit(self) -> None:
        """Remove the clip's last edit."""
        edits = [(str(k), float(a)) for k, a in self.info.get("edits", [])]
        if edits:
            self._set(edits=edits[:-1])

    def detect_warps(self) -> None:
        """Markers at the clip's onsets, placed where they play now (no audible change
        until one is moved)."""
        project = self.session.project
        if project is None or self.target is None or not self.file:
            return
        onsets = [float(t) for t in detect_onsets(self.file)]
        if len(onsets) < 2:
            self.session.failed.emit("Warp", "fewer than two onsets found")
            return
        stretch = float(self.info.get("stretch", 1.0)) or 1.0
        bpm = project.bpm_at(0)
        ticks_per_second = PPQ * bpm / 60.0
        warps = [(t, round(t * stretch * ticks_per_second)) for t in onsets]
        self._set(warps=warps)
