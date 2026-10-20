"""Every action the main window offers, declared once (phase_6.md 4.10).

The menu bar is built from this list; the keymap holds each one's shortcut; the
enablement predicates are evaluated when a menu opens and whenever the selection or the
project changes. Panel-scoped actions (the playlist's tools) carry their panel's id and
work only while it has focus.
"""

from __future__ import annotations

from functools import partial
from typing import TYPE_CHECKING

from adx.actions import ActionRegistry, ActionSpec

if TYPE_CHECKING:
    from adx.mainwindow import MainWindow
    from adx.panels.base import DockPanel


def _toggle_visible(panel: DockPanel) -> None:
    """Show a hidden panel, hide a shown one."""
    panel.setVisible(not panel.isVisible())


def register_actions(window: MainWindow, registry: ActionRegistry) -> None:
    """Declare every action of the main window."""
    w = window
    has_project = lambda: w.session.project is not None  # noqa: E731
    selected = lambda: bool(w.selection.current)  # noqa: E731
    can_undo = lambda: w.session.project is not None and w.session.project.undo_depth() > 0  # noqa: E731
    specs = [
        ActionSpec("file.new", "&New", w.new_project, "&File", "Ctrl+N"),
        ActionSpec("file.open", "&Open…", w.open_dialog, "&File", "Ctrl+O"),
        ActionSpec("file.save", "&Save", w.save, "&File", "Ctrl+S", has_project),
        ActionSpec("file.save_as", "Save &As…", w.save_as, "&File", "Ctrl+Shift+S", has_project),
        ActionSpec(
            "file.export",
            "&Export WAV…",
            w.export_dialog,
            "&File",
            "Ctrl+E",
            has_project,
            separator=True,
        ),
        ActionSpec(
            "file.preferences",
            "&Preferences…",
            w.preferences_dialog,
            "&File",
            "Ctrl+,",
            separator=True,
        ),
        ActionSpec("file.quit", "&Quit", w.close, "&File", "Ctrl+Q", separator=True),
        ActionSpec("edit.undo", "&Undo", w.undo, "&Edit", "Ctrl+Z", can_undo),
        ActionSpec("edit.redo", "&Redo", w.redo, "&Edit", "Ctrl+Y", has_project),
        ActionSpec("edit.copy", "&Copy", w.ops.copy, "&Edit", "Ctrl+C", selected, separator=True),
        ActionSpec("edit.paste", "&Paste", w.ops.paste, "&Edit", "Ctrl+V", has_project),
        ActionSpec("edit.duplicate", "D&uplicate", w.ops.duplicate, "&Edit", "Ctrl+D", selected),
        ActionSpec("edit.delete", "&Delete", w.ops.delete, "&Edit", "Del", selected),
        ActionSpec("edit.deselect", "Select &none", w.selection.clear, "&Edit", "Esc", selected),
        ActionSpec(
            "insert.channel",
            "&Channel",
            w.channel_rack.add_channel,
            "&Insert",
            "Ctrl+Shift+C",
            has_project,
        ),
        ActionSpec(
            "insert.pattern",
            "&Pattern",
            w.channel_rack.add_pattern,
            "&Insert",
            "Ctrl+Shift+P",
            has_project,
        ),
        ActionSpec(
            "insert.track",
            "Playlist &track",
            lambda: w.session.build_and_execute("AddPlaylistTrackNew", ""),
            "&Insert",
            "Ctrl+Shift+T",
            has_project,
        ),
        ActionSpec(
            "insert.insert",
            "Mixer &insert",
            w.mixer.add_insert,
            "&Insert",
            "Ctrl+Shift+I",
            has_project,
        ),
        ActionSpec(
            "insert.marker",
            "&Marker at playhead",
            w.add_marker_at_playhead,
            "&Insert",
            "Ctrl+M",
            has_project,
        ),
        ActionSpec(
            "insert.arrangement",
            "&Arrangement",
            w.playlist.toolbar.add_arrangement,
            "&Insert",
            "",
            has_project,
        ),
        ActionSpec("transport.play", "&Play / pause", w.toggle_play, "&Transport", "Space"),
        ActionSpec("transport.stop", "&Stop", w.stop, "&Transport", "Ctrl+Space"),
        ActionSpec(
            "transport.loop", "&Loop", w.toggle_loop, "&Transport", "Ctrl+L", checkable=True
        ),
        ActionSpec(
            "transport.metronome",
            "&Metronome",
            w.toggle_metronome,
            "&Transport",
            "Ctrl+K",
            checkable=True,
        ),
        ActionSpec(
            "transport.follow",
            "&Follow playhead",
            w.toggle_follow,
            "&Transport",
            "Ctrl+F",
            checkable=True,
        ),
    ]
    for panel in w.panels:
        specs.append(
            ActionSpec(
                f"view.{panel.panel_id}",
                panel.title,
                partial(_toggle_visible, panel),
                "&View",
                checkable=True,
            )
        )
    specs += [
        ActionSpec("view.reset_layout", "&Reset layout", w.reset_layout, "&View", separator=True),
        ActionSpec("view.dark", "Dark theme", lambda: w.set_theme_name("dark"), "&View"),
        ActionSpec("view.light", "Light theme", lambda: w.set_theme_name("light"), "&View"),
        ActionSpec("help.about", "&About adX", w.about, "&Help"),
    ]
    for tool in w.playlist.tools.values():
        specs.append(
            ActionSpec(
                f"playlist.tool.{tool.name}",
                f"{tool.label} tool",
                partial(w.playlist.set_tool, tool.name),
                "",
                tool.shortcut,
                scope="playlist",
            )
        )
    for spec in specs:
        registry.register(spec)
    for action in registry.scoped("playlist"):
        w.playlist.addAction(action)
