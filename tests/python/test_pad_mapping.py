"""Pad controllers from Python (phase_11.md 5): the shipped profiles, learn mode, the pad
controllers object, and - where loopMIDI runs - virtual_pad_drives_a_set: the emulated
controller launches, stops and fires scenes, and the lights it receives match the clip
states."""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import pytest

import adx_engine
from adx import pads
from adx.engine_bridge import Engine, Project

ROOT = Path(__file__).resolve().parents[2]

SET = """[PROJECT]
ADX_VERSION=2

[TEMPO]
0:0:0 120

[CHANNEL Lead]
INSTRUMENT=testtone
OUTPUT=insert.1

[CHANNEL Bass]
INSTRUMENT=testtone
OUTPUT=insert.1

[PATTERN A]
LENGTH=1:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100

[PATTERN B]
LENGTH=1:0:0
NOTES Bass
  C2 0:0:0 0:1:0 100

[PLAYLIST]
TRACK 1 name="Lead"
TRACK 2 name="Bass"

[MIXER]
INSERT 1 name="Master"

[SESSION]
QUANTIZE=1/4
SCENE "One"
SCENE "Two"
TRACK Lead linked=1
  1 A
  2 A
TRACK Bass linked=2
  1 B
"""


def test_shipped_profiles() -> None:
    found = pads.profiles()
    names = {p.name for p in found}
    assert {"Launchpad Mini MK3", "APC mini", "adX Virtual Pad"} <= names
    for profile in found:
        assert profile.warnings == [], (profile.name, profile.warnings)
        # 8 x 8 grids, but for the FM-1's 27 keys (8 x 3, phase_11.5.md 4.10).
        assert profile.columns * profile.rows == (24 if profile.name == "FM-1 keys" else 64)
    assert pads.match("LPMiniMK3 MIDI 1") is not None
    assert pads.match("loopMIDI Port 0").name == "adX Virtual Pad"  # type: ignore[union-attr]
    assert pads.match("Some keyboard") is None
    parsed = adx_engine.parse_pad_profile(found[0].text)
    assert adx_engine.parse_pad_profile(parsed["text"])["profile"] == parsed["profile"]


def test_user_profile_overrides(tmp_path: Path) -> None:
    learn = pads.Learn("My pads", 2, 1, match_device="MyPads")
    for number in (40, 41):
        assert learn.press({"kind": "note", "channel": 1, "number": number, "value": 0.8})
    assert learn.prompt == "scene 1"
    learn.skip()  # no scene buttons
    assert learn.press({"kind": "cc", "channel": 1, "number": 9, "value": 1.0})
    learn.skip()
    assert learn.done
    path = pads.save_learned(learn.result(), tmp_path)
    assert path.parent == tmp_path / "pads"
    mine = pads.match("MyPads 2", tmp_path)
    assert mine is not None
    assert not mine.shipped
    info = adx_engine.parse_pad_profile(mine.text)["profile"]
    assert info["notes"] == [[40, 41]]
    assert info["stop_all"] == {"cc": True, "numbers": [9]}


def test_pad_messages() -> None:
    text = (pads.SHIPPED / "adx_virtual_pad.adxpad").read_text(encoding="utf-8")
    assert adx_engine.pad_message(text, 3, "playing") == bytes([0x9F, 3, 3])
    assert adx_engine.pad_message(text, 3, "empty") == bytes([0x9F, 3, 0])
    lp = (pads.SHIPPED / "launchpad_mini_mk3.adxpad").read_text(encoding="utf-8")
    assert adx_engine.pad_message(lp, 81, "queued", lit=False) == bytes([0x90, 81, 0])


def _loopmidi() -> tuple[str, str] | None:
    if os.environ.get("CI") or sys.platform != "win32":
        return None
    engine = Engine(null_backend=True)
    engine.start()
    try:
        ins = [n for n in engine.midi_inputs() if "loopMIDI" in n]
        outs = [n for n in engine.midi_outputs() if "loopMIDI" in n]
    finally:
        engine.stop()
    return (ins[0], outs[0]) if ins and outs else None


def test_virtual_pad_drives_a_set() -> None:
    found = _loopmidi()
    if found is None:
        pytest.skip("needs loopMIDI running with a port (not on CI)")
    port_in, _port_out = found
    sys.path.insert(0, str(ROOT / "tools"))
    from virtual_pad import VirtualPad

    project, _diagnostics = Project.loads(SET)
    engine = Engine(null_backend=True)
    engine.set_project(project)
    engine.start()
    controllers = pads.PadControllers(engine)
    pad = None
    try:
        profile_text = (pads.SHIPPED / "adx_virtual_pad.adxpad").read_text(encoding="utf-8")
        pad = VirtualPad(profile_text, "loopMIDI")  # listening before the first lights
        engine.open_midi(port_in)
        controllers.set_cells(project.session_info())
        profile = controllers.attach(port_in)
        assert profile is not None
        assert profile.name == "adX Virtual Pad"
        assert controllers.lit is not None
        time.sleep(0.4)

        def wait_for(track: int, row: int, state: str, timeout: float = 3.0) -> None:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if engine.clip_state(track, row) == state and pad.light(track, row) == state:
                    return
                time.sleep(0.02)
            raise AssertionError(
                f"({track}, {row}): engine {engine.clip_state(track, row)}, "
                f"light {pad.light(track, row)}, wanted {state}"
            )

        pad.launch(0, 1, 90)
        wait_for(0, 1, "playing")
        pad.scene(0)
        wait_for(0, 0, "playing")
        wait_for(1, 0, "playing")
        assert pad.light(1, 1) == "empty"
        pad.stop(1)
        wait_for(1, 0, "stopped")
        pad.stop_all()
        wait_for(0, 0, "stopped")
        assert engine.pad_presses >= 4
        feedback = engine.pad_feedback
        assert feedback["running"]
        assert feedback["own_thread"]
        assert feedback["ticks"] > 0
        # Every light the pad received matches its cell's state now.
        for track in (0, 1):
            for row in (0, 1):
                filled = project.session_info()["tracks"][track]["slots"][row] is not None
                expected = engine.clip_state(track, row) if filled else "empty"
                assert pad.light(track, row) == expected
    finally:
        if pad is not None:
            pad.close()
        controllers.stop()
        engine.stop()
