"""The user's preferences (phase_6.md 4.9): read once at start, written by the dialog,
stored in QSettings under ``preferences/``.

Audio (sample rate and buffer size, applied the next time the engine starts), paths
(samples, presets, projects, plugins), appearance (theme, meter scale), editing (default
note length, snap, undo limit, autosave interval, text sync mode). MIDI is Phase 9's; its
page is there and says so.
"""

from __future__ import annotations

from dataclasses import dataclass, field, fields

from PySide6.QtCore import QSettings

from adx.engine_bridge import PPQ

GROUP = "preferences"


@dataclass(slots=True)
class Preferences:
    """Every preference, with its default."""

    sample_rate: int = 48000
    buffer_frames: int = 256
    #: Open the default input for recording (phase_8.md 3.1); applies at the next start.
    record_input: bool = False
    #: Play the default output in WASAPI exclusive mode (STATE.md P9-9): about 6 ms
    #: instead of about 60 on shared mode, but output only, and other apps go quiet.
    exclusive_audio: bool = False
    #: MIDI inputs to open, by port name (phase_9.md 4.1).
    midi_inputs: list[str] = field(default_factory=list)
    #: MIDI outputs to open, by port name (STATE.md P9-5).
    midi_outputs: list[str] = field(default_factory=list)
    #: Send MIDI clock, and MIDI time code (30 fps), to every open output.
    midi_send_clock: bool = False
    midi_send_mtc: bool = False
    #: The arrangement follows an input's clock or time code: none, clock or mtc.
    sync_follow: str = "none"
    sample_dirs: list[str] = field(default_factory=list)
    preset_dirs: list[str] = field(default_factory=list)
    project_dir: str = ""
    plugin_dirs: list[str] = field(default_factory=list)
    #: Plugins (by uid) whose editors are not embedded: the generic editor only (P9-8).
    plugins_never_embed: list[str] = field(default_factory=list)
    theme: str = "dark"
    meter_floor_db: float = -60.0
    default_note_ticks: int = PPQ // 4
    snap_ticks: int = PPQ // 4
    undo_limit: int = 1000
    autosave_minutes: int = 5
    #: How the project's file follows the project: on-save, live or off (phase_7.md).
    sync_mode: str = "on-save"

    @classmethod
    def load(cls, settings: QSettings) -> Preferences:
        """Stored values over the defaults; anything unreadable keeps its default."""
        prefs = cls()
        settings.beginGroup(GROUP)
        for f in fields(cls):
            if not settings.contains(f.name):
                continue
            stored = settings.value(f.name)
            current = getattr(prefs, f.name)
            try:
                if isinstance(current, list):
                    value: object = (
                        [str(v) for v in stored]
                        if isinstance(stored, list)
                        else ([str(stored)] if stored else [])
                    )
                elif isinstance(current, bool):
                    value = str(stored).lower() in ("1", "true", "yes")
                elif isinstance(current, int):
                    value = int(str(stored))
                elif isinstance(current, float):
                    value = float(str(stored))
                else:
                    value = str(stored)
            except (TypeError, ValueError):
                continue
            setattr(prefs, f.name, value)
        settings.endGroup()
        return prefs

    def save(self, settings: QSettings) -> None:
        """Write every preference."""
        settings.beginGroup(GROUP)
        for f in fields(self):
            settings.setValue(f.name, getattr(self, f.name))
        settings.endGroup()
