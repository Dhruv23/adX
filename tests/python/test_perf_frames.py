"""The 60 fps claim, measured on a real GPU (phase_5.md 4.9, 6). Non-blocking in CI.

GitHub's Windows runners have no usable GPU, and a frame-rate test under a software
rasterizer measures the rasterizer, not adX - so this is marked ``gpu``, CI deselects
it (``-m "not gpu"``), and it is run locally and recorded in the phase log. The
deterministic causes of a slow frame (build cost, FFI calls per pan frame) are gated
on every push by test_perf_geometry.py and bench_piano_roll.cpp.
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys

import pytest

DRIVER = pathlib.Path(__file__).with_name("perf_frames_driver.py")
FRAME_BUDGET_MS = 1000.0 / 60.0


@pytest.mark.gpu
def test_perf_frames_gpu() -> None:
    """p99 frame time under 16.6 ms while panning and zooming 10,000 notes."""
    if os.environ.get("CI"):
        pytest.skip("no usable GPU on CI runners (phase_5.md 4.9)")
    result = subprocess.run(
        [sys.executable, str(DRIVER), "1200"],
        capture_output=True,
        text=True,
        timeout=300,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    stats = json.loads(result.stdout.strip().splitlines()[-1])
    print(f"perf_frames_gpu: {stats}")
    if stats["api"] in ("Software", "Null"):
        pytest.skip(f"Qt Quick is on the {stats['api']} backend: no GPU to measure")
    assert stats["frames"] >= 1000
    # The work per frame fits in a 60 Hz frame at the 99th percentile...
    assert stats["p99_ms"] < FRAME_BUDGET_MS, stats
    # ...and fewer than 1% of frames missed their vsync (a miss doubles the interval).
    assert stats["interval_p99_ms"] < FRAME_BUDGET_MS * 1.5, stats
