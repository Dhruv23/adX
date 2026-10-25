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
    sample_dirs: list[str] = field(default_factory=list)
    preset_dirs: list[str] = field(default_factory=list)
    project_dir: str = ""
    plugin_dirs: list[str] = field(default_factory=list)
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
