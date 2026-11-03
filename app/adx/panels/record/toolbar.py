"""The record toolbar (phase_8.md 3.1): arm, monitor, punch, count-in and the latency
the take will be moved back by.

Arming starts a take on the playlist track the window names; disarming stops it and
places it. The session does the transport itself (seek to the pre-roll, play, stop), so
the toolbar holds nothing but the session between the two.
"""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import Signal
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

        self.arm = QToolButton()
        self.arm.setText("● Rec")
        self.arm.setCheckable(True)
        self.arm.setToolTip("Record on the selected playlist track (Ctrl+R)")
        self.arm.toggled.connect(self._armed)
        self.addWidget(self.arm)
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
