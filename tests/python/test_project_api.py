"""The Project handle: commands, undo, redo, revision - and no per-note objects."""

from __future__ import annotations

import pathlib

import pytest

import adx_engine


@pytest.fixture
def suffocation(repo_root: pathlib.Path) -> adx_engine.Project:
    """suffocation.adx, loaded."""
    path = repo_root / "docs" / "examples" / "suffocation.adx"
    project, _ = adx_engine.Project.load(str(path))
    return project


def test_load_gives_a_clean_history(suffocation: adx_engine.Project) -> None:
    """Loading is not an edit the user can undo past."""
    assert suffocation.history() == []
    assert suffocation.undo() is False


def test_execute_undo_redo(suffocation: adx_engine.Project) -> None:
    """A command is one undo step, and undo restores the exact text."""
    before = suffocation.dumps()
    revision = suffocation.revision

    suffocation.execute(adx_engine.commands.MoveNotes("Lead", "Lead", 120))
    assert suffocation.revision > revision
    assert suffocation.history() == ["Move notes"]
    moved = suffocation.dumps()
    assert moved != before

    assert suffocation.undo() is True
    assert suffocation.dumps() == before
    assert suffocation.redo() is True
    assert suffocation.dumps() == moved


def test_rename_rewrites_references(suffocation: adx_engine.Project) -> None:
    """Paths resolve to ids at load, so a rename updates every path that named it."""
    suffocation.execute(adx_engine.commands.RenameChannel("Lead", "Arp"))
    text = suffocation.dumps()
    assert "[CHANNEL Arp]" in text
    assert "channel.Arp.resfilter.cutoff" in text
    assert "channel.Lead." not in text


def test_a_command_executes_once(suffocation: adx_engine.Project) -> None:
    """Reusing a built command is an error rather than a silent double edit."""
    command = adx_engine.commands.SetChannelVolume("Lead", 0.5)
    suffocation.execute(command)
    with pytest.raises(ValueError, match="already"):
        suffocation.execute(command)


def test_unknown_names_raise(suffocation: adx_engine.Project) -> None:
    """A name that does not resolve is a ValueError, not a no-op."""
    with pytest.raises(ValueError, match="Ghost"):
        suffocation.execute(adx_engine.commands.SetChannelVolume("Ghost", 0.5))


def test_notes_are_counts_not_objects(suffocation: adx_engine.Project) -> None:
    """Rule 2: a count, or one structured array for a whole clip - never a list of note
    objects. (Phase 2 had no notes() at all; Phase 5 adds the array, phase_5.md 4.7.)"""
    assert suffocation.note_count("Kick") == 80
    notes = suffocation.notes("Kick", "Kick")
    assert not isinstance(notes, list)
    assert notes.dtype == adx_engine.NOTE_DTYPE
    assert len(notes) == 80


def test_validate_is_clean(suffocation: adx_engine.Project) -> None:
    """The migrated project satisfies every invariant."""
    assert not [d for d in suffocation.validate() if d["severity"] == "error"]
