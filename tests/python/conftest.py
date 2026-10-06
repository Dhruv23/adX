"""Shared fixtures for the Python suite."""

from __future__ import annotations

import os
import pathlib
import random

import pytest

# Widget and Qt Quick tests run without a display unless a test asks for the GPU (and
# runs itself in a subprocess). Set before PySide6 creates the application.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

_REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

#: PPQ, repeated so the fixtures need no engine import to build text.
_PPQ = 3840


@pytest.fixture(scope="session")
def repo_root() -> pathlib.Path:
    """The repository root.

    Phase 2 onward needs it to reach ``docs/examples/*.adx``, which are permanent
    parser regression fixtures (FINAL_PLAN 6).
    """
    return _REPO_ROOT


def roll_text(
    notes: int, bars: int = 64, seed: int = 1, ghosts: int = 0, voice: bool = False
) -> str:
    """A v2 project: one pattern, a Lead channel with ``notes`` seeded random notes,
    a Pad channel with ``ghosts`` more, the pattern placed at tick 0."""
    rng = random.Random(seed)
    sixteenth = _PPQ // 4

    def lines(count: int) -> list[str]:
        out = []
        for _ in range(count):
            start = rng.randrange(bars * 16) * sixteenth
            length = rng.randint(1, 8) * sixteenth
            bar, rest = divmod(start, _PPQ * 4)
            beat, tick = divmod(rest, _PPQ)
            out.append(
                f"  {rng.randint(36, 95)} {bar}:{beat}:{tick} 0:0:{length} {rng.randint(1, 127)}"
            )
        return out

    lead = "\n".join(lines(notes))
    pad = "\n".join(lines(ghosts))
    instrument = "INSTRUMENT=Voice\n" if voice else ""
    text = f"""[PROJECT]
ADX_VERSION=2

[CHANNEL Lead]
{instrument}OUTPUT=insert.1

[CHANNEL Pad]
OUTPUT=insert.1

[PATTERN Roll]
LENGTH={bars}:0:0
"""
    if notes:
        text += f"NOTES Lead\n{lead}\n"
    if ghosts:
        text += f"NOTES Pad\n{pad}\n"
    text += """
[PLAYLIST]
TRACK 1
  PATTERN Roll 0:0:0

[MIXER]
INSERT 1 name="Master"
"""
    return text


@pytest.fixture(scope="session")
def ten_k_text() -> str:
    """The 10,000-note pattern phase_5.md's budgets are written for (plus 1,000 ghosts)."""
    return roll_text(10000, bars=64, seed=1, ghosts=1000)


def dispose(widget: object) -> None:
    """Destroy a Qt widget now, while the application still exists, so tests do not
    leave windows (and their engines) for the garbage collector to find at exit."""
    import gc

    import shiboken6
    from PySide6.QtWidgets import QApplication

    if shiboken6.isValid(widget):
        shiboken6.delete(widget)
    # Signal connections to lambdas make reference cycles; collect them now.
    gc.collect()
    QApplication.processEvents()


def pytest_sessionfinish(session: pytest.Session, exitstatus: int) -> None:
    """Tear every Qt widget down before the interpreter does (see :func:`dispose`)."""
    import gc

    from PySide6.QtWidgets import QApplication

    if QApplication.instance() is None:
        return
    for widget in QApplication.topLevelWidgets():
        dispose(widget)
    gc.collect()


@pytest.fixture
def quick(qapp: object) -> None:
    """The scene-graph plugin, loaded; skipped (or, in CI, failed) when it was not built."""
    from adx import engine_bridge

    if not engine_bridge.quick_plugin_path().exists():
        if os.environ.get("CI"):
            pytest.fail("adx_quick was not built in CI: the Qt SDK step failed (phase_5.md 4.3)")
        pytest.skip("adx_quick not built (python tools/fetch_qt.py; pip install -e .)")
    engine_bridge.load_quick_plugin()
