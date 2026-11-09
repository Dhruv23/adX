"""dx7_offline_equals_live (phase_11.5.md 5): a project playing dx7 voices renders the same
offline as through a live engine's callbacks, sample for sample.

The live side is a loopback engine (no device, driven block by block as a device would
drive it, at 256 frames); the offline side is render_to_file. The dx7 instrument's own
grid restarts at each note that wakes it, so where a buffer boundary falls cannot move
its output (test_dx7_instrument.cpp's block-size test, through the whole engine here).
"""

from __future__ import annotations

import pathlib
import time

import numpy as np

from adx import engine_bridge
from adx.engine_bridge import Engine, Project
from adx.hardware import dx7
from adx.testing import glitch
from adx.testing.wav import read_wav

FIXTURES = pathlib.Path(__file__).resolve().parents[1] / "fixtures" / "dx7"
NOTES = ("C4", "E4", "G4", "B3", "D5", "A3")


def project_text(fm1_rate: bool) -> str:
    voice = dx7.read_file(FIXTURES / "random.syx")["voices"][2]
    values = engine_bridge.dx7_voice_to_params(voice)
    names = dx7.param_names()
    params = "\n".join(f"PARAM {names[i]}={values[i]:g}" for i in range(156))
    notes = "\n".join(
        f"  {pitch} {bar}:{beat}:0 0:2:0 {90 + 5 * k}"
        for k, (pitch, (bar, beat)) in enumerate(
            zip(NOTES, [(0, 0), (0, 1), (0, 2), (1, 0), (1, 1), (2, 3)], strict=True)
        )
    )
    return f"""[PROJECT]
ADX_VERSION=2
BPM=120

[CHANNEL Keys]
INSTRUMENT=dx7
OUTPUT=insert.1
{params}
PARAM fm1.rate={1 if fm1_rate else 0}

[PATTERN Riff]
LENGTH=4:0:0
NOTES Keys
{notes}

[PLAYLIST]
TRACK 1
  PATTERN Riff 0:0:0

[MIXER]
INSERT 1 name="Master"
"""


def live(project: Project, seconds: float) -> np.ndarray:
    engine = Engine(loopback=0)
    engine.set_project(project)
    engine.start()
    try:
        engine.capture_start()
        engine.play()
        rendered = 0
        for block in range(int(seconds * 48000 / 256)):
            engine.render_offline(256)
            rendered += 256
            if block % 16 == 15:
                # The capture drains on a thread of its own; rendering faster than it
                # drains would lose frames. Wait for it to catch up.
                deadline = time.monotonic() + 5.0
                while engine.capture_frames() < rendered and time.monotonic() < deadline:
                    time.sleep(0.002)
        result = engine.capture_stop()
        assert int(result["lost"]) == 0
    finally:
        engine.stop()
    return np.asarray(result["samples"], dtype=np.float32).reshape(-1, 2)


def test_dx7_offline_equals_live(tmp_path: pathlib.Path) -> None:
    for fm1_rate in (False, True):
        project, diagnostics = Project.loads(project_text(fm1_rate))
        assert not [d for d in diagnostics if d["severity"] == "error"]
        path = tmp_path / f"offline-{fm1_rate}.wav"
        engine_bridge.render_to_file(project, str(path), tail_seconds=1.0)
        offline, _rate = read_wav(path)
        captured = live(project, 9.0)
        assert float(np.abs(offline).max()) > 0.01
        assert float(np.abs(captured).max()) > 0.01
        assert glitch.bit_identical(captured, offline), f"fm1.rate={fm1_rate}"
