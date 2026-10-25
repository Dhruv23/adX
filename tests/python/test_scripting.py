"""The scripting API and the console (phase_7.md 4.7, 6)."""

from __future__ import annotations

import pathlib
import threading
import time

import pytest

EXAMPLES = pathlib.Path(__file__).resolve().parents[2] / "app" / "adx" / "scripting" / "examples"


def example_paths() -> list[pathlib.Path]:
    return sorted(p for p in EXAMPLES.glob("*.py") if not p.name.startswith("_"))


@pytest.mark.usefixtures("qapp")
@pytest.mark.parametrize("path", example_paths(), ids=lambda p: p.stem)
def test_script_examples_run(path: pathlib.Path) -> None:
    """Every example executes and its own asserts - the result it documents - hold."""
    from adx.scripting import api

    api.new()
    exec(compile(path.read_text(encoding="utf-8"), str(path), "exec"), {"adx": api})


@pytest.mark.usefixtures("qapp")
@pytest.mark.parametrize("path", example_paths(), ids=lambda p: p.stem)
def test_script_is_undoable(path: pathlib.Path) -> None:
    """Each example's undo_group undoes cleanly, in one step, to the text before it."""
    from adx.scripting import api

    project = api.new()
    before = project.text()
    exec(compile(path.read_text(encoding="utf-8"), str(path), "exec"), {"adx": api})
    assert project.text() != before
    assert project.undo()
    assert project.text() == before
    assert not project.undo()  # it was one step


@pytest.mark.usefixtures("qapp")
def test_script_issues_commands_only() -> None:
    """A script's edit is an ordinary history entry, labelled as the script named it."""
    from adx.scripting import api

    project = api.new()
    with project.undo_group("my edit"):
        project.add_channel("Lead")
        project.add_pattern("A", bars=1)
    assert project.native.history() == ["my edit"]


@pytest.mark.usefixtures("qapp")
def test_script_errors_are_clear() -> None:
    from adx.scripting import api

    project = api.new()
    with pytest.raises(api.ScriptError):
        project.channel("Nope")
    pattern = project.add_pattern("A", bars=1)
    channel = project.add_channel("Lead")
    with pytest.raises(ValueError, match="mini-notation"):
        pattern.notes(channel).add_mini("[bd sn")
    with pytest.raises(ValueError, match="not a position"):
        project.position("bar one")


def test_script_never_touches_audio_thread(qapp: object) -> None:
    """on_bar callbacks are observed on the main thread only - while the audio runs on its
    own thread."""
    from PySide6.QtWidgets import QApplication

    from adx.scripting import api

    project = api.new()
    channel = project.add_channel("Tone")
    pattern = project.add_pattern("Loop", bars=1)
    pattern.notes(channel).add(60, at="0:0:0", length="0:1:0")
    project.playlist.track(1).place(pattern, at="0:0:0")
    project.set_tempo(960)  # four bars a second, so the test is short
    seen: list[tuple[int, int]] = []

    def record(bar: int) -> None:
        seen.append((bar, threading.get_ident()))

    api.on_bar(record)
    try:
        api.transport.play()
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline and len(seen) < 4:
            QApplication.processEvents()
            time.sleep(0.005)
    finally:
        api.off_bar(record)
        api.transport.stop()
    assert len(seen) >= 4, seen
    assert {ident for _bar, ident in seen} == {threading.main_thread().ident}
    bars = [bar for bar, _ident in seen]
    assert bars == sorted(bars)


def test_console_is_a_repl(qapp: object) -> None:
    """History, multi-line blocks, output and tracebacks, in the app's interpreter."""
    from adx.scripting import api
    from adx.scripting.console import ConsolePanel

    api.new()
    console = ConsolePanel({"adx": api})
    assert not console.run("p = adx.project()")
    assert not console.run("print(len(p.channels()))")
    assert "0\n" in console.output.toPlainText()
    assert console.run("for name in ('A', 'B'):")
    assert console.run("    p.add_channel(name)")
    assert not console.run("")
    assert api.project().channels() == ["A", "B"]
    console.run("1 / 0")
    assert "ZeroDivisionError" in console.output.toPlainText()
    # Completion over the namespace.
    assert console.complete("adx.proj").startswith("adx.project(")
    # History on Up.
    console.input.history.extend(["first", "second"])
    console.input._position = 2
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QKeyEvent

    console.input.keyPressEvent(
        QKeyEvent(QKeyEvent.Type.KeyPress, Qt.Key.Key_Up, Qt.KeyboardModifier.NoModifier)
    )
    assert console.input.text() == "second"
    console.deleteLater()
