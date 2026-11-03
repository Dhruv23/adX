"""scenario_wannacry_matches_reference (phase_10.5.md 4.8): what the agent wrote is the
song. The transcription's render against the tutorial's audio (tools/extract_reference.py):
- ``adx analyze bpm`` within 1 of 126 on both;
- the same key from ``adx analyze key`` on both;
- per-bar chroma correlation, the render aligned to the reference, above the thresholds
  fixed on the first run (phase_10.5.md 10): a mean of 0.88 and a lowest tenth of 0.82
  were measured; the gate is 0.80 and 0.75.

A software check of the transcription and of the engine playing it, not a listening
verdict. On the stand-in (CI) there is no recording, so the render of a committed
genre-suite track (STAKILLAZ, whose drums give it a pulse; the C418 track's pads do not)
is held to the project's own tempo and to the key of its notes.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys

import numpy as np
import pytest
import reference_match as rm
from wannacry import MATERIAL, ROOT, require

from adx import analysis
from adx.engine_bridge import Project, analyze, scale_of
from adx.testing.wav import read_wav

PULSE = ROOT / "tests" / "golden" / "genre_stakillaz_seed1.adx"
MEAN = 0.80
LOW_TENTH = 0.75


def adx(*args: str) -> dict[str, object]:
    run = subprocess.run(
        [sys.executable, "-m", "adx", *args], capture_output=True, text=True, check=False, cwd=ROOT
    )
    assert run.returncode == 0, run.stderr
    out: dict[str, object] = json.loads(run.stdout)
    return out


def render(project: pathlib.Path, out: pathlib.Path) -> pathlib.Path:
    run = subprocess.run(
        [sys.executable, "-m", "adx", "render", str(project), "-o", str(out), "--format", "wav32f"],
        capture_output=True,
        text=True,
        check=False,
        cwd=ROOT,
    )
    assert run.returncode == 0, run.stderr
    return out


def key_name(result: dict[str, object]) -> str:
    key = result["key"]
    return str(key["name"]) if isinstance(key, dict) else str(key)


@pytest.mark.parametrize("material", MATERIAL)
def test_wannacry_matches_reference(material: str, tmp_path: pathlib.Path) -> None:
    if material == "standin":
        ours = render(PULSE, tmp_path / "render.wav")
        bpm = adx("analyze", "bpm", str(ours))["bpm"]
        project = Project.load(str(PULSE))[0]
        assert bpm is not None and abs(float(bpm) - project.bpm_at(0)) <= 1.0, (
            bpm,
            project.bpm_at(0),
        )
        notes = [n.row() for n in analysis.arrangement_notes(project)]
        from_notes = analyze("key", {"notes": notes})
        # The same scale: the key, or its relative (E major and C# minor share their notes,
        # and an audio key detector confuses the two; phase_10.md 11).
        heard, written = key_name(adx("analyze", "key", str(ours))), key_name(from_notes)
        assert set(scale_of(heard)) == set(scale_of(written)), (heard, written)
        return

    corpus = require("piano", "reference")
    ours = render(corpus.piano, tmp_path / "render.wav")
    for path in (ours, corpus.reference):
        bpm = adx("analyze", "bpm", str(path))["bpm"]
        assert bpm is not None and abs(float(bpm) - 126.0) <= 1.0, (path.name, bpm)
    assert key_name(adx("analyze", "key", str(ours))) == key_name(
        adx("analyze", "key", str(corpus.reference))
    )

    a, rate = read_wav(ours)
    b, reference_rate = read_wav(corpus.reference)
    assert rate == reference_rate
    ca, cb = rm.chroma(a, rate), rm.chroma(b, rate)
    shift = rm.best_offset(ca, cb, int(60 * rate / rm.HOP))
    bars = rm.bar_correlations(ca, cb, shift, 4 * 60 / 126 * rate / rm.HOP)
    assert len(bars) > 100
    mean, low = float(np.mean(bars)), float(np.percentile(bars, 10))
    print(
        f"aligned {shift * rm.HOP / rate:.2f} s into the reference; {len(bars)} bars, "
        f"mean {mean:.3f}, lowest tenth {low:.3f}, lowest {min(bars):.3f}"
    )
    assert mean >= MEAN and low >= LOW_TENTH, (mean, low)
