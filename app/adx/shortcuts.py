"""The keymap: every action's shortcut, user-remappable and persisted (phase_6.md 4.10).

Defaults come from the action registry; a remap is stored in QSettings under
``shortcuts/<action id>`` and wins over the default. :meth:`Keymap.conflicts` finds two
actions sharing a key - the conflict users would otherwise discover for us.
"""

from __future__ import annotations

from PySide6.QtCore import QSettings
from PySide6.QtGui import QKeySequence


def normalise(text: str) -> str:
    """A shortcut in Qt's portable text form, so "ctrl+z" and "Ctrl+Z" are one key."""
    return QKeySequence(text).toString(QKeySequence.SequenceFormat.PortableText) if text else ""


class Keymap:
    """Default shortcuts, the user's overrides, and where they are saved."""

    GROUP = "shortcuts"

    def __init__(self, settings: QSettings | None = None) -> None:
        self.settings = settings
        self._defaults: dict[str, str] = {}
        self._scopes: dict[str, str] = {}
        self._overrides: dict[str, str] = {}
        if settings is not None:
            settings.beginGroup(self.GROUP)
            for key in settings.childKeys():
                self._overrides[key] = normalise(str(settings.value(key, "")))
            settings.endGroup()

    def declare(self, action_id: str, default: str, scope: str = "") -> None:
        """Record an action's default shortcut and where it works ("" everywhere)."""
        self._defaults[action_id] = normalise(default)
        self._scopes[action_id] = scope

    def shortcut(self, action_id: str) -> str:
        """The shortcut in force: the user's, else the default."""
        if action_id in self._overrides:
            return self._overrides[action_id]
        return self._defaults.get(action_id, "")

    def default(self, action_id: str) -> str:
        """The shortcut an action ships with."""
        return self._defaults.get(action_id, "")

    def set(self, action_id: str, shortcut: str) -> None:
        """Remap (an empty string clears the key); persisted at once."""
        value = normalise(shortcut)
        if value == self._defaults.get(action_id, ""):
            self._overrides.pop(action_id, None)
            if self.settings is not None:
                self.settings.remove(f"{self.GROUP}/{action_id}")
            return
        self._overrides[action_id] = value
        if self.settings is not None:
            self.settings.setValue(f"{self.GROUP}/{action_id}", value)

    def reset(self) -> None:
        """Every action back to its default."""
        self._overrides.clear()
        if self.settings is not None:
            self.settings.remove(self.GROUP)

    def conflicts(self) -> dict[str, list[str]]:
        """Shortcuts bound to more than one action where both work: key -> action ids. A
        global key collides with every scope; two panel keys collide only in one panel."""
        by_key: dict[str, list[str]] = {}
        for action_id in self._defaults:
            key = self.shortcut(action_id)
            if key:
                by_key.setdefault(key, []).append(action_id)
        out: dict[str, list[str]] = {}
        for key, ids in by_key.items():
            clashing = [
                a
                for a in ids
                if any(
                    b != a
                    and (
                        not self._scopes[a]
                        or not self._scopes[b]
                        or self._scopes[a] == self._scopes[b]
                    )
                    for b in ids
                )
            ]
            if clashing:
                out[key] = clashing
        return out
