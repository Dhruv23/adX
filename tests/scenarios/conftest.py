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


def pytest_runtest_logreport(report: pytest.TestReport) -> None:
    """On GitHub Actions, each failure is also an ``::error`` annotation (as in
    tests/python/conftest.py): the full check's log needs a token, annotations do not."""
    if not os.environ.get("GITHUB_ACTIONS") or not report.failed:
        return
    path, line, _name = report.location
    text = str(report.longrepr) if report.longrepr is not None else "failed"
    lines = [ln for ln in text.splitlines() if ln.startswith("E ")] or text.splitlines()[-3:]
    message = " | ".join(ln.strip() for ln in lines[:10])[:1500]
    message = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    where = f"file={path.replace(chr(92), '/')},line={(line or 0) + 1}"
    print(f"\n::error {where}::{report.nodeid} [{report.when}]: {message}")
