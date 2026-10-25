"""Hot reload in the app (phase_7.md 4.1, 4.4): the file changes on disk and the open
project follows - patched, one undo step, never over unsaved edits without asking."""

from __future__ import annotations

import pathlib
import time
from collections.abc import Callable

from PySide6.QtWidgets import QApplication

from adx.mainwindow import MainWindow
from adx.text_sync import write_atomically

TEXT = """[PROJECT]
ADX_VERSION=2

[CHANNEL Lead]
OUTPUT=insert.1
VOLUME=0.8

[PATTERN Verse]
LENGTH=1:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100
  E4 0:1:0 0:1:0 90

[PLAYLIST]
TRACK 1
  PATTERN Verse 0:0:0

[MIXER]
INSERT 1 name="Master"
"""


def pump_until(condition: Callable[[], bool], seconds: float = 3.0) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        QApplication.processEvents()
        if condition():
            return True
        time.sleep(0.005)
    QApplication.processEvents()
    return condition()


def pump(seconds: float) -> None:
    pump_until(lambda: False, seconds)


def open_file(studio: MainWindow, tmp_path: pathlib.Path, text: str = TEXT) -> pathlib.Path:
    path = tmp_path / "song.adx"
    path.write_text(text, encoding="utf-8", newline="")
    studio.text.file_sync.debounce_ms = 40
    # Throw away whatever is open, or opening asks to save it - a modal box that
    # nobody answers.
    if studio.project is not None:
        studio.saved_revision = studio.project.revision()
    assert studio.open_project(str(path))
    return path


def volume(studio: MainWindow) -> float:
    return float(next(c for c in studio.project.channels_info() if c["name"] == "Lead")["volume"])


def test_external_edit_reloads(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    path = open_file(studio, tmp_path)
    write_atomically(str(path), TEXT.replace("VOLUME=0.8", "VOLUME=0.4"))
    assert pump_until(lambda: abs(volume(studio) - 0.4) < 1e-6)
    assert studio.project.history() == ["Reload from file"]
    assert not studio._dirty()
    # The editor panel follows.
    studio.editor._refresh_if_stale()
    assert "VOLUME=0.4" in studio.editor.text()
    # An in-place write (how VS Code saves by default) is seen too.
    path.write_text(TEXT.replace("VOLUME=0.8", "VOLUME=0.3"), encoding="utf-8", newline="")
    assert pump_until(lambda: abs(volume(studio) - 0.3) < 1e-6)
    # Undo takes a reload back like any edit.
    assert studio.session.undo()
    assert abs(volume(studio) - 0.4) < 1e-6


def test_external_tempo_edit_shows_in_the_transport(
    studio: MainWindow, tmp_path: pathlib.Path
) -> None:
    tempo = TEXT.replace("[CHANNEL Lead]", "[TEMPO]\n0:0:0 120\n\n[CHANNEL Lead]")
    for mode in ("on-save", "live"):
        studio.text.set_mode(mode)
        path = open_file(studio, tmp_path, tempo)
        assert studio.transport.tempo.value() == 120
        path.write_text(tempo.replace("0:0:0 120", "0:0:0 121"), encoding="utf-8", newline="")
        assert pump_until(lambda: studio.project.bpm_at(0) == 121), mode
        assert studio.transport.tempo.value() == 121, mode
        # Nothing writes the old tempo back over the edit.
        pump(0.6)
        assert "0:0:0 121" in path.read_text(encoding="utf-8"), mode
        # Undo moves the box back too.
        assert studio.session.undo()
        assert studio.transport.tempo.value() == 120, mode


def test_invalid_file_is_shown_not_applied(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    path = open_file(studio, tmp_path)
    messages: list[tuple[int, str]] = []
    studio.text.file_sync.invalid.connect(lambda line, message: messages.append((line, message)))
    path.write_text(TEXT.replace("[CHANNEL Lead]", "[CHANNEL Lead"), encoding="utf-8")
    assert pump_until(lambda: bool(messages))
    assert messages[0][0] == 4
    assert studio.project.channels() == ["Lead"]
    assert studio.project.history() == []


def test_conflict_is_raised_never_guessed(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    path = open_file(studio, tmp_path)
    layer = studio.text
    layer.file_sync.conflict.disconnect(layer._conflict)
    conflicts: list[str] = []
    layer.file_sync.conflict.connect(conflicts.append)
    lead = next(c for c in studio.project.channels_info() if c["name"] == "Lead")
    assert studio.session.build_and_execute("SetChannelField", int(lead["id"]), "pan", -0.5)
    theirs = TEXT.replace("VOLUME=0.8", "VOLUME=0.2")
    write_atomically(str(path), theirs)
    assert pump_until(lambda: bool(conflicts))
    # Neither side dropped: the project kept its edit, the file kept its own.
    assert "PAN=-0.5" in studio.project.dumps()
    assert path.read_text(encoding="utf-8") == theirs
    assert "this window" in layer.diff_text(theirs)
    # Take theirs: the file's text, and the discarded edit is one undo away.
    assert layer.file_sync.take_theirs()
    assert abs(volume(studio) - 0.2) < 1e-6
    assert "PAN=-0.5" not in studio.project.dumps()
    # A second conflict, resolved the other way: keep mine writes the project out.
    assert studio.session.build_and_execute("SetChannelField", int(lead["id"]), "pan", 0.25)
    write_atomically(str(path), theirs.replace("VOLUME=0.2", "VOLUME=0.9"))
    assert pump_until(lambda: len(conflicts) == 2)
    layer.file_sync.keep_mine()
    written = path.read_text(encoding="utf-8")
    assert "PAN=0.25" in written
    assert "VOLUME=0.2" in written
    pump(0.3)  # the watcher hears that write as adX's own, and does nothing
    assert len(conflicts) == 2


def test_live_mode_writes_and_ignores_its_own_echo(
    studio: MainWindow, tmp_path: pathlib.Path
) -> None:
    path = open_file(studio, tmp_path)
    layer = studio.text
    layer.set_mode("live")
    reloads: list[int] = []
    layer.file_sync.reloaded.connect(lambda commands, _structural: reloads.append(commands))
    lead = next(c for c in studio.project.channels_info() if c["name"] == "Lead")
    assert studio.session.build_and_execute("SetChannelField", int(lead["id"]), "volume", 0.55)
    assert pump_until(lambda: "VOLUME=0.55" in path.read_text(encoding="utf-8"))
    assert path.read_text(encoding="utf-8") == layer.file_sync.sync.render_text()
    pump(0.4)
    assert reloads == []  # its own write was recognised
    assert not studio._dirty()


def test_off_mode_ignores_the_file(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    path = open_file(studio, tmp_path)
    studio.text.set_mode("off")
    write_atomically(str(path), TEXT.replace("VOLUME=0.8", "VOLUME=0.1"))
    pump(0.4)
    assert abs(volume(studio) - 0.8) < 1e-6


def test_reload_keeps_playing(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    path = open_file(studio, tmp_path)
    studio.engine.play()
    assert pump_until(lambda: studio.engine.state() == "playing")
    for value in ("0.5", "0.6", "0.7"):
        write_atomically(str(path), TEXT.replace("VOLUME=0.8", f"VOLUME={value}"))
        assert pump_until(lambda value=value: abs(volume(studio) - float(value)) < 1e-6)
        assert studio.engine.state() == "playing"
    studio.engine.stop_playback()
