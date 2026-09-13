"""Phase 0's Python test surface: the bridge loads and the version is one value.

Three tests is the right number here. Their job is to prove the pipeline works,
not to prove code correct - there is no code yet (phase_0.md 5).
"""

from __future__ import annotations

import re
import subprocess
import sys

import adx
import adx_engine

_SEMVER = re.compile(r"^\d+\.\d+\.\d+$")


def test_module_imports() -> None:
    """``import adx_engine`` succeeds and reports a well-formed version."""
    reported = adx_engine.version()
    assert _SEMVER.match(reported), f"engine version is not MAJOR.MINOR.PATCH: {reported!r}"


def test_version_agrees() -> None:
    """The wheel's metadata and the compiled engine report the same version.

    Both derive from the ``project(adx VERSION ...)`` line in CMakeLists.txt, by
    two independent routes: scikit-build-core's regex metadata provider, and
    ``configure_file`` into Version.h. This test is what makes a divergence a CI
    failure rather than a confusing bug report (phase_0.md 3.6).
    """
    assert adx_engine.version() == adx.__version__


def test_cli_version() -> None:
    """``adx --version`` exits 0 and prints the same version string."""
    result = subprocess.run(
        [sys.executable, "-m", "adx", "--version"],
        capture_output=True,
        text=True,
        check=True,
    )
    assert adx_engine.version() in result.stdout
