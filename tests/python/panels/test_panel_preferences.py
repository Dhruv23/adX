"""Preferences > Shortcuts (phase_11.6.md 4.4, 5)."""

from __future__ import annotations

from PySide6.QtCore import QSettings
from PySide6.QtGui import QKeySequence

from adx.mainwindow import MainWindow
from adx.panels.preferences.dialog import PreferencesDialog
from adx.shortcuts import Keymap


def open_page(studio: MainWindow) -> PreferencesDialog:
    dialog = PreferencesDialog(studio.prefs, 5.0, studio, registry=studio.registry)
    assert dialog.shortcuts is not None
    return dialog


def test_shortcuts_page_lists_every_action(studio: MainWindow) -> None:
    dialog = open_page(studio)
    page = dialog.shortcuts
    assert page is not None
    specs = list(studio.registry.specs())
    assert page.table.rowCount() == len(specs)
    for spec in specs:
        assert page.shown_shortcut(spec.id) == studio.keymap.shortcut(spec.id)
    assert page.shown_shortcut("piano_roll.tool.cut") == "0"
    page.search.setText("piano roll")
    assert 0 < page.visible_rows() < len(specs)
    page.search.setText("")
    assert page.visible_rows() == len(specs)
    dialog.deleteLater()


def test_shortcuts_page_rebind_persists(studio: MainWindow) -> None:
    dialog = open_page(studio)
    page = dialog.shortcuts
    assert page is not None
    page.edits["edit.duplicate"].setKeySequence(QKeySequence("Ctrl+Shift+D"))
    page.edits["edit.duplicate"].editingFinished.emit()
    dialog.reject()  # rebinds are saved as they are made, not on OK
    assert studio.keymap.shortcut("edit.duplicate") == "Ctrl+Shift+D"
    assert studio.registry.action("edit.duplicate").shortcut().toString() == "Ctrl+Shift+D"
    again = open_page(studio)
    assert again.shortcuts is not None
    assert again.shortcuts.shown_shortcut("edit.duplicate") == "Ctrl+Shift+D"
    # And in a fresh keymap over the same settings file: it survives a restart.
    studio.settings.sync()
    fresh = Keymap(QSettings(studio.settings.fileName(), QSettings.Format.IniFormat))
    fresh.declare("edit.duplicate", "Ctrl+D")
    assert fresh.shortcut("edit.duplicate") == "Ctrl+Shift+D"
    # Reset puts the default back.
    again.shortcuts.reset("edit.duplicate")
    assert studio.keymap.shortcut("edit.duplicate") == "Ctrl+D"
    again.deleteLater()
    dialog.deleteLater()


def test_shortcuts_page_flags_conflicts(studio: MainWindow) -> None:
    dialog = open_page(studio)
    page = dialog.shortcuts
    assert page is not None
    assert not page.conflict_marked("piano_roll.tool.cut")
    page.rebind("piano_roll.tool.cut", "2")  # the Paint tool's key, same panel
    assert page.conflict_marked("piano_roll.tool.cut")
    assert page.conflict_marked("piano_roll.tool.paint")
    # A playlist key may reuse it: different panel, no conflict.
    playlist = [s.id for s in studio.registry.specs() if s.scope == "playlist"]
    assert not any(page.conflict_marked(action_id) for action_id in playlist)
    page.rebind("piano_roll.tool.cut", "0")
    assert not page.conflict_marked("piano_roll.tool.cut")
    assert not page.conflict_marked("piano_roll.tool.paint")
    page.rebind("edit.copy", "Ctrl+V")
    assert page.conflict_marked("edit.copy")
    assert page.conflict_marked("edit.paste")
    page.reset_all()
    assert studio.keymap.conflicts() == {}
    assert page.shown_shortcut("edit.copy") == "Ctrl+C"
    dialog.deleteLater()
