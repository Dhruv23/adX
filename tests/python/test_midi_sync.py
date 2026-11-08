"""MIDI out, clock and time code (STATE.md P9-5): the preferences page, LiveMidi opening
outputs, and - where loopMIDI is running - one engine's clock followed by another's
through a real MIDI port."""

from __future__ import annotations

import os
import time

import pytest

from adx.engine_bridge import PPQ, Engine, Project


def test_midi_page_outputs_and_sync(qapp: object) -> None:
    from adx.panels.preferences.pages.midi import MidiPage
    from adx.preferences import Preferences

    prefs = Preferences(midi_outputs=["Old synth"], midi_send_clock=True, sync_follow="mtc")
    page = MidiPage(prefs, ["Keys"], ["loopMIDI Port", "Synth"])
    assert list(page.out_boxes) == ["loopMIDI Port", "Synth", "Old synth"]
    assert "not connected" in page.out_boxes["Old synth"].text()
    assert page.send_clock.isChecked()
    assert not page.send_mtc.isChecked()
    assert page.follow.currentData() == "mtc"
    page.out_boxes["Synth"].setChecked(True)
    page.out_boxes["Old synth"].setChecked(False)
    page.send_mtc.setChecked(True)
    page.follow.setCurrentIndex(page.follow.findData("clock"))
    page.apply()
    assert prefs.midi_outputs == ["Synth"]
    assert prefs.midi_send_mtc
    assert prefs.sync_follow == "clock"
    page.deleteLater()


class _FakeEngine:
    def __init__(self) -> None:
        self.opened: list[str] = ["Gone"]
        self.calls: list[tuple[object, ...]] = []

    @property
    def midi_outputs_opened(self) -> list[str]:
        return list(self.opened)

    def open_midi_output(self, name: str) -> None:
        if name == "Broken":
            raise RuntimeError("no MIDI output named Broken")
        self.opened.append(name)

    def close_midi_output(self, name: str) -> None:
        self.opened.remove(name)

    def set_midi_sync(self, name: str, clock: bool, mtc: bool) -> None:
        self.calls.append(("sync", name, clock, mtc))

    def set_sync_follow(self, source: str) -> None:
        self.calls.append(("follow", source))

    def send_midi(self, name: str, message: list[int]) -> None:
        if name not in self.opened:
            raise RuntimeError(f"not open: {name}")
        self.calls.append(("send", name, list(message)))

    def set_midi_target(self, channel: int) -> None:
        pass


def test_live_midi_applies_outputs() -> None:
    from adx.live_midi import LiveMidi
    from adx.selection import Selection
    from adx.session import Session

    engine = _FakeEngine()
    reports: list[str] = []
    session = Session(engine)  # type: ignore[arg-type]
    live = LiveMidi(engine, session, Selection(), reports.append)  # type: ignore[arg-type]
    failed = live.apply_outputs(["Synth", "Broken"], clock=True, mtc=False, follow="clock")
    assert failed == ["Broken"]
    assert engine.opened == ["Synth"]  # "Gone" closed
    assert ("sync", "Synth", True, False) in engine.calls
    assert ("follow", "clock") in engine.calls
    assert reports
    assert "Broken" in reports[0]
    assert live.send("Synth", [0x90, 60, 100])
    assert ("send", "Synth", [0x90, 60, 100]) in engine.calls
    assert not live.send("Nope", [0x90, 60, 100])


def _project(bpm: float) -> Project:
    text = (
        f"[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 {bpm:g}\n\n"
        '[CHANNEL Probe]\nINSTRUMENT=testtone\n\n[MIXER]\nINSERT 1 name="Master"\n'
    )
    project, _ = Project.loads(text)
    return project


def _base(name: str) -> str:
    """RtMidi appends the port index, which differs between a port's two directions."""
    head, _, tail = name.rpartition(" ")
    return head if tail.isdigit() else name


def _loopmidi_port() -> tuple[str, str] | None:
    """A loopMIDI port as (output name, input name)."""
    if os.environ.get("CI"):
        return None
    engine = Engine(null_backend=True)
    engine.start()
    try:
        outs = [n for n in engine.midi_outputs() if "loopMIDI" in n]
        ins = [n for n in engine.midi_inputs() if "loopMIDI" in n]
    finally:
        engine.stop()
    for out in outs:
        for name in ins:
            if _base(out) == _base(name):
                return out, name
    return None


def test_clock_followed_through_loopmidi() -> None:
    """Engine A plays at 120 bpm and sends clock into loopMIDI; engine B, whose project
    says 100, follows it: B starts, and runs 1.2 times as fast."""
    found = _loopmidi_port()
    if found is None:
        pytest.skip("needs loopMIDI running with a port (not on CI)")
    port, port_in = found
    sender = Engine(null_backend=True)
    follower = Engine(null_backend=True)
    a, b = _project(120.0), _project(100.0)
    sender.set_project(a)
    follower.set_project(b)
    sender.start()
    follower.start()
    try:
        follower.open_midi(port_in)
        follower.set_sync_follow("clock")
        sender.open_midi_output(port)
        sender.set_midi_sync(port, clock=True, mtc=False)
        time.sleep(0.3)
        sender.play()
        deadline = time.monotonic() + 5.0
        while follower.state() != "playing" and time.monotonic() < deadline:
            time.sleep(0.02)
        assert follower.state() == "playing"
        time.sleep(4.0)
        theirs = sender.position_ticks()
        ours = follower.position_ticks()
        # Within a quarter of a beat of the sender after four seconds.
        assert abs(theirs - ours) < PPQ // 4, (theirs, ours)
        assert sender.midi_sync_counts["sent"] > 24 * 4
        sender.stop_playback()
        deadline = time.monotonic() + 2.0
        while follower.state() == "playing" and time.monotonic() < deadline:
            time.sleep(0.02)
        assert follower.state() != "playing"
    finally:
        follower.set_sync_follow("none")
        sender.stop()
        follower.stop()
