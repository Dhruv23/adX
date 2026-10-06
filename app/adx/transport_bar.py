"""The transport bar: play, stop, record, loop, tempo, position, metronome.

Every button is one engine call; the position display reads the frame clock (one engine
call a frame, shared with the meters and the playhead) and never calls the engine
itself.

Record is Phase 8's (plans/phase_8.md). The metronome needs a click source in the
engine, which no phase has built yet; the button is here, disabled, and says so -
open issue P5-3.
"""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import Signal
from PySide6.QtGui import QAction
from PySide6.QtWidgets import QDoubleSpinBox, QLabel, QToolBar, QWidget

from adx.engine_bridge import PPQ


def format_position(ticks: int, numerator: int, denominator: int, seconds: float) -> str:
    """``bar:beat:tick`` (1-based bar and beat) and ``m:ss.mmm``."""
    beat = PPQ * 4 // max(1, denominator)
    bar = beat * max(1, numerator)
    bars, rest = divmod(max(0, ticks), bar)
    beats, sub = divmod(rest, beat)
    minutes, secs = divmod(max(0.0, seconds), 60.0)
    return f"{bars + 1:>3}:{beats + 1}:{sub:04d}   {int(minutes)}:{secs:06.3f}"


class TransportBar(QToolBar):
    """Emits requests; the main window turns them into engine calls and commands."""

    play_requested = Signal()
    stop_requested = Signal()
    loop_toggled = Signal(bool)
    tempo_changed = Signal(float)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__("Transport", parent)
        self.setObjectName("transport")
        self.play_action = QAction("▶ Play", self)
        self.play_action.setShortcut("Space")
        self.play_action.setToolTip("Play / pause (Space)")
        self.play_action.triggered.connect(self.play_requested)
        self.stop_action = QAction("■ Stop", self)
        self.stop_action.setToolTip("Stop and return to the start")
        self.stop_action.triggered.connect(self.stop_requested)
        self.record_action = QAction("● Rec", self)
        self.record_action.setEnabled(False)
        self.record_action.setToolTip("Recording arrives in Phase 8 (plans/phase_8.md)")
        self.loop_action = QAction("⟲ Loop", self, checkable=True)
        self.loop_action.setToolTip("Loop the arrangement")
        self.loop_action.toggled.connect(self.loop_toggled)
        self.metronome_action = QAction("Metronome", self, checkable=True)
        self.metronome_action.setEnabled(False)
        self.metronome_action.setToolTip("Needs a click source in the engine: open issue P5-3")
        for action in (self.play_action, self.stop_action, self.record_action, self.loop_action):
            self.addAction(action)
        self.addSeparator()
        self.tempo = QDoubleSpinBox()
        self.tempo.setRange(1.0, 999.0)
        self.tempo.setDecimals(2)
        self.tempo.setSuffix(" bpm")
        self.tempo.setKeyboardTracking(False)
        self.tempo.valueChanged.connect(self._tempo_edited)
        self.addWidget(self.tempo)
        self.addAction(self.metronome_action)
        self.addSeparator()
        self.position = QLabel(format_position(0, 4, 4, 0.0))
        self.position.setObjectName("position")
        self.addWidget(self.position)
        self._quiet = False
        self.seconds_at: Callable[[int], float] | None = None
        self.meter = (4, 4)

    def _tempo_edited(self, bpm: float) -> None:
        if not self._quiet:
            self.tempo_changed.emit(bpm)

    def set_tempo(self, bpm: float) -> None:
        """Show a tempo without emitting a change."""
        self._quiet = True
        self.tempo.setValue(bpm)
        self._quiet = False

    def set_playing(self, playing: bool) -> None:
        """Flip the play button's label."""
        self.play_action.setText("❚❚ Pause" if playing else "▶ Play")

    def show_position(self, ticks: int, seconds: float) -> None:
        """The frame clock's position, formatted."""
        self.position.setText(format_position(ticks, self.meter[0], self.meter[1], seconds))
