"""Shared fixtures for the Python suite."""

from __future__ import annotations

import os
import pathlib
import random
from collections.abc import Iterator

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


@pytest.fixture
def studio(quick: None, tmp_path: pathlib.Path) -> Iterator[object]:
    """The whole application on the null backend, a new project open, settings in
    ``tmp_path`` (phase_6.md 5: panel tests drive real widgets). Yields the MainWindow."""
    from PySide6.QtCore import QSettings

    from adx.application import create

    settings = QSettings(str(tmp_path / "settings.ini"), QSettings.Format.IniFormat)
    _app, main = create(null_audio=True, settings=settings)
    main.clock.stop()
    main.new_project()
    yield main
    main.clock.stop()
    main.engine.stop()
    main.saved_revision = main.project.revision() if main.project else 0
    main.close()
    dispose(main)


#: Seconds one test may take before the watchdog ends the run. The slowest test takes
#: well under a minute; ADX_TEST_TIMEOUT overrides it (0 turns the watchdog off).
#: pytest's own faulthandler_timeout (pyproject.toml) dumps every thread's stack first.
_TEST_TIMEOUT = float(os.environ.get("ADX_TEST_TIMEOUT", "300"))

_config: list[pytest.Config] = []


def pytest_configure(config: pytest.Config) -> None:
    _config.append(config)


def _hung(nodeid: str) -> None:
    """The watchdog fired: say which test hung and where, then end the process. A hang
    otherwise prints nothing at all until the CI job's own limit kills it."""
    import sys
    import threading
    import traceback

    # Output is being captured into a file that os._exit would throw away.
    capture = _config[0].pluginmanager.getplugin("capturemanager") if _config else None
    if capture is not None:
        capture.suspend_global_capture(in_=False)
    main = sys._current_frames().get(threading.main_thread().ident or 0)
    frames = traceback.extract_stack(main)[-8:] if main is not None else []
    where = " <- ".join(
        f"{pathlib.Path(f.filename).name}:{f.lineno} {f.name}" for f in reversed(frames)
    )
    message = f"{nodeid} hung for {_TEST_TIMEOUT:.0f} s at: {where or 'unknown'}"
    if os.environ.get("GITHUB_ACTIONS"):
        message = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print(f"\n::error title=pytest watchdog::{message}", flush=True)
    else:
        print(f"\n{message}", file=sys.stderr, flush=True)
    os._exit(1)


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_protocol(item: pytest.Item, nextitem: pytest.Item | None) -> Iterator[None]:
    """Run each test (setup, call and teardown) under the watchdog."""
    if _TEST_TIMEOUT <= 0:
        yield
        return
    import threading

    timer = threading.Timer(_TEST_TIMEOUT, _hung, args=(item.nodeid,))
    timer.daemon = True
    timer.start()
    try:
        yield
    finally:
        timer.cancel()


@pytest.fixture(autouse=True)
def no_modal_dialogs(monkeypatch: pytest.MonkeyPatch) -> None:
    """A modal dialog in a test waits for a click that never comes, and the run hangs
    (Phase 7's first push sat for six hours on one). Make opening one fail instead."""
    from PySide6.QtWidgets import QDialog, QFileDialog, QMessageBox

    def refuse(*args: object, **_kwargs: object) -> None:
        text = " / ".join(str(a) for a in args if isinstance(a, str))
        raise RuntimeError(f"a modal dialog opened during a test: {text or args[:1]}")

    for name in ("question", "warning", "critical", "information"):
        monkeypatch.setattr(QMessageBox, name, staticmethod(refuse))
    for name in ("getOpenFileName", "getSaveFileName", "getExistingDirectory"):
        monkeypatch.setattr(QFileDialog, name, staticmethod(refuse))
    monkeypatch.setattr(QMessageBox, "exec", refuse)
    monkeypatch.setattr(QDialog, "exec", refuse)


def pytest_runtest_logreport(report: pytest.TestReport) -> None:
    """On GitHub Actions, each failure is also an ``::error`` annotation: it shows on the
    run's summary page, and the public API returns it, so a failure can be read without
    downloading the (authenticated) job log."""
    if not os.environ.get("GITHUB_ACTIONS") or not report.failed:
        return
    path, line, _name = report.location
    text = str(report.longrepr) if report.longrepr is not None else "failed"
    lines = [ln for ln in text.splitlines() if ln.startswith("E ")] or text.splitlines()[-3:]
    message = " | ".join(ln.strip() for ln in lines[:6])[:900]
    # Workflow commands escape %, CR and LF in the message.
    message = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    where = f"file={path.replace(chr(92), '/')},line={(line or 0) + 1}"
    print(f"\n::error {where}::{report.nodeid} [{report.when}]: {message}")
