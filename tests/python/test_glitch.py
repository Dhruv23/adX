"""The glitch detector is a gate, so it is seen to fire first (phase_10.5.md 4.5, 5:
``glitch_positive_controls``): each defect planted in a clean render is found where it was
planted, and a clean render yields nothing. Plus the capture and the xrun count."""

from __future__ import annotations

import pathlib
import time

import numpy as np
import pytest

from adx import engine_bridge, generate
from adx.engine_bridge import Engine, Project
from adx.testing import glitch
from adx.testing.wav import read_wav

RATE = 48000


def _render(text: str, path: pathlib.Path, seconds: float = 6.0) -> np.ndarray:
    project, _ = Project.loads(text)
    result = engine_bridge.export_project(
        project,
        {
            "output": str(path),
            "format": "wav32f",
            "range": "selection",
            "from": 0,
            "to": project.ticks_at_seconds(seconds),
        },
    )
    assert result["ok"], result
    audio, rate = read_wav(path)
    assert rate == RATE
    return audio


@pytest.fixture(scope="module")
def clean_renders(tmp_path_factory: pytest.TempPathFactory) -> list[np.ndarray]:
    """Two kinds of material: soft pads and arps, and blown-out kicks and screeches."""
    folder = tmp_path_factory.mktemp("glitch")
    return [
        _render(generate.generate("c418", seed=1, bars=8).text, folder / "c418.wav"),
        _render(generate.generate("stakillaz", seed=1, bars=8).text, folder / "stakillaz.wav"),
    ]


def test_clean_renders_yield_nothing(clean_renders: list[np.ndarray]) -> None:
    for audio in clean_renders:
        assert float(np.abs(audio).max()) > 0.05
        found = glitch.find_glitches(audio, RATE)
        assert found == [], [str(f) for f in found[:5]]


def test_glitch_positive_controls(clean_renders: list[np.ndarray]) -> None:
    for clean in clean_renders:
        # Somewhere loud, so every defect lands in the music, not in a gap.
        level = np.abs(clean).max(axis=1)
        at = int(np.argmax(level[RATE : 4 * RATE])) + RATE

        dropout = clean.copy()
        dropout[at : at + 240] = 0.0  # 5 ms
        found = glitch.dropouts(dropout, RATE)
        assert len(found) == 1, found
        assert abs(found[0].start - at) <= 1, found

        repeated = clean.copy()
        repeated[at + 256 : at + 512] = repeated[at : at + 256]  # a stuck 256-frame buffer
        found = glitch.repeats(repeated)
        assert any(abs(f.start - (at + 256)) <= 1 for f in found), found

        clicked = clean.copy()
        # A one-sample spike, larger than any edge around it: in square-wave material a
        # click no bigger than the edges is not one a listener can pick out either.
        around = np.abs(np.diff(clean[at + 100 - 480 : at + 100 + 480], axis=0)).max()
        clicked[at + 100] += max(0.6, 5.0 * float(around))
        found = glitch.clicks(clicked, RATE)
        assert any(abs(f.start - (at + 100)) <= 1 for f in found), found

        noisy = clean.copy()
        rng = np.random.default_rng(1)
        noisy[at : at + 4800] += (rng.standard_normal((4800, 2)) * 10 ** (-60 / 20)).astype(
            np.float32
        )
        found = glitch.residuals(noisy, clean, RATE, lag=0)
        assert found
        assert all(at - 1024 <= f.start <= at + 4800 for f in found), found
        # ...and the window a scenario says it edited is not reported.
        assert (
            glitch.residuals(
                noisy, clean, RATE, lag=0, ignore=[(at / RATE - 0.05, (at + 4800) / RATE + 0.05)]
            )
            == []
        )

        # Every planted defect through find_glitches, with its kind.
        kinds = {f.kind for f in glitch.find_glitches(dropout, RATE)}
        assert "dropout" in kinds


def test_alignment_and_identity(clean_renders: list[np.ndarray]) -> None:
    clean = clean_renders[0]
    shifted = np.concatenate([np.zeros((333, 2), dtype=np.float32), clean])
    assert glitch.align(shifted, clean) == 333
    assert glitch.bit_identical(shifted, clean)
    assert glitch.residuals(shifted, clean, RATE) == []
    nudged = shifted.copy()
    nudged[5000, 0] += 1e-3
    assert not glitch.bit_identical(nudged, clean)


def test_capture_and_xruns_from_python(tmp_path: pathlib.Path) -> None:
    """Engine.capture_*, Engine.xruns(): a null-audio engine plays; the capture is whole
    (nothing lost), clean, and the stall hook raises the xrun count."""
    project, _ = Project.loads(generate.generate("c418", seed=2, bars=8).text)
    engine = Engine(null_backend=True)
    engine.set_project(project)
    engine.start()
    engine.capture_start()
    engine.play()
    time.sleep(2.0)
    before = engine.xruns()
    path = tmp_path / "capture.wav"
    result = engine.capture_stop(str(path))
    assert result["lost"] == 0
    assert result["frames"] > RATE
    audio, rate = read_wav(path)
    assert len(audio) == result["frames"]
    assert float(np.abs(audio).max()) > 0.01
    assert glitch.find_glitches(audio, rate) == []
    engine.set_callback_stall_us(12000)
    time.sleep(1.0)
    engine.set_callback_stall_us(0)
    assert engine.xruns() > before + 5
    engine.stop()
