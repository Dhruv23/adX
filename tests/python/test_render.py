"""The render engine from Python: offline render, and a transport that moves."""

from __future__ import annotations

import pathlib
import struct
import time

import pytest

import adx_engine


@pytest.fixture
def suffocation(repo_root: pathlib.Path) -> adx_engine.Project:
    """suffocation.adx, loaded through the v1 shim."""
    project, _ = adx_engine.Project.load(str(repo_root / "docs" / "examples" / "suffocation.adx"))
    return project


def test_render_suffocation_to_wav(suffocation: adx_engine.Project, tmp_path: pathlib.Path) -> None:
    """phase_3.md 7: render 30 s of suffocation.adx offline to a WAV, and it makes sound."""
    out = tmp_path / "suffocation.wav"
    ticks_per_second = adx_engine.PPQ * 120 // 60  # suffocation is 120 bpm
    stats = adx_engine.render_offline(suffocation, str(out), end=30 * ticks_per_second)

    assert stats["frames"] == 30 * 48000
    assert stats["rt_violations"] == 0
    assert stats["peak"] > 0.01
    assert stats["rms"] > 0.001
    assert len(stats["hash"]) == 32

    # A real WAV: IEEE float, stereo, 48 kHz, the right length, not silent.
    raw = out.read_bytes()
    assert raw[:4] == b"RIFF"
    assert raw[8:12] == b"WAVE"
    audio_format, channels, rate = struct.unpack("<HHI", raw[20:28])
    assert (audio_format, channels, rate) == (3, 2, 48000)
    data_bytes = struct.unpack("<I", raw[40:44])[0]
    assert data_bytes == 30 * 48000 * 2 * 4
    samples = struct.unpack(f"<{data_bytes // 4}f", raw[44 : 44 + data_bytes])
    assert max(abs(sample) for sample in samples[::97]) > 0.01


def test_render_is_deterministic(suffocation: adx_engine.Project) -> None:
    """Two renders of the same project are the same bits."""
    first = adx_engine.render_offline(suffocation, end=4 * adx_engine.PPQ)
    second = adx_engine.render_offline(suffocation, end=4 * adx_engine.PPQ)
    assert first["hash"] == second["hash"]


def test_transport_moves_on_a_null_stream(suffocation: adx_engine.Project) -> None:
    """Play, poll the position the way a 60 Hz UI timer would, seek, stop."""
    engine = adx_engine.Engine(null_backend=True)
    engine.set_project(suffocation)
    transport = engine.transport
    transport.play()
    engine.start()
    try:
        deadline = time.monotonic() + 5.0
        while transport.position_ticks() < adx_engine.PPQ and time.monotonic() < deadline:
            time.sleep(1 / 60)
            engine.pump()
        assert transport.position_ticks() >= adx_engine.PPQ
        assert transport.state() == "playing"

        transport.stop()
        transport.seek_ticks(16 * adx_engine.PPQ)
        deadline = time.monotonic() + 5.0
        while transport.state() != "stopped" and time.monotonic() < deadline:
            time.sleep(1 / 60)
        time.sleep(0.05)
        assert transport.state() == "stopped"
        assert transport.position_ticks() == 16 * adx_engine.PPQ
    finally:
        engine.stop()
    assert adx_engine.rt_violation_count() == 0 or not adx_engine.rt_guard_enabled()


def test_levels_reads_every_strip_in_one_call(suffocation: adx_engine.Project) -> None:
    """P3-7: one call returns every strip's latest meter frame, as an N x 9 array."""
    engine = adx_engine.Engine(null_backend=True)
    engine.set_project(suffocation)
    idle = engine.levels()
    assert idle.dtype.name == "float32"
    assert idle.ndim == 2
    assert idle.shape[1] == 9
    strips = idle.shape[0]
    assert strips >= 2  # the master and at least one track's insert
    assert sorted(set(idle[:, 0].tolist())) == sorted(idle[:, 0].tolist())  # one row per insert

    engine.transport.play()
    engine.start()
    try:
        deadline = time.monotonic() + 10.0
        levels = engine.levels()
        while time.monotonic() < deadline:
            time.sleep(1 / 60)
            engine.pump()
            levels = engine.levels()
            # The kick plays from the first beat; wait for the loudness window to fill.
            if levels[:, 1].max() > 0.01 and levels[:, 5].max() > -70.0:
                break
    finally:
        engine.stop()
    assert levels.shape == (strips, 9)
    assert levels[:, 1].max() > 0.01  # peak left
    assert levels[:, 3].max() > 0.001  # RMS left
    assert levels[:, 5].max() > -70.0  # momentary LUFS, not silence (-200)
    assert (levels[:, 1] <= 4.0).all()


def test_knob_turn_does_not_rebuild(suffocation: adx_engine.Project) -> None:
    """set_param edits the project and posts the value; no snapshot is rebuilt."""
    engine = adx_engine.Engine(null_backend=True)
    engine.set_project(suffocation)
    assert engine.snapshots_built == 1
    for step in range(100):
        assert engine.set_param(suffocation, "insert.1.gain", 1.0 - step / 200.0)
        assert engine.commit(suffocation) is False
    assert engine.snapshots_built == 1
    with pytest.raises(ValueError, match="no such parameter"):
        engine.set_param(suffocation, "insert.1.nonsense", 0.5)


def test_sample_accurate_render_through_python(tmp_path: pathlib.Path) -> None:
    """A note at tick 1000 at 120 bpm starts on frame 6250, exactly.

    On the test tone, whose first sample is never zero: a sine-phase instrument's is.
    """
    text = """[PROJECT]
ADX_VERSION=2

[CHANNEL Tone]
INSTRUMENT=testtone
OUTPUT=insert.1

[PATTERN P]
LENGTH=1:0:0
NOTES Tone
  A4 0:0:1000 0:1:0 100

[PLAYLIST]
TRACK 1
  PATTERN P 0:0:0

[MIXER]
INSERT 1 name="Master"
"""
    project, diagnostics = adx_engine.Project.loads(text, "")
    assert not [d for d in diagnostics if d["severity"] == "error"]
    out = tmp_path / "tone.wav"
    adx_engine.render_offline(project, str(out))
    raw = out.read_bytes()[44:]
    left = struct.unpack(f"<{len(raw) // 4}f", raw)[0::2]
    first = next(index for index, sample in enumerate(left) if sample != 0.0)
    assert first == 6250
