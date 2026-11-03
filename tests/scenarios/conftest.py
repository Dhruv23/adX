"""Scenarios (phase_10.5.md 5): adX launched as the user launches it, driven through the
control channel, measured by probes, captures and the glitch detector.

Run them explicitly: ``py -3.12 -m pytest tests/scenarios``. ``-m "not wannacry"`` is
what CI runs; locally the WannaCry cases must run, not skip (wannacry.py).
"""

from __future__ import annotations

import os
import pathlib
import sys
import tempfile

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

os.environ.setdefault(
    "ADX_PLUGIN_CACHE",
    str(pathlib.Path(tempfile.gettempdir()) / f"adx-scenarios-{os.getpid()}" / "plugin-cache.json"),
)


@pytest.fixture
def evidence(tmp_path: pathlib.Path) -> pathlib.Path:
    """Where a scenario leaves its captures and screenshots."""
    out = tmp_path / "evidence"
    out.mkdir()
    return out
