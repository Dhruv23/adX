"""The record toolbar (phase_8.md 3.1): arm, monitor, punch, count-in and the latency
the take will be moved back by.

Arming starts a take on the playlist track the window names; disarming stops it and
places it. The MIDI button records what the keyboard plays into the channel it plays
(STATE.md P9-6): a pattern of notes, bends in their pitch curves, on a new track; the
Auto button writes automation from plugin editors (P9-7). The session does the transport
itself (seek to the pre-roll, play, stop), so the toolbar holds nothing but the session
between the two.
"""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import QTimer, Signal
from PySide6.QtWidgets import QCheckBox, QComboBox, QLabel, QSpinBox, QToolBar, QToolButton, QWidget

from adx.engine_bridge import Engine, RecordSession
from adx.session import Session

#: Where to record and what to punch, asked of the window when a take starts.
TrackProvider = Callable[[], int | None]
RangeProvider = Callable[[], tuple[int, int] | None]


class RecordToolbar(QToolBar):
    """Emits ``message(text)`` for the status bar and ``take(result)`` after each take."""

    message = Signal(str)
    take = Signal(dict)

    def __init__(
        self,
        engine: Engine,
        session: Session,
        track: TrackProvider,
        punch_range: RangeProvider,
        directory: Callable[[], str],
        round_trip: Callable[[], int | None] = lambda: None,
        midi_channel: Callable[[], int] = lambda: 0,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__("Record", parent)
        self.setObjectName("record_toolbar")
        self._engine = engine
        self._session = session
        self._track = track
        self._punch_range = punch_range
        self._directory = directory
        self._round_trip = round_trip
        self.recording: RecordSession | None = None
        self._midi_channel = midi_channel
        #: The tick a MIDI take started at, while one is recording.
        self.midi_origin: int | None = None
        self._drain = QTimer(self)
        self._drain.setInterval(50)
        self._drain.timeout.connect(self._engine.drain_midi_take)

        self.arm = QToolButton()
        self.arm.setText("● Rec")
        self.arm.setCheckable(True)
        self.arm.setToolTip("Record on the selected playlist track (Ctrl+R)")
        self.arm.toggled.connect(self._armed)
        self.addWidget(self.arm)
        self.midi = QToolButton()
        self.midi.setObjectName("record_midi")
        self.midi.setText("● MIDI")
        self.midi.setCheckable(True)
        self.midi.setToolTip(
            "Record the MIDI keyboard into the channel it plays: a new pattern on a new "
            "playlist track, pitch bend kept in each note's pitch curve"
        )
        self.midi.toggled.connect(self._midi_armed)
        self.addWidget(self.midi)
        self.automation = QToolButton()
        self.automation.setObjectName("write_automation")
        self.automation.setText("● Auto")
        self.automation.setCheckable(True)
        self.automation.setToolTip(
            "Write automation: turn a plugin's own controls while playing, and each one "
            "becomes a lane in a new pattern when you turn this off"
        )
        self.automation.toggled.connect(self._automation_armed)
        self.addWidget(self.automation)
        self.monitor = QComboBox()
        self.monitor.addItem("Direct monitoring", "direct")
        self.monitor.addItem("Software monitoring", "software")
        self.monitor.setToolTip(
            "Direct: the interface monitors, adX stays out of it. Software: through the "
            "track's effects, at the cost of the round trip shown on the right"
        )
        self.addWidget(self.monitor)
        self.punch = QCheckBox("Punch")
        self.punch.setToolTip("Record only inside the loop region, rolling in from before it")
        self.addWidget(self.punch)
        self.addWidget(QLabel(" Count-in"))
        self.count_in = QSpinBox()
        self.count_in.setRange(0, 4)
        self.count_in.setSuffix(" bar")
        self.addWidget(self.count_in)
        self.latency = QLabel()
        self.addWidget(self.latency)
        self._show_latency(None)

    def toggle(self) -> None:
        """The menu action: arm or disarm."""
        self.arm.toggle()

    def _show_latency(self, samples: int | None) -> None:
        rate = max(1, self._engine.sample_rate)
        if self._engine.input_channels == 0:
            self.latency.setText("  no input open")
            self.latency.setToolTip("Turn on a recording input in Preferences > Audio")
        elif samples is None:
            self.latency.setText("")
        else:
            self.latency.setText(f"  round trip {samples / rate * 1000.0:.1f} ms")

    def settings(self, track: int) -> dict[str, object]:
        """The session settings the controls say, for ``track``."""
        settings: dict[str, object] = {
            "track": track,
            "monitor": str(self.monitor.currentData()),
            "count_in_bars": self.count_in.value(),
            "project_directory": self._directory(),
        }
        measured = self._round_trip()
        if measured is not None:
            settings["round_trip"] = measured
        if self.punch.isChecked():
            region = self._punch_range()
            if region is not None:
                settings["punch_in"], settings["punch_out"] = region
        return settings

    def _automation_armed(self, on: bool) -> None:
        project = self._session.project
        if on:
            if project is None:
                self.message.emit("Write automation: no project open")
                self.automation.blockSignals(True)
                self.automation.setChecked(False)
                self.automation.blockSignals(False)
                return
            self._engine.write_automation(True)
            self.message.emit("Writing automation: move a plugin's controls while it plays")
            return
        name = self._engine.write_automation(False, project)
        self._session.notify()
        self.message.emit(
            f"Automation written: {name}" if name else "Write automation: nothing moved"
        )

    def _midi_armed(self, on: bool) -> None:
        if on:
            self._start_midi()
        else:
            self._stop_midi()

    def _start_midi(self) -> None:
        if self._session.project is None or self._midi_channel() == 0:
            self.message.emit("Record MIDI: select the channel the keyboard should play")
            self.midi.blockSignals(True)
            self.midi.setChecked(False)
            self.midi.blockSignals(False)
            return
        self.midi_origin = self._engine.position_ticks()
        self._engine.record_midi(True)
        self._drain.start()
        if self._engine.state() != "playing":
            self._engine.play()
        self.message.emit("Recording MIDI…")

    def _stop_midi(self) -> None:
        if self.midi_origin is None:
            return
        stop = self._engine.position_ticks()
        self._engine.record_midi(False)
        self._drain.stop()
        project = self._session.project
        origin, self.midi_origin = self.midi_origin, None
        if project is None:
            return
        name = self._engine.commit_midi_take(project, self._midi_channel(), origin, stop)
        self._session.notify()
        self.message.emit(
            f"MIDI take recorded: {name}" if name else "Record MIDI: nothing was played"
        )

    def _armed(self, on: bool) -> None:
        if on:
            self._start()
        else:
            self._stop()

    def _refuse(self, why: str) -> None:
        self.message.emit(why)
        self.arm.blockSignals(True)
        self.arm.setChecked(False)
        self.arm.blockSignals(False)

    def _start(self) -> None:
        project = self._session.project
        if project is None:
            self._refuse("Record: no project open")
            return
        if self._engine.input_channels == 0:
            self._refuse("Record: no input is open - turn one on in Preferences > Audio")
            return
        track = self._track()
        if track is None:
            self._refuse("Record: select a playlist track to record on")
            return
        try:
            self.recording = RecordSession(self._engine, project, self.settings(track))
            self.recording.start()
        except (RuntimeError, ValueError) as error:
            self.recording = None
            self._refuse(f"Record: {error}")
            return
        self._show_latency(self.recording.latency["compensation"])
        self.message.emit("Recording…")

    def _stop(self) -> None:
        if self.recording is None:
            return
        result = self.recording.stop()
        self.recording = None
        self._session.notify()
        if not result["ok"]:
            self.message.emit(f"Record: {result['error']}")
        elif result["overran"]:
            self.message.emit("Take kept, but the disk fell behind and it ended early")
        else:
            seconds = int(result["frames"]) / max(1, self._engine.sample_rate)
            self.message.emit(f"Take recorded: {seconds:.1f} s on lane {result['lane']}")
        self.take.emit(result)
