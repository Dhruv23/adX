"""The sound library's fixed vocabulary (phase_12.md 4.11).

Categories are the same in every pack, so browsing works across them. Each top-level
category fixes what its eight macro slots mean - every Lead's Macro 1 is Brightness - and
the loudness its audition phrase should land at, so clicking from preset to preset never
jumps by 15 dB (``pack_loudness_window``).
"""

from __future__ import annotations

from dataclasses import dataclass

#: Every category a preset or rack may name, as ``CATEGORY=``.
CATEGORIES: tuple[str, ...] = (
    "Drums/Kick",
    "Drums/Snare",
    "Drums/Clap",
    "Drums/Hat",
    "Drums/Cymbal",
    "Drums/Perc",
    "Drums/Kit",
    "Drums/Break",
    "Bass/Sub",
    "Bass/808",
    "Bass/Reese",
    "Bass/Synth",
    "Bass/Acoustic",
    "Lead",
    "Pluck",
    "Keys/Piano",
    "Keys/EP",
    "Keys/Organ",
    "Keys/Mallet",
    "Pad",
    "Strings",
    "Brass",
    "Woodwind",
    "Guitar",
    "Choir/Vocal",
    "FX/Riser",
    "FX/Impact",
    "FX/Texture",
    "Chain/Kick",
    "Chain/Bus",
    "Chain/Vocal",
    "Chain/Master",
    "Chain/Space",
)

#: What Macro 1..8 mean, by top-level category. A rack exposes at most these eight, in
#: this order; a slot it has no use for is left out, not renamed.
MACRO_SLOTS: dict[str, tuple[str, ...]] = {
    "Drums": ("Pitch", "Decay", "Punch", "Drive", "Tone", "Body", "Space", "Width"),
    "Bass": ("Brightness", "Drive", "Glide", "Decay", "Sub", "Movement", "Space", "Width"),
    "Lead": ("Brightness", "Detune", "Vibrato", "Drive", "Attack", "Release", "Space", "Width"),
    "Pluck": ("Brightness", "Decay", "Body", "Drive", "Attack", "Release", "Space", "Width"),
    "Keys": ("Brightness", "Tone", "Vibrato", "Drive", "Attack", "Release", "Space", "Width"),
    "Pad": ("Brightness", "Movement", "Detune", "Texture", "Attack", "Release", "Space", "Width"),
    "Strings": ("Brightness", "Dynamics", "Vibrato", "Bow", "Attack", "Release", "Space", "Width"),
    "Brass": ("Brightness", "Dynamics", "Vibrato", "Growl", "Attack", "Release", "Space", "Width"),
    "Woodwind": (
        "Brightness",
        "Dynamics",
        "Vibrato",
        "Breath",
        "Attack",
        "Release",
        "Space",
        "Width",
    ),
    "Guitar": ("Brightness", "Pick", "Mute", "Drive", "Body", "Release", "Space", "Width"),
    "Choir": ("Brightness", "Vowel", "Vibrato", "Breath", "Attack", "Release", "Space", "Width"),
    "FX": ("Length", "Pitch", "Movement", "Drive", "Filter", "Texture", "Space", "Width"),
    "Chain": ("Amount", "Drive", "Tone", "Dynamics", "Character", "Mix", "Space", "Width"),
}

#: Integrated LUFS each category's audition phrase is set to land at.
LOUDNESS_TARGETS: dict[str, float] = {
    "Drums/Kick": -8.0,
    "Drums/Snare": -10.0,
    "Drums/Clap": -10.0,
    "Drums/Hat": -16.0,
    "Drums/Cymbal": -16.0,
    "Drums/Perc": -14.0,
    "Drums/Kit": -10.0,
    "Drums/Break": -10.0,
    "Bass": -10.0,
    "Lead": -12.0,
    "Pluck": -13.0,
    "Keys": -14.0,
    "Pad": -16.0,
    "Strings": -16.0,
    "Brass": -14.0,
    "Woodwind": -15.0,
    "Guitar": -13.0,
    "Choir": -15.0,
    "FX": -14.0,
    "Chain": -12.0,
}

#: How far from its target an audition may land (LU).
LOUDNESS_WINDOW = 2.0


@dataclass(frozen=True)
class Category:
    """A parsed ``CATEGORY=`` value."""

    path: str

    @property
    def top(self) -> str:
        """``Drums`` for ``Drums/Kick``; ``Choir`` for ``Choir/Vocal``."""
        return self.path.split("/", 1)[0]

    @property
    def known(self) -> bool:
        return self.path in CATEGORIES

    @property
    def macro_slots(self) -> tuple[str, ...]:
        return MACRO_SLOTS.get(self.top, ())

    @property
    def loudness_target(self) -> float:
        """The full path's target, else its top category's; -14 for anything unknown."""
        return LOUDNESS_TARGETS.get(self.path, LOUDNESS_TARGETS.get(self.top, -14.0))


def category(path: str) -> Category:
    return Category(path.strip())


def macro_names_ok(category_path: str, names: list[str]) -> list[str]:
    """The macro names that are not their category's slot names, or are out of order."""
    slots = category(category_path).macro_slots
    problems: list[str] = []
    last = -1
    for name in names:
        if name not in slots:
            problems.append(f"{name!r} is not a {category(category_path).top} macro")
            continue
        at = slots.index(name)
        if at < last:
            problems.append(f"{name!r} is out of slot order")
        last = at
    if len(names) > 8:
        problems.append(f"{len(names)} macros; a rack has at most 8")
    return problems
