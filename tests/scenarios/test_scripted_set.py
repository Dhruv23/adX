"""scenario_scripted_set_20min (phase_11.md 5, 6): the emulated half of "a human has
performed a real set".

adX is launched through adX.cmd with the control channel and opens a set: a session cut
from the material's own patterns (the WannaCry transcription's, or the stand-in's). The
emulated controller (tools/virtual_pad.py, the shipped "adX Virtual Pad" profile) plays a
20-minute set script into a loopMIDI port the app opens as its pad controller: launches
at pad velocities, scenes, stops, stop-alls, follow actions, held tempo nudges from the
transport bar and taps. The performance is recorded, and the master captured.

Passes when: no xrun; no glitch-detector finding on the capture; every light the
controller received matches the clip state it shows, at every settled check; no launch
refused; and the recorded performance, committed into the arrangement and rendered
offline, plays what was heard - the same notes at the same samples (within a sample
where a rate changed; phase_11.md §10).

``ADX_SET_MINUTES`` shortens it for a trial run (default 20).
"""

from __future__ import annotations

import os
import pathlib
import sys
import time

import numpy as np
import pytest
from live_set import build_set, set_script
from wannacry import MATERIAL, ROOT, VIA, project_for

from adx import engine_bridge
from adx.testing.driver import Driver
from adx.testing.glitch import find_glitches
from adx.testing.wav import read_wav

pytestmark = [
    pytest.mark.skipif(sys.platform != "win32", reason="loopMIDI and adX.cmd are Windows"),
    pytest.mark.skipif(bool(os.environ.get("CI")), reason="needs a loopMIDI port (not on CI)"),
]

MINUTES = float(os.environ.get("ADX_SET_MINUTES", "20"))

ATTACH = """
names = [n for n in window.engine.midi_inputs() if "loopMIDI" in n]
if names and names[0] not in window.engine.midi_opened:
    window.engine.open_midi(names[0])
window.attach_pads()
result = {"inputs": names, "pads": sorted(window.pads.attached), "lit": window.pads.lit}
"""

STATES = """
e = window.engine
info = window.session.project.session_info()
result = [[e.clip_state(t, r) if slot is not None else "empty"
           for r, slot in enumerate(track["slots"])] for t, track in enumerate(info["tracks"])]
"""

COUNTS = """
e = window.engine
result = {"counts": e.session_counts, "rate": e.session_rate, "presses": e.pad_presses,
          "feedback": e.pad_feedback, "recording": e.performance_recording}
"""


def envelope(audio: np.ndarray, start: int, frames: int, window: int = 480) -> np.ndarray:
    """RMS per 10 ms window from ``start``: what was heard, in a form the two renders
    can agree on when a voice's start phase differs (a seeded oscillator's phase follows
    its placement's id, and a recording places a new one)."""
    mono = audio[start : start + frames].astype(np.float64).mean(axis=1)
    usable = len(mono) // window * window
    return np.asarray(np.sqrt((mono[:usable].reshape(-1, window) ** 2).mean(axis=1)))


def first_sound(audio: np.ndarray, threshold: float = 1e-4) -> int:
    loud = np.flatnonzero(np.abs(audio).max(axis=1) > threshold)
    return int(loud[0]) if len(loud) else 0


@pytest.mark.long(1800)
@pytest.mark.parametrize("material", MATERIAL)
def test_scripted_set(material: str, evidence: pathlib.Path) -> None:
    if not any("loopmidi" in n.lower() for n in _ports()):
        pytest.skip("needs loopMIDI running with a port")
    sys.path.insert(0, str(ROOT / "tools"))
    from virtual_pad import VirtualPad

    source = project_for(material)
    set_path = evidence / "set.adx"
    info = build_set(source, set_path, scene_tempo=None)
    steps = set_script(MINUTES, info)
    profile = (ROOT / "app" / "adx" / "pads" / "profiles" / "adx_virtual_pad.adxpad").read_text(
        encoding="utf-8"
    )
    pad = VirtualPad(profile, "loopMIDI")
    mismatches: list[str] = []
    checks = 0
    try:
        with Driver.launch(
            via=VIA, settings_dir=evidence / "settings", timeout=180, null_audio=False
        ) as app:
            app.open(set_path)
            attached = app.script(ATTACH)
            assert attached["pads"], attached
            assert attached["lit"], attached
            assert not app.state()["audio"]["null"], "no audio device: the set needs one"
            before = int(app.state()["audio"]["xruns"])
            app.script("window.transport.perform_action.setChecked(True)\nresult = True")
            app.capture_start()
            start = time.perf_counter()
            for step in steps:
                while time.perf_counter() - start < step.at:
                    time.sleep(0.01)
                    pad.read_lights()
                if step.verb == "launch":
                    pad.launch(*step.args)
                elif step.verb == "scene":
                    pad.scene(*step.args)
                elif step.verb == "stop":
                    pad.stop(*step.args)
                elif step.verb == "stopall":
                    pad.stop_all()
                elif step.verb == "tap":
                    pad.tap()
                elif step.verb == "nudge":
                    app.script(f"window.nudge({step.args[0]})\nresult = True")
                    time.sleep(0.8)
                    app.script("window.nudge(0)\nresult = True")
                # Settled? Compare every light with the cell it shows, at quiet moments:
                # twice in a row the same states, so nothing was in flight.
                if step.verb in ("launch", "stop") and checks < 10_000:
                    time.sleep(0.15)
                    first = app.script(STATES)
                    time.sleep(0.15)
                    states = app.script(STATES)
                    if states != first:
                        continue
                    time.sleep(0.1)
                    pad.read_lights()
                    # A clip queued near its boundary can move on while the lights are
                    # read; the lights then show the next state, which is right. Only a
                    # window with no change in it is a check.
                    if app.script(STATES) != states:
                        continue
                    checks += 1
                    for t, column in enumerate(states):
                        for r, state in enumerate(column):
                            shown = pad.light(t, r)
                            want = "queued" if state == "retriggered" else state
                            if shown != want and not (want == "playing" and shown == "recording"):
                                note = pad.notes[r][t]
                                age = time.perf_counter() - pad.when.get(note, 0.0)
                                time.sleep(1.0)
                                later = app.script(STATES)[t][r]
                                mismatches.append(
                                    f"{step.at:.1f}s ({t},{r}): {shown} != {want}"
                                    f" (light {age:.2f}s old; 1s later {pad.light(t, r)}"
                                    f" vs {later})"
                                )
            time.sleep(3.0)
            counts = app.script(COUNTS)
            app.script("window.transport.perform_action.setChecked(False)\nresult = True")
            take = app.capture_stop(evidence / "set.wav")
            xruns = int(app.state()["audio"]["xruns"]) - before
            recorded = evidence / "recorded.adx"
            app.script(
                "pathlib = __import__('pathlib')\n"
                f"pathlib.Path(r'{recorded}').write_bytes(window.session.project.dumps()"
                ".encode('utf-8'))\nresult = True"
            )
    finally:
        pad.close()

    assert counts["counts"]["refused"] == 0, counts
    assert counts["presses"] > len(steps) // 2, counts
    assert counts["feedback"]["own_thread"], counts
    assert xruns == 0, f"{xruns} xrun(s)"
    assert checks > MINUTES * 5
    assert not mismatches, f"{len(mismatches)} of {checks} checks: {mismatches[:10]}"
    audio, rate = read_wav(take["path"])
    assert float(np.abs(audio).max()) > 0.01, "the capture is silent"
    assert find_glitches(audio, rate) == []

    # The performance, reopened as an arrangement and rendered, plays what was heard.
    reopened, _diagnostics = engine_bridge.Project.load(str(recorded))
    assert "PATTERN" in recorded.read_text(encoding="utf-8")
    rendered_path = evidence / "rendered.wav"
    engine_bridge.render_to_file(reopened, str(rendered_path), tail_seconds=2.0)
    rendered, rendered_rate = read_wav(rendered_path)
    assert rendered_rate == rate
    # Aligned on their first sound, the two follow one another window by window.
    a, b = first_sound(audio), first_sound(rendered)
    frames = min(len(audio) - a, len(rendered) - b) - rate  # the capture's last second out
    assert frames > rate * 30
    heard, played = envelope(audio, a, frames), envelope(rendered, b, frames)
    correlation = float(np.corrcoef(heard, played)[0, 1])
    loud = heard > 0.01
    level = float(np.median(np.abs(20 * np.log10((played[loud] + 1e-9) / (heard[loud] + 1e-9)))))
    (evidence / "compare.txt").write_text(
        f"correlation {correlation:.4f}, median level difference {level:.2f} dB, "
        f"{frames / rate:.1f} s compared\n",
        encoding="utf-8",
    )
    assert correlation > 0.95, correlation
    assert level < 1.0, level


def _ports() -> list[str]:
    from adx.testing.midi import ports

    try:
        found = ports()
    except RuntimeError:
        return []
    return found["inputs"] + found["outputs"]
