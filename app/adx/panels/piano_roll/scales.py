"""Scale highlighting and snapping (phase_5.md 4.7).

A scale is a root pitch class and a 12-bit mask: bit n set means the pitch class
``root + n`` is in it. The engine draws the highlight from the same mask
(engine/geometry/PianoRollGeometry.h, ScaleHighlight), so the two cannot disagree.
"""

from __future__ import annotations

import numpy as np
import numpy.typing as npt
from PySide6.QtCore import QSettings

#: Intervals above the root, by scale name.
SCALES: dict[str, tuple[int, ...]] = {
    "Chromatic": tuple(range(12)),
    "Major": (0, 2, 4, 5, 7, 9, 11),
    "Natural minor": (0, 2, 3, 5, 7, 8, 10),
    "Harmonic minor": (0, 2, 3, 5, 7, 8, 11),
    "Melodic minor": (0, 2, 3, 5, 7, 9, 11),
    "Dorian": (0, 2, 3, 5, 7, 9, 10),
    "Phrygian": (0, 1, 3, 5, 7, 8, 10),
    "Lydian": (0, 2, 4, 6, 7, 9, 11),
    "Mixolydian": (0, 2, 4, 5, 7, 9, 10),
    "Locrian": (0, 1, 3, 5, 6, 8, 10),
    "Major pentatonic": (0, 2, 4, 7, 9),
    "Minor pentatonic": (0, 3, 5, 7, 10),
    "Blues": (0, 3, 5, 6, 7, 10),
    "Whole tone": (0, 2, 4, 6, 8, 10),
    # Toward FL Studio's list (phase_11.6.md 4.1).
    "Harmonic major": (0, 2, 4, 5, 7, 8, 11),
    "Diminished (whole-half)": (0, 2, 3, 5, 6, 8, 9, 11),
    "Diminished (half-whole)": (0, 1, 3, 4, 6, 7, 9, 10),
    "Hungarian minor": (0, 2, 3, 6, 7, 8, 11),
    "Hungarian major": (0, 3, 4, 6, 7, 9, 10),
    "Neapolitan major": (0, 1, 3, 5, 7, 9, 11),
    "Neapolitan minor": (0, 1, 3, 5, 7, 8, 11),
    "Enigmatic": (0, 1, 4, 6, 8, 10, 11),
    "Persian": (0, 1, 4, 5, 6, 8, 11),
    "8-tone Spanish": (0, 1, 3, 4, 5, 6, 8, 10),
    "Gypsy (Romani)": (0, 2, 3, 6, 7, 8, 10),
    "Arabic (double harmonic)": (0, 1, 4, 5, 7, 8, 11),
    "Ahava Rabba (Phrygian dominant)": (0, 1, 4, 5, 7, 8, 10),
    "Hirajoshi": (0, 2, 3, 7, 8),
    "Iwato": (0, 1, 5, 6, 10),
    "Kumoi": (0, 2, 3, 7, 9),
    "Pelog": (0, 1, 3, 7, 8),
}

#: The scale combo's first entry (no highlighting) and its last (opens the editor).
NO_SCALE = "No scale"
CUSTOM_ENTRY = "Custom…"

NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")


def mask_of(scale: str) -> int:
    """The 12-bit mask of a named scale; "None" (or unknown) is 0, no highlighting."""
    intervals = SCALES.get(scale)
    if intervals is None:
        return 0
    return mask_of_intervals(intervals)


def mask_of_intervals(intervals: tuple[int, ...] | list[int]) -> int:
    """The 12-bit mask of some intervals above the root."""
    mask = 0
    for interval in intervals:
        mask |= 1 << (interval % 12)
    return mask


class CustomScales:
    """The user's own scales: a name and a root-relative 12-bit mask each, kept in
    QSettings like the keymap (group ``custom_scales``, one key per name). The root is
    never stored, so a custom scale works from whatever root the combo picks."""

    GROUP = "custom_scales"

    def __init__(self, settings: QSettings | None = None) -> None:
        self.settings = settings
        self._masks: dict[str, int] = {}
        if settings is not None:
            settings.beginGroup(self.GROUP)
            for key in settings.childKeys():
                try:
                    mask = int(str(settings.value(key, "0")), 0)
                except ValueError:
                    continue
                if 0 < mask < 1 << 12:
                    self._masks[key] = mask
            settings.endGroup()

    @staticmethod
    def valid_name(name: str) -> bool:
        """A name can be saved: not empty, not a preset's, no QSettings separators."""
        name = name.strip()
        reserved = {*SCALES, NO_SCALE, CUSTOM_ENTRY}
        return bool(name) and name not in reserved and "/" not in name and "\\" not in name

    def names(self) -> list[str]:
        return sorted(self._masks, key=str.casefold)

    def mask(self, name: str) -> int:
        return self._masks.get(name, 0)

    def save(self, name: str, mask: int) -> None:
        """Add or overwrite (one entry per name). The root's bit is always set."""
        name = name.strip()
        if not self.valid_name(name):
            raise ValueError(f"not a usable scale name: {name!r}")
        mask = (mask | 1) & 0xFFF
        self._masks[name] = mask
        if self.settings is not None:
            self.settings.setValue(f"{self.GROUP}/{name}", f"0x{mask:03x}")
            self.settings.sync()

    def delete(self, name: str) -> None:
        self._masks.pop(name, None)
        if self.settings is not None:
            self.settings.remove(f"{self.GROUP}/{name}")
            self.settings.sync()


def in_scale(pitch: int, root: int, mask: int) -> bool:
    """Whether a pitch is in the scale. Everything is, when there is no scale."""
    return mask == 0 or bool(mask & (1 << ((pitch - root) % 12)))


def snap_pitch(pitch: int, root: int, mask: int) -> int:
    """The nearest in-scale pitch; a tie resolves downward. Identity with no scale."""
    if in_scale(pitch, root, mask):
        return max(0, min(127, pitch))
    for distance in range(1, 12):
        for candidate in (pitch - distance, pitch + distance):
            if 0 <= candidate <= 127 and in_scale(candidate, root, mask):
                return candidate
    return max(0, min(127, pitch))


def snap_pitches(pitches: npt.NDArray[np.integer], root: int, mask: int) -> npt.NDArray[np.int64]:
    """:func:`snap_pitch` over an array, through a 128-entry table."""
    table = np.array([snap_pitch(p, root, mask) for p in range(128)], dtype=np.int64)
    return table[np.clip(pitches.astype(np.int64), 0, 127)]


def degrees(root: int, mask: int) -> list[int]:
    """The scale's pitch classes above the root, ascending (all twelve with no scale)."""
    if mask == 0:
        return list(range(12))
    return [n for n in range(12) if mask & (1 << n)]


def step_in_scale(pitch: int, steps: int, root: int, mask: int) -> int:
    """The pitch ``steps`` scale degrees above (below, if negative) an in-scale pitch."""
    current = snap_pitch(pitch, root, mask)
    direction = 1 if steps >= 0 else -1
    remaining = abs(steps)
    while remaining > 0:
        current += direction
        if current < 0 or current > 127:
            return max(0, min(127, current - direction))
        if in_scale(current, root, mask):
            remaining -= 1
    return current
