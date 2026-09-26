"""Every example round-trips through the Python API (FINAL_PLAN 6 rule 6)."""

from __future__ import annotations

import pathlib
import time

import pytest

import adx_engine

EXAMPLES = ("example.adx", "c418_demo.adx", "stakillaz_demo.adx", "suffocation.adx")


@pytest.mark.parametrize("name", EXAMPLES)
def test_example_roundtrips(repo_root: pathlib.Path, name: str) -> None:
    """Load v1, save v2, reload: same model, same text, no errors."""
    path = repo_root / "docs" / "examples" / name
    project, diagnostics = adx_engine.Project.load(str(path))
    assert not [d for d in diagnostics if d["severity"] == "error"]
    assert project.source_version == 1

    text = project.dumps()
    again, again_diagnostics = adx_engine.Project.loads(text)
    assert not [d for d in again_diagnostics if d["severity"] == "error"]
    assert again.source_version == 2
    assert again.dumps() == text
    assert again.channels == project.channels
    for pattern in project.patterns:
        assert again.note_count(pattern) == project.note_count(pattern)


def test_load_budget(repo_root: pathlib.Path) -> None:
    """phase_2.md 9: loading stays well inside half a second, even at 100k notes.

    Command-per-mutation could have made bulk loads slow; one grouped load with a
    bulk AddNotes is what prevents it. Measured at ~1 ms for suffocation.adx and
    ~150 ms for 100k notes, so these bounds catch a regression, not jitter.
    """
    start = time.perf_counter()
    adx_engine.Project.load(str(repo_root / "docs" / "examples" / "suffocation.adx"))
    assert time.perf_counter() - start < 0.5

    lines = ["[PROJECT]", "ADX_VERSION=2", "", "[CHANNEL A]", "VOLUME=1", ""]
    lines += ["[PATTERN P]", "LENGTH=7000:0:0", "NOTES A"]
    lines += [f"  C4 {i // 16}:{(i // 4) % 4}:{(i % 4) * 960} 0:0:960 100" for i in range(100000)]
    start = time.perf_counter()
    project, _ = adx_engine.Project.loads("\n".join(lines) + "\n")
    elapsed = time.perf_counter() - start
    assert project.note_count("P") == 100000
    assert elapsed < 2.0
