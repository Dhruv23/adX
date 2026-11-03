"""The control channel (phase_10.5.md 4.2, 5): off by default, token-checked, and its
input real - a driver's click lands in the same handlers a mouse does.

In-process tests drive a server in this test's own window from a worker thread while the
main thread runs the event loop, as the app's would; the rest launch adX."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import threading
import time
from collections.abc import Callable
from typing import Any

import numpy as np
import pytest
from PySide6.QtCore import QCoreApplication

import ui_input
from adx.control.inspect import INTERACTIVE, ensure_names
from adx.control.server import ControlServer, requested
from adx.mainwindow import MainWindow
from adx.panels.channel_rack.channel_row import ROW_HEIGHT
from adx.panels.geometry_view import Pointer
from adx.testing.driver import Driver, DriverError

ROOT = pathlib.Path(__file__).resolve().parents[2]


def drive[T](
    studio: MainWindow, tmp_path: pathlib.Path, work: Callable[[Driver], T], timeout: float = 60.0
) -> T:
    """Run ``work`` against a server in ``studio`` from a thread, pumping events here."""
    file = tmp_path / "control.json"
    server = ControlServer(studio, str(file))
    outcome: dict[str, Any] = {}

    def run() -> None:
        try:
            with_driver = Driver(file)
            try:
                outcome["value"] = work(with_driver)
            finally:
                with_driver._channel.close()
        except BaseException as error:
            outcome["error"] = error

    thread = threading.Thread(target=run)
    thread.start()
    deadline = time.monotonic() + timeout
    while thread.is_alive() and time.monotonic() < deadline:
        QCoreApplication.processEvents()
        time.sleep(0.002)
    thread.join(1.0)
    server.close()
    if "error" in outcome:
        raise outcome["error"]
    assert not thread.is_alive(), "the driver did not finish"
    return outcome.get("value")


def test_control_off_by_default(tmp_path: pathlib.Path) -> None:
    """No pipe exists without --control or ADX_CONTROL."""
    environment = {
        k: v for k, v in os.environ.items() if k not in ("ADX_CONTROL", "ADX_CONTROL_FILE")
    }
    assert not requested() or "ADX_CONTROL" in os.environ
    report = tmp_path / "smoke.json"
    environment.update(ADX_SMOKE=str(report), ADX_SETTINGS_DIR=str(tmp_path), ADX_NO_QUICKSTART="1")
    process = subprocess.Popen(
        [sys.executable, "-m", "adx", "gui", "--null-audio"], env=environment, cwd=ROOT
    )
    pipes_seen: set[str] = set()
    deadline = time.monotonic() + 90
    while process.poll() is None and time.monotonic() < deadline:
        if sys.platform == "win32":
            pipes_seen |= {
                p for p in os.listdir("\\\\.\\pipe\\") if p.startswith(f"adx-control-{process.pid}")
            }
        time.sleep(0.2)
    process.wait(30)
    facts = json.loads(report.read_text(encoding="utf-8"))
    assert facts["control"] is False
    assert pipes_seen == set()


def test_control_handshake(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    file = tmp_path / "control.json"
    server = ControlServer(studio, str(file))
    facts = json.loads(file.read_text(encoding="utf-8"))
    assert facts["name"].startswith(f"adx-control-{os.getpid()}-")
    wrong = dict(facts, token="0" * 32)
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps(wrong), encoding="utf-8")
    errors: list[BaseException] = []

    def connect_wrong() -> None:
        try:
            Driver(bad)
        except BaseException as error:
            errors.append(error)

    thread = threading.Thread(target=connect_wrong)
    thread.start()
    while thread.is_alive():
        QCoreApplication.processEvents()
        time.sleep(0.002)
    server.close()
    assert errors
    assert isinstance(errors[0], DriverError)
    assert errors[0].code in ("bad-token", "closed")
    assert not file.exists()  # the control file goes with the server
    # The right token is accepted and versioned.
    version = drive(studio, tmp_path, lambda d: d.version)
    assert version


def test_control_input_is_real(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    """A driver click on a channel rack step produces the same history entry as the
    in-process gesture through the view's input."""
    studio.show()
    rack = studio.channel_rack
    rack.select_pattern("P1")
    rack.raise_()
    QCoreApplication.processEvents()
    grid = rack.grid
    x = 4.5 * grid.state.cell_width
    y = 0.5 * ROW_HEIGHT
    # In process, through the view's forward object.
    ui_input.click(grid.view, Pointer(x, y))
    in_process = studio.project.history()[-1]
    notes_in_process = len(studio.project.notes("P1", "Lead"))
    assert studio.project.undo()
    ensure_names(studio)

    def click(d: Driver) -> dict[str, Any]:
        d.click(grid.view.objectName(), x=x, y=y)
        return d.state()

    state = drive(studio, tmp_path, click)
    assert state["project"]["history"][-1] == in_process
    assert len(studio.project.notes("P1", "Lead")) == notes_in_process


def test_control_actions_cover_registry(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    listed = drive(studio, tmp_path, lambda d: d.actions())
    ids = [a["id"] for a in listed]
    assert ids == [spec.id for spec in studio.registry.specs()]

    # Triggerable: a harmless pair, through the channel, takes effect.
    def toggle(d: Driver) -> tuple[bool, bool]:
        before = d.state()["transport"]["playing"]
        d.trigger("transport.play")
        d.wait_for("transport.playing", predicate="equals", value=not before, timeout=5)
        d.trigger("transport.play")
        d.wait_for("transport.playing", predicate="equals", value=before, timeout=5)
        return before, d.state()["transport"]["playing"]

    studio.clock.start()
    first, last = drive(studio, tmp_path, toggle)
    assert first == last
    with pytest.raises(DriverError):
        drive(studio, tmp_path, lambda d: d.trigger("no.such.action"))


def test_control_wait_for_times_out(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    def wait(d: Driver) -> DriverError:
        try:
            d.wait_for("project.undo_depth", predicate="gte", value=999, timeout=0.3)
        except DriverError as error:
            return error
        raise AssertionError("wait_for did not time out")

    started = time.monotonic()
    error = drive(studio, tmp_path, wait)
    assert time.monotonic() - started < 5
    assert error.code == "timeout"
    assert "last value" in str(error)


def test_every_interactive_widget_is_named(studio: MainWindow) -> None:
    studio.show()
    QCoreApplication.processEvents()
    ensure_names(studio)
    unnamed: list[str] = []
    for kind in INTERACTIVE:
        unnamed += [type(w).__name__ for w in studio.findChildren(kind) if not w.objectName()]
    assert unnamed == []


def test_control_tree_names_stable(tmp_path: pathlib.Path) -> None:
    """Every interactive widget reachable from the window has a name, and the same names
    come back on a relaunch."""
    # Every interactive widget, shown or not: which ones are on screen depends on the
    # layout (a toolbar's overflow, which docks are tabbed together), and that is not what
    # this test is about. QMainWindow's own tab bars are left out for the same reason: Qt
    # makes one per tabbed dock group (STATE.md P10.5-3).
    listing = (
        "from adx.control import inspect\n"
        "result = sorted(inspect.path_of(w) for w in inspect._widgets(inspect._roots())\n"
        "    if isinstance(w, inspect.INTERACTIVE)\n"
        "    and not (w.parent() is window and w.objectName().startswith('window.tabs.')))"
    )
    names = []
    for run in range(2):
        with Driver.launch(via="gui", settings_dir=tmp_path / f"s{run}") as app:
            tree = app.tree()
            assert all(w["name"] for w in tree), [w for w in tree if not w["name"]][:5]
            names.append(app.script(listing))
    assert names[0] == names[1]


def test_piano_roll_lists_new_patterns_and_channels(studio: MainWindow) -> None:
    """A pattern or channel made after the project opened can be chosen in the piano
    roll (its boxes were filled once, at open: found by the build-track scenario)."""
    roll = studio.piano_roll
    assert studio.session.build_and_execute("AddPatternNew", "Later", 3840 * 16, "")
    assert studio.session.build_and_execute("AddChannelNew", "Extra", "additive", 0, "")
    patterns = [roll.pattern_box.itemText(i) for i in range(roll.pattern_box.count())]
    channels = [roll.channel_box.itemText(i) for i in range(roll.channel_box.count())]
    assert "Later" in patterns
    assert "Extra" in channels


def test_inspector_survives_enter_in_its_own_field(studio: MainWindow) -> None:
    """Enter in the inspector's Velocity applies it, which rebuilds the form: the field
    the key went to must outlive the key (found by the build-track scenario, where the
    release landed on a deleted spin box)."""
    from PySide6.QtCore import Qt
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QDoubleSpinBox

    from adx.engine_bridge import NOTE_DTYPE, commands
    from adx.selection import Context, Kind

    project = studio.project
    note = np.zeros(1, dtype=NOTE_DTYPE)
    note["length"], note["pitch"], note["velocity"] = 960, 60, 100
    assert studio.session.execute(
        commands.edit_notes(
            "P1", "Lead", np.zeros(0, np.uint32), np.zeros(0, dtype=NOTE_DTYPE), note, "add"
        )
    )
    ids = [int(i) for i in project.notes("P1", "Lead")["id"]]
    studio.show()
    studio.selection.select(Kind.NOTES, ids, Context(pattern="P1", channel="Lead"))
    QCoreApplication.processEvents()
    spin = studio.findChild(QDoubleSpinBox, "inspector.velocity")
    assert spin is not None
    spin.setFocus()
    spin.lineEdit().selectAll()
    QTest.keyClicks(spin, "88")
    QTest.keyClick(spin, Qt.Key.Key_Return)
    QCoreApplication.processEvents()
    assert int(project.notes("P1", "Lead")["velocity"][0]) == 88
