"""Live MIDI in the window (phase_9.md 4.1): the inputs the preferences name are opened
at start, and whatever channel is selected is the one a keyboard plays - as in a
channel rack, where the selected row is the one under your fingers.

The notes themselves never pass through Python: the device thread timestamps them into
the engine's ring and the audio thread places them sample-accurately (MidiInput.h).
"""

from __future__ import annotations

from collections.abc import Callable

from adx.engine_bridge import Engine
from adx.selection import Kind, Selection
from adx.session import Session


class LiveMidi:
    """Opens inputs and keeps the engine's MIDI target on the selected channel."""

    def __init__(
        self,
        engine: Engine,
        session: Session,
        selection: Selection,
        report: Callable[[str], None] = lambda _message: None,
    ) -> None:
        self.engine = engine
        self.session = session
        self.selection = selection
        self.report = report
        #: The channel live input plays, or 0.
        self.target = 0
        selection.changed.connect(self.follow)
        session.changed.connect(self.follow)

    def available(self) -> list[str]:
        """The system's MIDI inputs; empty when the engine has no MIDI service (or none)."""
        try:
            return self.engine.midi_inputs()
        except RuntimeError:
            return []

    def apply(self, wanted: list[str]) -> list[str]:
        """Open exactly the inputs in ``wanted`` that exist; return the ones that failed."""
        failed: list[str] = []
        try:
            opened = set(self.engine.midi_opened)
            for name in opened - set(wanted):
                self.engine.close_midi(name)
            for name in wanted:
                if name in opened:
                    continue
                try:
                    self.engine.open_midi(name)
                except RuntimeError as error:
                    failed.append(name)
                    self.report(f"MIDI: {error}")
        except RuntimeError as error:
            self.report(f"MIDI: {error}")
            return list(wanted)
        self.follow()
        return failed

    def follow(self) -> None:
        """Point live input at the selected channel; with none selected, keep the last
        one if it still exists, else the first channel."""
        project = self.session.project
        if project is None:
            self._set(0)
            return
        channels = [int(c["id"]) for c in project.channels_info()]
        selected = self.selection.ids(Kind.CHANNELS)
        if selected:
            self._set(int(selected[0]))
        elif self.target not in channels:
            self._set(channels[0] if channels else 0)

    def _set(self, channel: int) -> None:
        if channel != self.target:
            self.target = channel
            self.engine.set_midi_target(channel)
