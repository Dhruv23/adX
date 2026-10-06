"""Scale highlighting and snapping (phase_5.md 4.7).

A scale is a root pitch class and a 12-bit mask: bit n set means the pitch class
``root + n`` is in it. The engine draws the highlight from the same mask
(engine/geometry/PianoRollGeometry.h, ScaleHighlight), so the two cannot disagree.
"""

from __future__ import annotations

import numpy as np
import numpy.typing as npt

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
}

NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")


def mask_of(scale: str) -> int:
    """The 12-bit mask of a named scale; "None" (or unknown) is 0, no highlighting."""
    intervals = SCALES.get(scale)
    if intervals is None:
        return 0
    mask = 0
    for interval in intervals:
        mask |= 1 << interval
    return mask


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
