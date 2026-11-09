"""Every action the main window offers, declared once (phase_6.md 4.10).

The menu bar is built from this list; the keymap holds each one's shortcut; the
enablement predicates are evaluated when a menu opens and whenever the selection or the
project changes. Panel-scoped actions (the playlist's and the piano roll's tools) carry
their panel's id and work only while it has focus.
"""

from __future__ import annotations

from collections.abc import Callable
from functools import partial
from typing import TYPE_CHECKING

from adx.actions import ActionRegistry, ActionSpec

if TYPE_CHECKING:
    from adx.mainwindow import MainWindow


#: FL Studio's keys for the panels a musician switches between most.
PANEL_KEYS = {
    "playlist": "F5",
    "channel_rack": "F6",
    "piano_roll": "F7",
    "browser": "F8",
    "mixer": "F9",
}

#: The piano roll's tool keys, stated once (phase_11.6.md 4.3): the first nine keep the
#: 1-9 they always had, Cut takes 0, and the chord tool has none.
PIANO_ROLL_TOOL_KEYS = {
    "draw": "1",
    "paint": "2",
    "select": "3",
    "slice": "4",
    "glue": "5",
    "strum": "6",
    "mute": "7",
    "slide": "8",
    "bend": "9",
    "cut": "0",
}


def register_actions(window: MainWindow, registry: ActionRegistry) -> None:
    """Declare every action of the main window."""
    w = window
    has_project = lambda: w.session.project is not None  # noqa: E731
    specs = [
        ActionSpec("file.new", "&New", w.new_project, "&File", "Ctrl+N"),
        ActionSpec("file.open", "&Open…", w.open_dialog, "&File", "Ctrl+O"),
        ActionSpec("file.save", "&Save", w.save, "&File", "Ctrl+S", has_project),
        ActionSpec("file.save_as", "Save &As…", w.save_as, "&File", "Ctrl+Shift+S", has_project),
        ActionSpec(
            "file.export",
            "&Export…",
            w.export_dialog,
            "&File",
            "Ctrl+E",
            has_project,
            separator=True,
            tooltip="Export audio: any format, stems, range, normalize (Ctrl+E)",
        ),
        ActionSpec(
            "file.export_midi",
            "Export &MIDI…",
            w.delivery.export_midi_dialog,
            "&File",
            "",
            has_project,
        ),
        ActionSpec(
            "file.import_midi",
            "&Import MIDI…",
            w.delivery.import_midi_dialog,
            "&File",
            "",
            has_project,
        ),
        ActionSpec(
            "file.collect",
            "&Collect and save…",
            w.delivery.collect_and_save,
            "&File",
            "",
            has_project,
            tooltip="Copy the project and every sample it uses into one folder, and zip it",
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
        *_edit_specs(w, has_project),
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
        *_transport_specs(w),
    ]
    for panel in w.panels:
        specs.append(
            ActionSpec(
                f"view.{panel.panel_id}",
                panel.title,
                partial(w.show_panel, panel),
                "&View",
                PANEL_KEYS.get(panel.panel_id, ""),
                tooltip=f"Show {panel.title} and bring it to the front",
            )
        )
    specs += [
        ActionSpec(
            "view.maximize",
            "&Maximize panel",
            w.toggle_maximize,
            "&View",
            "F11",
            separator=True,
            tooltip="Fill the window with the panel you are working in; again to restore",
        ),
        ActionSpec("view.reset_layout", "&Reset layout", w.reset_layout, "&View"),
        ActionSpec("view.dark", "Dark theme", lambda: w.set_theme_name("dark"), "&View"),
        ActionSpec("view.light", "Light theme", lambda: w.set_theme_name("light"), "&View"),
        ActionSpec("help.quickstart", "&Quick start", w.show_quickstart, "&Help", "F1"),
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
    specs += _piano_roll_specs(w)
    for spec in specs:
        registry.register(spec)
    for action in registry.scoped("playlist"):
        w.playlist.addAction(action)
    _attach_piano_roll(w, registry)


def _piano_roll_specs(w: MainWindow) -> list[ActionSpec]:
    """The piano roll's tool keys (phase_11.6.md 4.3) and its scale-lock action."""
    specs: list[ActionSpec] = []
    for name, roll_tool in w.piano_roll.tools.items():
        specs.append(
            ActionSpec(
                f"piano_roll.tool.{name}",
                f"{roll_tool.label} tool",
                partial(w.piano_roll.set_tool, name),
                "",
                PIANO_ROLL_TOOL_KEYS.get(name, ""),
                checkable=True,
                scope="piano_roll",
            )
        )
    specs.append(
        ActionSpec(
            "piano_roll.snap_notes_to_scale",
            "Snap notes to scale",
            w.piano_roll.snap_notes_to_scale,
            "",
            "",
            w.piano_roll.has_scale,
            scope="piano_roll",
            tooltip="Move every note in the pattern onto the chosen scale (one undo step)",
        )
    )
    return specs


def _attach_piano_roll(w: MainWindow, registry: ActionRegistry) -> None:
    """The roll's toolbar buttons become the registry's actions."""
    w.piano_roll.adopt_tool_actions(
        {name: registry.action(f"piano_roll.tool.{name}") for name in w.piano_roll.tools}
    )
    w.piano_roll.snap_action = registry.action("piano_roll.snap_notes_to_scale")


def _edit_specs(w: MainWindow, has_project: Callable[[], bool]) -> list[ActionSpec]:
    """The Edit menu: undo and redo, and the four operations over the selection."""
    selected = lambda: bool(w.selection.current)  # noqa: E731
    can_undo = lambda: w.session.project is not None and w.session.project.undo_depth() > 0  # noqa: E731
    return [
        ActionSpec(
            "edit.undo", "&Undo", w.undo, "&Edit", "Ctrl+Z", can_undo, tooltip="Undo (Ctrl+Z)"
        ),
        ActionSpec(
            "edit.redo",
            "&Redo",
            w.redo,
            "&Edit",
            "Ctrl+Shift+Z",
            has_project,
            tooltip="Redo (Ctrl+Shift+Z or Ctrl+Y)",
            alternates=("Ctrl+Y",),
        ),
        ActionSpec("edit.copy", "&Copy", w.ops.copy, "&Edit", "Ctrl+C", selected, separator=True),
        ActionSpec(
            "edit.paste",
            "&Paste",
            w.paste,
            "&Edit",
            "Ctrl+V",
            has_project,
            tooltip="Paste at the playhead (Ctrl+V)",
        ),
        ActionSpec("edit.duplicate", "D&uplicate", w.ops.duplicate, "&Edit", "Ctrl+D", selected),
        ActionSpec("edit.delete", "&Delete", w.ops.delete, "&Edit", "Del", selected),
        ActionSpec("edit.deselect", "Select &none", w.selection.clear, "&Edit", "Esc", selected),
        ActionSpec(
            "edit.freeze",
            "&Freeze channel",
            w.delivery.freeze,
            "&Edit",
            "",
            w.delivery.can_freeze,
            separator=True,
            tooltip="Render the selected channel to audio and play that instead (undoable)",
        ),
        ActionSpec(
            "edit.unfreeze",
            "Unf&reeze channel",
            w.delivery.unfreeze,
            "&Edit",
            "",
            w.delivery.can_freeze,
        ),
        ActionSpec(
            "edit.restart_plugins",
            "Restart crashed &plugins",
            w.plugin_runtime.restart_crashed,
            "&Edit",
            "",
            separator=True,
            tooltip="Bring crashed plugins back with their last state (phase_9.md 4.4)",
        ),
    ]


def _transport_specs(w: MainWindow) -> list[ActionSpec]:
    """The Transport menu."""
    return [
        ActionSpec("transport.play", "&Play / pause", w.toggle_play, "&Transport", "Space"),
        ActionSpec(
            "transport.stop", "&Stop", w.stop, "&Transport", "Ctrl+Space", tooltip="Stop in place"
        ),
        ActionSpec(
            "transport.rewind",
            "Return to s&tart",
            w.rewind,
            "&Transport",
            "Home",
            tooltip="Move the playhead to the start",
        ),
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
            "transport.record",
            "&Record",
            w.delivery.record.toggle,
            "&Transport",
            "Ctrl+R",
            tooltip="Record on the selected clip's track, or the last track (Ctrl+R)",
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
