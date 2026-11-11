"""The audition tool and the taxonomy (phase_12.md 4.11, audition_loudness)."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pytest

from adx import taxonomy

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "packs"))

import audition  # noqa: E402


def test_audition_loudness_matches_bs1770() -> None:
    # BS.1770-4's reference: a full-scale 997 Hz sine in both channels reads 0 LUFS
    # (-3.01 in one channel), so -20 dBFS in both reads -20. The tool's reading is the
    # engine's meter; it must land within 0.1 LU.
    rate = 48000
    t = np.arange(rate * 10) / rate
    tone = (10 ** (-20 / 20)) * np.sin(2 * np.pi * 997 * t)
    stereo = np.stack([tone, tone], axis=1).astype(np.float32)
    levels = audition.measure(stereo, rate)
    assert abs(levels["lufs"] - (-20.0)) < 0.1
    # True peak of a sine is its amplitude.
    assert abs(levels["true_peak_db"] - (-20.0)) < 0.05


def test_true_peak_finds_intersample_peaks() -> None:
    # fs/4 at 45 degrees: every sample is at 0.707 of the peak; the true peak is 3 dB up.
    n = np.arange(4800)
    x = np.sin(np.pi / 2 * n + np.pi / 4).astype(np.float32)
    stereo = np.stack([x, x], axis=1)
    assert 20 * np.log10(np.max(np.abs(x))) == pytest.approx(-3.01, abs=0.01)
    assert audition.true_peak_db(stereo) == pytest.approx(0.0, abs=0.05)


def test_phrases_exist_for_every_category() -> None:
    for path in taxonomy.CATEGORIES:
        hits, beats, _ = audition.phrase_for(path)
        assert hits, path
        assert all(h.beat + h.beats <= beats + 1e-9 for h in hits), path
        assert taxonomy.category(path).macro_slots, path
        assert -30 < taxonomy.category(path).loudness_target < 0, path


def test_macro_names_follow_the_category() -> None:
    assert taxonomy.macro_names_ok("Lead", ["Brightness", "Vibrato", "Space"]) == []
    problems = taxonomy.macro_names_ok("Lead", ["Vibrato", "Brightness", "Tail Length"])
    assert any("out of slot order" in p for p in problems)
    assert any("Tail Length" in p for p in problems)


def test_audition_renders_and_caches(tmp_path: Path) -> None:
    preset = tmp_path / "saw.adxpreset"
    preset.write_text(
        '[PRESET]\nNAME="Test Saw"\nTYPE=va\nPACK=test\nCATEGORY=Lead\n\n[PARAMS]\nlevel=-12\n',
        encoding="utf-8",
    )
    result = audition.audition(preset, tmp_path / "out")
    assert result["category"] == "Lead"
    assert result["frames"] > 48000 * 4
    assert -40 < result["lufs"] < 0
    assert result["true_peak_db"] < 0
    folder = tmp_path / "out" / "test"
    assert (folder / "Test Saw.wav").stat().st_size > 1000
    assert (folder / "Test Saw.png").stat().st_size > 1000
    saved = json.loads((folder / "Test Saw.json").read_text(encoding="utf-8"))
    assert saved["lufs"] == pytest.approx(result["lufs"])
    # Deterministic: a second audition is the same render.
    again = audition.audition(preset, tmp_path / "again")
    assert again["hash"] == result["hash"]
