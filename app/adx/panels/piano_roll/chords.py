"""Chords, arpeggiation and riffs: pattern edits that make many notes at once
(phase_5.md 4.7). Each is one EditNotes command, so each undoes in a single step.

"Arpeggiate" here rewrites the selected notes into a run - a *pattern edit*. It is not
the per-channel arpeggiator of Phase 2's ``Channel.arp``, which plays held notes as an
arpeggio at render time without changing them; the UI names the two differently
("Arpeggiate selection" against the channel's "Arp") so nobody conflates them.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from adx.engine_bridge import PPQ, NoteArray
from adx.panels.piano_roll import scales
from adx.panels.piano_roll.model import RollModel
from adx.panels.piano_roll.tools import LEFT, Pointer, Tool

#: Chord shapes in semitones above the root, used with no scale (or when "chromatic").
CHORDS: dict[str, tuple[int, ...]] = {
    "Major": (0, 4, 7),
    "Minor": (0, 3, 7),
    "Diminished": (0, 3, 6),
    "Augmented": (0, 4, 8),
    "Sus2": (0, 2, 7),
    "Sus4": (0, 5, 7),
    "Major 7": (0, 4, 7, 11),
    "Minor 7": (0, 3, 7, 10),
    "Dominant 7": (0, 4, 7, 10),
    "Half-diminished 7": (0, 3, 6, 10),
    "Major 9": (0, 4, 7, 11, 14),
    "Minor 9": (0, 3, 7, 10, 14),
    "Add 9": (0, 4, 7, 14),
    "Minor 11": (0, 3, 7, 10, 14, 17),
}

#: With a scale, a chord is stacked thirds of the scale: this many of them.
DIATONIC: dict[str, int] = {"Triad": 3, "Seventh": 4, "Ninth": 5, "Eleventh": 6}


def chord_pitches(root: int, shape: str, scale_root: int, scale_mask: int) -> list[int]:
    """The pitches of a chord on ``root``: a diatonic stack in a scale, else a shape."""
    if shape in DIATONIC:
        base = scales.snap_pitch(
            root, scale_root, scale_mask if scale_mask else scales.mask_of("Major")
        )
        mask = scale_mask if scale_mask else scales.mask_of("Major")
        root_class = scale_root if scale_mask else base % 12
        return [scales.step_in_scale(base, 2 * i, root_class, mask) for i in range(DIATONIC[shape])]
    return [min(127, root + interval) for interval in CHORDS.get(shape, CHORDS["Major"])]


@dataclass(slots=True)
class ChordTool(Tool):
    """Click places a chord rooted at the cursor."""

    name: str = "chord"
    label: str = "Chord"
    shape: str = "Triad"
    _root: tuple[int, int] | None = None

    def press(self, model: RollModel, p: Pointer) -> None:
        self.reset()
        if p.button != LEFT:
            return
        start = model.snap_tick(model.view.tick_at(p.x))
        root = model.snap_pitch(model.view.row_at(p.y))
        self._root = (start, root)
        pitches = chord_pitches(root, self.shape, model.scale_root, model.scale_mask)
        self.preview = [(start, start + model.default_length, q) for q in pitches]

    def release(self, model: RollModel, p: Pointer) -> None:
        if self._root is None:
            return
        start, root = self._root
        self._root = None
        self.reset()
        pitches = chord_pitches(root, self.shape, model.scale_root, model.scale_mask)
        length = model.default_length
        model.edit(
            f"Chord ({self.shape})",
            add=model.new_notes([start] * len(pitches), [length] * len(pitches), pitches),
        )


#: Arpeggio orders.
ARP_ORDERS = ("Up", "Down", "Up-down", "Random")


def chords_in(rows: NoteArray, tolerance: int) -> list[NoteArray]:
    """Group notes into chords: notes starting within ``tolerance`` ticks of each other."""
    if not len(rows):
        return []
    ordered = rows[np.argsort(rows["start"], kind="stable")]
    groups: list[NoteArray] = []
    begin = 0
    for i in range(1, len(ordered) + 1):
        if i == len(ordered) or int(ordered["start"][i]) - int(ordered["start"][begin]) > tolerance:
            groups.append(ordered[begin:i])
            begin = i
    return groups


def arpeggiate_rows(rows: NoteArray, step: int, order: str, seed: int = 1) -> NoteArray:
    """The run that replaces ``rows``: each chord becomes ``step``-long notes cycling
    through its pitches for as long as the chord lasted."""
    rng = np.random.default_rng(seed)
    runs: list[NoteArray] = []
    for chord in chords_in(rows, tolerance=max(1, step // 2)):
        pitches = sorted(int(p) for p in chord["pitch"])
        if order == "Down":
            cycle = pitches[::-1]
        elif order == "Up-down":
            cycle = pitches + pitches[-2:0:-1] if len(pitches) > 2 else pitches
        else:
            cycle = pitches
        start = int(chord["start"].min())
        end = int((chord["start"].astype(np.int64) + chord["length"].astype(np.int64)).max())
        count = max(1, (end - start) // step)
        run = np.repeat(chord[:1], count)
        run["start"] = start + np.arange(count, dtype=np.int64) * step
        run["length"] = step
        if order == "Random":
            run["pitch"] = rng.choice(pitches, size=count)
        else:
            run["pitch"] = [cycle[i % len(cycle)] for i in range(count)]
        runs.append(run)
    return np.concatenate(runs) if runs else rows[:0]


def arpeggiate(model: RollModel, order: str = "Up") -> bool:
    """Rewrite the selected chords as a run: one command, or none with nothing chordal."""
    rows = model.selected_notes()
    if len(rows) < 2:
        return False
    run = arpeggiate_rows(rows, model.step(), order)
    model.select(rows["id"], "remove")
    model.edit(f"Arpeggiate selection ({order.lower()})", remove=rows["id"].copy(), add=run)
    return True


#: Rhythm templates: per sixteenth of one bar, the chance a note starts there.
RHYTHMS: dict[str, tuple[float, ...]] = {
    "Straight 8ths": (1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0),
    "Offbeat": (0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0),
    "Syncopated": (1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0),
    "Sixteenths": (1.0,) * 16,
    "Sparse": (1, 0, 0, 0, 0, 0, 0.6, 0, 0, 0, 0.6, 0, 0, 0, 0, 0),
}


@dataclass(frozen=True, slots=True)
class RiffSettings:
    """A riff generator's input; the seed makes it reproducible."""

    seed: int = 1
    bars: int = 1
    rhythm: str = "Syncopated"
    density: float = 0.85
    low: int = 48
    high: int = 72
    start: int = 0


def riff_rows(model: RollModel, settings: RiffSettings) -> NoteArray:
    """A seeded riff over the model's scale (natural minor when it has none)."""
    rng = np.random.default_rng(settings.seed)
    template = RHYTHMS.get(settings.rhythm, RHYTHMS["Syncopated"])
    mask = model.scale_mask or scales.mask_of("Natural minor")
    root = model.scale_root if model.scale_mask else 9
    pool = [p for p in range(settings.low, settings.high + 1) if scales.in_scale(p, root, mask)]
    sixteenth = PPQ // 4
    starts: list[int] = []
    for bar in range(settings.bars):
        for index, chance in enumerate(template):
            if chance > 0 and rng.random() < chance * settings.density:
                starts.append(settings.start + (bar * 16 + index) * sixteenth)
    pitches = rng.choice(pool, size=len(starts)) if pool and starts else np.zeros(0, dtype=np.int64)
    lengths = [
        (starts[i + 1] - starts[i]) if i + 1 < len(starts) else sixteenth * 2
        for i in range(len(starts))
    ]
    return model.new_notes(
        starts, [min(length, sixteenth * 4) for length in lengths], [int(p) for p in pitches]
    )


def generate_riff(model: RollModel, settings: RiffSettings) -> bool:
    """Add a seeded riff: one AddNotes-shaped command."""
    rows = riff_rows(model, settings)
    if not len(rows):
        return False
    model.edit(f"Generate riff (seed {settings.seed})", add=rows)
    return True
