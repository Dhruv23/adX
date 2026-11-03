"""Recording from Python (phase_8.md 3.1), on an offline engine whose outputs are wired
back to its inputs: calibrate, record a take, comp two takes.

The engine's tests own the sample-exact claims (record_latency_alignment and the rest
in tests/cpp/record); these check that the bindings drive a whole take end to end.
"""

from __future__ import annotations

import pathlib
import struct
import time

import numpy as np
import pytest

from adx.engine_bridge import (
    Calibration,
    Engine,
    Project,
    RecordSession,
    commands,
    comp_takes,
    render_to_file,
)

RATE = 48000
BLOCK = 256
DELAY = 2 * BLOCK + 37


def _click(path: pathlib.Path) -> None:
    """Two seconds of 32-bit float stereo silence with a short click at one second."""
    frames = 2 * RATE
    samples = [0.0] * (frames * 2)
    for i in range(8):
        samples[(RATE + i) * 2] = samples[(RATE + i) * 2 + 1] = 0.9
    data = struct.pack(f"<{len(samples)}f", *samples)
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + len(data),
        b"WAVE",
        b"fmt ",
        16,
        3,
        2,
        RATE,
        RATE * 8,
        8,
        32,
        b"data",
        len(data),
    )
    path.write_bytes(header + data)


def _until_audible(engine: Engine, project: Project) -> None:
    """Let the engine finish decoding the click: an audio clip plays a silent stand-in
    until its buffer is ready, and a commit is what swaps the real one in (the app's
    60 Hz frame does it; nothing here would)."""
    deadline = time.monotonic() + 10.0
    while engine.audio_pending:
        assert time.monotonic() < deadline, "the click never finished decoding"
        engine.commit(project)
        time.sleep(0.005)


@pytest.fixture
def rig(tmp_path: pathlib.Path) -> tuple[Engine, Project, int]:
    """A click track, an empty track to record on, and a loopback engine playing them."""
    click = tmp_path / "click.wav"
    _click(click)
    text = (
        "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\n"
        f'TRACK 1 name="Click"\n  AUDIO "{click.as_posix()}" 0:0:0\n'
        'TRACK 2 name="Rec"\n\n[MIXER]\nINSERT 1 name="Master"\n'
    )
    project, diagnostics = Project.loads(text, str(tmp_path))
    assert not [d for d in diagnostics if d["severity"] == "error"]
    tracks = project.playlist_info()["tracks"]
    rec = int(next(track["id"] for track in tracks if track["name"] == "Rec"))
    engine = Engine(sample_rate=RATE, block_frames=BLOCK, input_channels=2, loopback=DELAY)
    engine.set_project(project)
    _until_audible(engine, project)
    engine.start()
    return engine, project, rec


def _first_click(project: Project, path: pathlib.Path) -> int:
    """The first frame above half scale of an offline render of ``project``."""
    render_to_file(project, str(path))
    raw = path.read_bytes()
    left = np.frombuffer(raw[raw.index(b"data") + 8 :], dtype=np.float32)[0::2]
    hits = np.flatnonzero(np.abs(left) > 0.4)
    return int(hits[0]) if hits.size else -1


def _blocks(seconds: float) -> int:
    return BLOCK * (int(seconds * RATE) // BLOCK + 4)


def test_calibration_measures_the_loopback(rig: tuple[Engine, Project, int]) -> None:
    """The ping comes back exactly the loopback's delay later."""
    engine, project, _ = rig
    calibration = Calibration(engine)
    calibration.begin(project)
    engine.render_offline(_blocks(0.25))
    assert calibration.finish() == DELAY
    engine.stop()


def test_a_take_lands_on_its_track(
    rig: tuple[Engine, Project, int], tmp_path: pathlib.Path
) -> None:
    """Start, roll two seconds, stop: a file, an item on the track, the latency moved back."""
    engine, project, rec = rig
    session = RecordSession(
        engine,
        project,
        {"track": rec, "project_directory": str(tmp_path), "round_trip": DELAY, "name": "Vox"},
    )
    session.start()
    engine.render_offline(_blocks(2.0))
    take = session.stop()
    engine.stop()
    assert take["ok"], take["error"]
    assert not take["overran"]
    assert pathlib.Path(take["file"]).is_file()
    assert take["frames"] > RATE
    assert take["latency"] == DELAY
    assert session.latency["compensation"] == DELAY
    items = project.playlist_items()
    assert take["item"] in {int(row["item"]) for row in items if int(row["track"]) == rec}
    assert project.undo_depth() == 1

    # The click alone, then the take alone: the same sample, within one.
    click = next(int(row["item"]) for row in items if int(row["track"]) != rec)
    project.execute(commands.build("SetPlaylistItem", rec, take["item"], {"muted": 1}))
    original = _first_click(project, tmp_path / "click_only.wav")
    project.undo()
    tracks = {int(row["item"]): int(row["track"]) for row in items}
    project.execute(commands.build("SetPlaylistItem", tracks[click], click, {"muted": 1}))
    recorded = _first_click(project, tmp_path / "take_only.wav")
    assert original == RATE
    assert abs(recorded - original) <= 1


def test_two_takes_comp_into_lane_zero(
    rig: tuple[Engine, Project, int], tmp_path: pathlib.Path
) -> None:
    """Two takes stack on lanes; a comp of half of each is one undo step."""
    engine, project, rec = rig
    takes = []
    for name in ("A", "B"):
        engine.seek(0)
        session = RecordSession(
            engine,
            project,
            {"track": rec, "project_directory": str(tmp_path), "round_trip": DELAY, "name": name},
        )
        session.start()
        engine.render_offline(_blocks(1.0))
        take = session.stop()
        assert take["ok"], take["error"]
        takes.append(take)
    engine.stop()
    assert takes[0]["lane"] != takes[1]["lane"]

    depth = project.undo_depth()
    half = 960
    comp_takes(project, rec, [(0, half, takes[0]["item"]), (half, 2 * half, takes[1]["item"])])
    assert project.undo_depth() == depth + 1
