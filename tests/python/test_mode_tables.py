"""physical's mode tables against the instruments they model (phase_12.md 4.4.3, 5:
mode_tables_match_references): each measured table's first three modes within 2 % of
the ratios tools/packs/measure_modes.py measures from its library samples.

Local: it needs VCSL in the library store (fetch_libraries.py fetch vcsl) and skips
without it, so CI, which fetches no libraries, does not run it.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "packs"))

import measure_modes  # noqa: E402


@pytest.mark.parametrize("name", sorted(measure_modes.REFERENCES))
def test_mode_tables_match_references(name: str) -> None:
    ref = measure_modes.REFERENCES[name]
    if measure_modes.library_folder(ref.library) is None:
        pytest.skip(f"{ref.library} is not in the library store")
    table = measure_modes.tables()[name]
    measured = measure_modes.measure(name)
    assert measured is not None, f"no {name} samples measured"
    for mode, (want, got) in enumerate(zip(table[:3], measured[:3], strict=True)):
        assert abs(got / want - 1.0) < 0.02, (
            f"{name} mode {mode + 1}: table {want}, measured {got:.3f}"
        )
