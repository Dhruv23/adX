"""Measure recording latency on a real interface: phase_8.md's hardware gate.

Cable the interface's output 1 to its input 1 (and 2 to 2 if you have a second
cable), turn its direct monitoring off, set its levels to about unity, then:

    py -3.12 tools/measure_latency.py

At 64, 256 and 1024 frames it opens the default devices, runs the loopback
calibration (one impulse out, listen for it), then records a click through the
cable with that measurement and checks the take lands where the click is. It prints
one line per buffer size - reported latency, measured round trip, and how many
samples the recorded click is from the original - and exits 0 only if every take is
within one sample (record_latency_alignment, on hardware).

Copy the printed lines into plans/phase_8.md's session table.
"""

from __future__ import annotations

import argparse
import struct
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

from adx.engine_bridge import (
    Calibration,
    Engine,
    Project,
    RecordSession,
    commands,
    render_to_file,
)

RATE = 48000
BLOCKS = (64, 256, 1024)


def _click_wav(path: Path) -> None:
    """Three seconds of silence with a short full-ish click at one second."""
    samples = np.zeros((3 * RATE, 2), dtype=np.float32)
    samples[RATE : RATE + 8] = 0.8
    data = samples.tobytes()
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + len(data), b"WAVE", b"fmt ", 16, 3, 2, RATE, RATE * 8, 8, 32,
        b"data", len(data),
    )  # fmt: skip
    path.write_bytes(header + data)


def _first_above(path: str, level: float) -> int:
    raw = Path(path).read_bytes()
    at = raw.index(b"data") + 8
    left = np.frombuffer(raw[at:], dtype=np.float32)[0::2]
    hits = np.flatnonzero(np.abs(left) > level)
    return int(hits[0]) if hits.size else -1


def _until_audible(engine: Engine, project: Project) -> None:
    """Let the engine finish decoding the click: an audio clip plays a silent stand-in
    until its buffer is ready, and a commit is what swaps the real one in (the app's
    60 Hz frame does it; nothing here would)."""
    deadline = time.monotonic() + 10.0
    while engine.audio_pending and time.monotonic() < deadline:
        engine.commit(project)
        time.sleep(0.005)


def _run(block: int, folder: Path, seconds: float, simulate: int | None) -> tuple[bool, str]:
    click = folder / "click.wav"
    _click_wav(click)
    text = (
        "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\n"
        f'TRACK 1 name="Click"\n  AUDIO "{click.as_posix()}" 0:0:0\n'
        'TRACK 2 name="Rec"\n\n[MIXER]\nINSERT 1 name="Master"\n'
    )
    project, _ = Project.loads(text, str(folder))
    engine = Engine(sample_rate=RATE, block_frames=block, input_channels=2, loopback=simulate)
    engine.set_project(project)
    _until_audible(engine, project)
    engine.start()

    def wait(duration: float) -> None:
        # A real device runs on its own clock; a simulated cable is rendered here.
        if simulate is None:
            time.sleep(duration)
        else:
            engine.render_offline(block * (int(duration * RATE) // block + 1))

    try:
        calibration = Calibration(engine)
        calibration.begin(project)
        wait(1.0)
        measured = calibration.finish()
        if measured is None:
            return False, f"block {block:5d}: the impulse never came back - check the cable"
        session = RecordSession(
            engine, project, {"track": 2, "project_directory": str(folder), "round_trip": measured}
        )
        session.start()
        reported = session.latency
        wait(seconds)
        take = session.stop()
    finally:
        engine.stop()
    if not take["ok"]:
        return False, f"block {block:5d}: the take failed: {take['error']}"

    # The click alone, then the take alone, rendered offline at the same block size.
    items = {int(r["track"]): int(r["item"]) for r in project.playlist_items()}
    project.execute(commands.build("SetPlaylistItem", 2, items[2], {"muted": 1}))
    render_to_file(project, str(folder / "a.wav"))
    original = _first_above(str(folder / "a.wav"), 0.3)
    project.undo()
    project.execute(commands.build("SetPlaylistItem", 1, items[1], {"muted": 1}))
    render_to_file(project, str(folder / "b.wav"))
    recorded = _first_above(str(folder / "b.wav"), 0.3)
    offset = recorded - original
    reported_rt = reported["input"] + reported["output"]
    line = (
        f"block {block:5d}: reported {reported_rt:5d} samples, measured {measured:5d}, "
        f"take offset {offset:+d} samples ({offset / RATE * 1000.0:+.3f} ms)"
    )
    return abs(offset) <= 1 and original >= 0 and recorded >= 0, line


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--seconds", type=float, default=2.5, help="length of each take")
    parser.add_argument(
        "--simulate",
        type=int,
        metavar="FRAMES",
        help="no hardware: an offline engine whose cable delays by FRAMES (checks the tool)",
    )
    args = parser.parse_args()
    ok = True
    for block in BLOCKS:
        with tempfile.TemporaryDirectory() as folder:
            passed, line = _run(block, Path(folder), args.seconds, args.simulate)
        print(("PASS  " if passed else "FAIL  ") + line)
        ok = ok and passed
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
