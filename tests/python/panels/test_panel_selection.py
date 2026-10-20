"""The global selection and the four operations over it (phase_6.md 4.7, 5:
``selection_uniform_ops`` and ``clipboard_is_adx_text``)."""

from __future__ import annotations

from collections.abc import Callable

import numpy as np
import pytest
from PySide6.QtWidgets import QApplication

from adx import engine_bridge
from adx.engine_bridge import PPQ, Project
from adx.mainwindow import MainWindow
from adx.selection import SELECTABLE, Context, Kind


def add_notes(studio: MainWindow, pitches: list[int]) -> list[int]:
    notes = engine_bridge.empty_notes(len(pitches))
    notes["start"] = [i * PPQ for i in range(len(pitches))]
    notes["length"] = PPQ // 2
    notes["pitch"] = pitches
    notes["velocity"] = 100
    studio.session.execute(
        engine_bridge.commands.edit_notes(
            "P1",
            "Lead",
            np.zeros(0, np.uint32),
            engine_bridge.empty_notes(),
            notes,
            "Add",
        )
    )
    return [int(i) for i in studio.project.notes("P1", "Lead")["id"]]


def setup(studio: MainWindow, kind: Kind) -> tuple[tuple[int, ...], Context, Callable[[], int]]:
    """Something of ``kind`` to select: (ids, context, how many such things exist)."""
    project, session = studio.project, studio.session
    if kind is Kind.NOTES:
        ids = add_notes(studio, [60, 64])
        return (ids[0],), Context("P1", "Lead"), lambda: len(project.notes("P1", "Lead"))
    if kind is Kind.CLIPS:
        item = int(project.playlist_items()[0]["item"])
        return (item,), Context(), lambda: len(project.playlist_items())
    if kind is Kind.POINTS:
        points = np.array(
            [[0, 0.2, 0, 0, 0.33, 0, 0.67, 1], [PPQ * 4, 0.8, 0, 0, 0.33, 0, 0.67, 1]],
            dtype=np.float64,
        )
        session.build_and_execute("AddAutomationClip", 0, "channel.Lead.volume", points)
        clip = int(project.automation_clips()[-1]["id"])
        return (1,), Context(clip=clip), lambda: len(project.automation_points(clip))
    if kind is Kind.CHANNELS:
        session.build_and_execute("AddChannelNew", "Bass", "va", 0, "")
        bass = int(project.channels_info()[-1]["id"])
        return (bass,), Context(), lambda: len(project.channels_info())
    master = int(project.mixer_info()["master"])
    if kind is Kind.INSERTS:
        insert = studio.mixer.add_insert()
        return (insert,), Context(), lambda: len(project.mixer_info()["inserts"])
    session.build_and_execute("AddSlot", master, "Delay")
    slot = int(project.mixer_info()["inserts"][0]["slots"][-1]["id"])
    return (
        (slot,),
        Context(insert=master),
        lambda: len(project.mixer_info()["inserts"][0]["slots"]),
    )


@pytest.mark.parametrize("kind", SELECTABLE, ids=lambda k: k.value)
def test_selection_uniform_ops(studio: MainWindow, kind: Kind) -> None:
    ids, context, count = setup(studio, kind)
    ops, project, actions = studio.ops, studio.project, studio.registry
    studio.selection.select(kind, ids, context)
    start = count()

    def step(action: str) -> None:
        depth = project.undo_depth()
        assert actions.trigger(action), action
        assert project.undo_depth() == depth + 1, action

    # Duplicate: one more, one step; undo puts it back.
    step("edit.duplicate")
    assert count() == start + 1
    studio.session.undo()
    assert count() == start
    # Copy then paste: one more, one step.
    studio.selection.select(kind, ids, context)
    ops.paste_tick = PPQ * 8
    assert actions.trigger("edit.copy")
    step("edit.paste")
    assert count() == start + 1
    studio.session.undo()
    # Delete: one fewer, one step; the selection is emptied.
    studio.selection.select(kind, ids, context)
    step("edit.delete")
    assert count() == start - 1
    assert studio.selection.kind is Kind.NONE
    studio.session.undo()
    assert count() == start


def test_clipboard_is_adx_text(studio: MainWindow) -> None:
    ids = add_notes(studio, [60, 64, 67])
    studio.selection.select(Kind.NOTES, ids, Context("P1", "Lead"))
    assert studio.registry.trigger("edit.copy")
    text = QApplication.clipboard().text()
    # The clipboard is a project a text editor can hold, and it parses cleanly.
    assert "ADX_VERSION=2" in text
    fragment, diagnostics = Project.loads(text)
    assert not [d for d in diagnostics if str(d["severity"]) == "error"]
    pattern = fragment.patterns()[0]
    assert sorted(
        int(p) for p in fragment.notes(pattern, fragment.pattern_channels(pattern)[0])["pitch"]
    ) == [60, 64, 67]
    # Edit it as text - the C4 becomes a D4 - and paste it back.
    edited = text.replace("\n  C4 ", "\n  D4 ")
    assert edited != text
    QApplication.clipboard().setText(edited)
    studio.ops.paste_tick = PPQ * 8
    before = studio.project.undo_depth()
    assert studio.registry.trigger("edit.paste")
    assert studio.project.undo_depth() == before + 1
    notes = studio.project.notes("P1", "Lead")
    pasted = notes[notes["start"] >= PPQ * 8]
    assert sorted(int(p) for p in pasted["pitch"]) == [62, 64, 67]
