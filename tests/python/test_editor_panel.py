"""The editor panel (phase_7.md 4.6, 6): highlighting from engine tokens, diagnostics at
their exact columns, step highlighting that follows the playhead, and the text and the
project kept in step through commands."""

from __future__ import annotations

import pathlib

from PySide6.QtGui import QColor
from PySide6.QtWidgets import QApplication

from adx import engine_bridge
from adx.engine_bridge import PPQ
from adx.mainwindow import MainWindow
from adx.panels.editor.highlighter import to_utf16
from adx.panels.editor.panel import EditorPanel

BAR = PPQ * 4

MINI_PROJECT = """[PROJECT]
ADX_VERSION=2

[CHANNEL Drums]
OUTPUT=insert.1

[CHANNEL Lead]
OUTPUT=insert.1
VOLUME=0.8

[PATTERN Beat]
LENGTH=2:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100
  E4 0:1:0 0:1:0 90
MINI Drums
  [bd sn]*2
  hh(3,8) <cp rim>

[PLAYLIST]
TRACK 1
  PATTERN Beat 1:0:0

[MIXER]
INSERT 1 name="Master"
"""


def corpus(repo_root: pathlib.Path) -> list[pathlib.Path]:
    return [
        *sorted((repo_root / "tests" / "data" / "canonical").glob("*.adx")),
        *sorted((repo_root / "tests" / "golden").glob("*.adx")),
    ]


def open_text(studio: MainWindow, text: str) -> EditorPanel:
    """Open ``text`` as the window's project; returns the editor panel, showing it."""
    project, _diagnostics = engine_bridge.Project.loads(text)
    studio._adopt(project, None)
    panel = studio.editor
    studio.show_panel(panel)
    QApplication.processEvents()
    return panel


def settle(panel: EditorPanel) -> None:
    """Let the panel's debounced refresh run now."""
    QApplication.processEvents()
    panel._refresh_if_stale()


def test_editor_highlight_from_engine_tokens(studio: MainWindow, repo_root: pathlib.Path) -> None:
    """Every coloured range in the editor is exactly an engine token, in its kind's colour,
    over the corpus."""
    panel = open_text(studio, MINI_PROJECT)
    kinds = engine_bridge.semantic_kinds()
    files = corpus(repo_root)
    assert len(files) >= 5
    painted_lines = 0
    for path in files:
        text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
        panel.set_text(text)
        panel.highlighter.rehighlight()
        tokens = engine_bridge.semantic_tokens(text)
        lines = text.split("\n")
        expected: dict[int, list[tuple[int, int, str]]] = {}
        for line, column, length, kind in tokens.tolist():
            start, size = to_utf16(lines[line - 1], column, length)
            colour = QColor(panel.theme.syntax[kinds[kind]]).name()
            expected.setdefault(line - 1, []).append((start, size, colour))
        block = panel.editor.document().begin()
        while block.isValid():
            painted = sorted(
                (r.start, r.length, r.format.foreground().color().name())
                for r in block.layout().formats()
            )
            assert painted == sorted(expected.get(block.blockNumber(), [])), (
                path.name,
                block.blockNumber() + 1,
                block.text(),
            )
            painted_lines += 1
            block = block.next()
    # The tokens are fetched per text change (setPlainText is two: clear, then insert),
    # never per line painted.
    assert panel.highlighter.fetches <= 2 * len(files) + 4
    assert painted_lines > 20 * len(files)


def test_editor_diagnostics_positions(studio: MainWindow) -> None:
    """Squiggles land on exactly the column range the engine reports."""
    panel = open_text(studio, MINI_PROJECT)
    broken = MINI_PROJECT.replace("  E4 0:1:0", "  X4 0:1:0").replace("  hh(3,8)", "  hh(3,8 ")
    panel.editor.setPlainText(broken)
    QApplication.processEvents()
    problems = {(p.line, p.code): p for p in panel.problems}
    lines = broken.split("\n")
    note_line = lines.index("  X4 0:1:0 0:1:0 90") + 1
    assert (note_line, "ADX2008") in problems
    bad = problems[(note_line, "ADX2008")]
    assert bad.column == 3  # at the X, where the note name went wrong
    width = max(bad.length, 1)
    mini = [p for p in panel.problems if p.code == "ADX2015"]
    assert len(mini) == 1
    selections = panel.editor.squiggles
    assert len(selections) == len(panel.problems)
    by_start = {s.cursor.selectionStart(): s for s in selections}
    block = panel.editor.document().findBlockByNumber(note_line - 1)
    squiggle = by_start[block.position() + 2]
    assert squiggle.cursor.selectionEnd() == block.position() + 2 + width
    assert squiggle.format.underlineStyle().name == "WaveUnderline"
    # The problem list navigates on click.
    panel.problem_list.activated.emit(note_line, 3)
    assert panel.editor.textCursor().blockNumber() == note_line - 1
    # Errors block applying.
    assert not panel.apply_now()
    assert studio.project.notes("Beat", "Lead")["pitch"].tolist() == [60, 64]


def test_editor_step_highlight_tracks(studio: MainWindow) -> None:
    """During playback the lit span is the engine's active step - at every frame tested,
    over several cycles - and nothing is lit when stopped."""
    panel = open_text(studio, MINI_PROJECT)
    panel.rebuild_steps()
    assert len(panel.steps.blocks) == 1
    block = panel.steps.blocks[0]
    joined = engine_bridge.mini_join(["[bd sn]*2", "hh(3,8) <cp rim>"])
    lines = panel.text().split("\n")
    first = lines.index("  [bd sn]*2")
    starts = [0, len("[bd sn]*2") + 1]
    for ticks in range(BAR, 3 * BAR, PPQ // 8):  # the placement covers bars 2 and 3
        cycle = ticks // BAR
        compiled = engine_bridge.mini_compile(joined, cycle * BAR, BAR, cycle)
        expected = set()
        for start, length, _pitch, _velocity, span_start, span_length in compiled["notes"]:
            if start <= ticks < start + length:
                row = 1 if span_start >= starts[1] else 0
                expected.add((first + row, 2 + span_start - starts[row], span_length))
        assert set(panel.steps.active(ticks)) == expected, ticks
    # Through the frame clock's path: lit while rolling, cleared when stopped.
    panel.frame(BAR + PPQ // 2, True)
    assert panel.editor.steps
    panel.frame(BAR + PPQ // 2, False)
    assert not panel.editor.steps
    # Outside every placement nothing is lit.
    assert panel.steps.active(0) == []
    assert block.pattern == "Beat"


def test_typing_applies_as_one_undoable_edit(studio: MainWindow) -> None:
    panel = open_text(studio, MINI_PROJECT)
    project = studio.project
    before = project.dumps()
    panel.editor.setPlainText(panel.text().replace("E4 0:1:0 0:1:0 90", "E4 0:1:0 0:1:0 77"))
    assert panel.buffer_dirty
    assert panel.apply_now()
    assert project.notes("Beat", "Lead")["velocity"].tolist() == [100, 77]
    assert project.history()[-1] == "Edit text"
    assert not panel.buffer_dirty
    # Undo is an ordinary undo, and the text follows it.
    assert studio.session.undo()
    settle(panel)
    assert project.dumps() == before
    assert "E4 0:1:0 0:1:0 90" in panel.text()


def test_project_edit_reloads_text_keeping_the_cursor(studio: MainWindow) -> None:
    panel = open_text(studio, MINI_PROJECT)
    panel.editor.go_to(12, 4)
    channel = next(c for c in studio.project.channels_info() if c["name"] == "Lead")
    assert studio.session.build_and_execute("SetChannelField", int(channel["id"]), "volume", 0.5)
    settle(panel)
    assert "VOLUME=0.5" in panel.text()
    cursor = panel.editor.textCursor()
    assert (cursor.blockNumber(), cursor.positionInBlock()) == (11, 3)


def test_highlighting_is_not_an_edit(studio: MainWindow) -> None:
    """The highlighter's deferred first pass changes formats, which Qt reports as a text
    change. It must not mark the text edited - an apply timer started by it once wrote
    stale text over an edit made in another panel."""
    panel = open_text(studio, MINI_PROJECT)
    panel.highlighter.rehighlight()
    QApplication.processEvents()
    assert not panel.buffer_dirty
    assert not panel._apply_timer.isActive()


def test_unapplied_text_is_never_overwritten(studio: MainWindow) -> None:
    panel = open_text(studio, MINI_PROJECT)
    panel.live.setChecked(False)
    mine = panel.text().replace("VOLUME=0.8", "VOLUME=0.6")
    panel.editor.setPlainText(mine)
    channel = next(c for c in studio.project.channels_info() if c["name"] == "Lead")
    assert studio.session.build_and_execute("SetChannelField", int(channel["id"]), "pan", -0.5)
    settle(panel)
    assert panel.text() == mine
    assert not panel.banner.isHidden()
    panel.load_from_project()
    assert "PAN=-0.5" in panel.text()
    assert panel.banner.isHidden()


def test_revert_section(studio: MainWindow) -> None:
    panel = open_text(studio, MINI_PROJECT)
    panel.live.setChecked(False)
    edited = panel.text().replace("VOLUME=0.8", "VOLUME=0.3").replace("  C4 0:0:0", "  D4 0:0:0")
    panel.editor.setPlainText(edited)
    line = edited.split("\n").index("VOLUME=0.3")
    panel.editor.go_to(line + 1)
    panel.revert_section()
    assert "VOLUME=0.8" in panel.text()  # the channel section came back
    assert "  D4 0:0:0" in panel.text()  # the pattern's edit was left alone


def test_completion_from_the_engine(studio: MainWindow) -> None:
    panel = open_text(studio, MINI_PROJECT)
    project = studio.project
    words = panel.completions.candidates("CHANNEL", "INST", "INST", project, False)
    assert "INSTRUMENT=" in words
    paths = panel.completions.candidates(
        "PATTERN", "AUTOMATION channel.Lead.f", "channel.Lead.f", project, False
    )
    assert "channel.Lead.filter.cutoff" in paths
    types = panel.completions.candidates("CHANNEL", "INSTRUMENT=v", "v", project, False)
    assert "va" in types
    mini = panel.completions.candidates("PATTERN", "  b", "b", project, True)
    assert "bd" in mini


def test_cursor_link_with_the_piano_roll(studio: MainWindow) -> None:
    panel = open_text(studio, MINI_PROJECT)
    roll = studio.piano_roll
    roll.select_clip("Beat", "Lead")
    assert roll.model is not None
    notes = roll.model.notes()
    second = int(notes["id"][notes["pitch"] == 64][0])
    # The roll selects a note: its line comes into view (the keyboard stays put).
    roll.model.select([second])
    roll.selection_changed()
    line = panel.text().split("\n").index("  E4 0:1:0 0:1:0 90")
    assert panel.editor.textCursor().blockNumber() == line
    # The cursor on a note line selects that note in the roll. (The panel does this for
    # cursor moves a person makes - with the editor focused - which the offscreen
    # platform cannot simulate, so the link is driven directly.)
    panel.editor.go_to(line, focus=False)  # the C4 line
    panel.link_cursor()
    first = int(notes["id"][notes["pitch"] == 60][0])
    assert roll.model.selection.tolist() == [first]
