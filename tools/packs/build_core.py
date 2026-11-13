"""Writes the `core` pack (phase_13.md): drum voices, kits, 808 basses, transitions,
chains and pattern templates, from the recipe tables below.

    python tools/packs/build_core.py [--calibrate]

The tables are the pack's source; the files under engine/preset/packs/core/ are their
output, committed so a reader (and the browser) needs no build step. Sampled voices are
imported from VCSL's SFZ (tools/packs/import_sfz.py) and given their synthesized
fallback. --calibrate auditions every sound and moves its level until the audition sits
on its category's loudness target (taxonomy.py), up to five passes.

Values are the plan's starting points; the human gate (phase_13.md 6) retunes them, and
what it approves is what ships.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PACK = REPO / "engine" / "preset" / "packs" / "core"
PY = sys.executable

# --- the drum voices (4.1) ---------------------------------------------------------------

#: name -> (category, tags, params). DrumSynth models: 0 kick, 1 snare, 2 hat, 3 clap,
#: 4 tom, 5 kit, 6 cowbell, 7 cymbal, 8 rim, 9 clave, 10 shaker, 11 conga.
DRUMS: dict[str, tuple[str, str, dict[str, float]]] = {
    "808 Kick Short": (
        "Drums/Kick",
        "808, kick",
        {
            "model": 0,
            "character": 0,
            "tune": -5,
            "decay": 0.35,
            "pitchDrop": 5,
            "pitchTime": 0.06,
            "click": 0.2,
        },
    ),
    # It doubles as the bass, and is almost all sub, which loudness weighting barely
    # counts: filed under Bass/808, with a little drive for the harmonics a phone hears.
    "808 Kick Long": (
        "Bass/808",
        "808, kick, bass",
        {
            "model": 0,
            "character": 0,
            "tune": -5,
            "decay": 1.6,
            "pitchDrop": 5,
            "pitchTime": 0.06,
            "click": 0.3,
            "keyTrack": 1,
            "drive": 3,
        },
    ),
    "909 Kick": (
        "Drums/Kick",
        "909, kick",
        {
            "model": 0,
            "character": 1,
            "pitchDrop": 24,
            "pitchTime": 0.025,
            "click": 0.7,
            "decay": 0.45,
            "drive": 2,
        },
    ),
    "House Kick": (
        "Drums/Kick",
        "house, kick",
        {"model": 0, "character": 1, "decay": 0.3, "tone": 0.6, "sub": 0.3},
    ),
    "Trap Kick": (
        "Drums/Kick",
        "trap, kick",
        {"model": 0, "character": 0, "decay": 0.5, "click": 0.6, "drive": 3},
    ),
    "808 Snare": (
        "Drums/Snare",
        "808, snare",
        {"model": 1, "character": 0, "tune": 0, "tone": 0.45, "noise": 0.6, "decay": 0.18},
    ),
    "909 Snare": (
        "Drums/Snare",
        "909, snare",
        {"model": 1, "character": 1, "tone": 0.6, "noise": 0.75, "decay": 0.22},
    ),
    "Rimshot 808": ("Drums/Snare", "808, rim", {"model": 8}),
    "808 Clap": ("Drums/Clap", "808, clap", {"model": 3, "character": 0, "decay": 0.3}),
    "Snap": ("Drums/Clap", "snap", {"model": 3, "decay": 0.05, "tone": 0.9}),
    "808 Closed Hat": ("Drums/Hat", "808, hat, closed", {"model": 2, "decay": 0.05, "tone": 0.7}),
    "808 Open Hat": ("Drums/Hat", "808, hat, open", {"model": 2, "decay": 0.45, "tone": 0.7}),
    "808 Hat Pair": (
        "Drums/Hat",
        "808, hat, choke",
        {"model": 2, "decay": 0.05, "tone": 0.7, "hat.openDecay": 0.45, "hat.choke": 1},
    ),
    "909 Hat": (
        "Drums/Hat",
        "909, hat, choke",
        {
            "model": 2,
            "character": 1,
            "decay": 0.06,
            "tone": 0.9,
            "hat.openDecay": 0.35,
            "hat.choke": 1,
        },
    ),
    "Trap Hat": (
        "Drums/Hat",
        "trap, hat, rolls, choke",
        {"model": 2, "decay": 0.035, "tone": 0.85, "hat.openDecay": 0.3, "hat.choke": 1},
    ),
    "808 Cymbal": ("Drums/Cymbal", "808, cymbal", {"model": 7, "decay": 2.5}),
    "808 Cowbell": (
        "Drums/Perc",
        "808, cowbell, phonk, melodic",
        {"model": 6, "character": 0, "decay": 0.25, "keyTrack": 1},
    ),
    "808 Clave": ("Drums/Perc", "808, clave", {"model": 9}),
    "Shaker": ("Drums/Perc", "shaker", {"model": 10, "tone": 0.75}),
    "808 Tom Low": ("Drums/Perc", "808, tom", {"model": 4, "tune": -7, "decay": 0.5}),
    "808 Tom Mid": ("Drums/Perc", "808, tom", {"model": 4, "tune": 0, "decay": 0.5}),
    "808 Tom High": ("Drums/Perc", "808, tom", {"model": 4, "tune": 5, "decay": 0.5}),
    "808 Conga Low": ("Drums/Perc", "808, conga", {"model": 11, "tune": -5}),
    "808 Conga Mid": ("Drums/Perc", "808, conga", {"model": 11, "tune": 0}),
    "808 Conga High": ("Drums/Perc", "808, conga", {"model": 11, "tune": 5}),
}

#: Sampled voices: name -> (category, tags, VCSL SFZ under the library, fallback, extra).
SAMPLED: dict[str, tuple[str, str, str, str, dict[str, float]]] = {
    "Acoustic Kick": (
        "Drums/Kick",
        "acoustic, kick",
        "Membranophones/Struck Membranophones/Bass Drum 2.sfz",
        "909 Kick",
        {},
    ),
    "Acoustic Snare": (
        "Drums/Snare",
        "acoustic, snare",
        "Membranophones/Struck Membranophones/Snare Drum, Modern 1.sfz",
        "909 Snare",
        {},
    ),
    "Rope Snare": (
        "Drums/Snare",
        "acoustic, snare, rope",
        "Membranophones/Struck Membranophones/Snare Drum, Rope Tension.sfz",
        "808 Snare",
        {},
    ),
    "Piccolo Snare": (
        "Drums/Snare",
        "acoustic, snare, piccolo",
        "Membranophones/Struck Membranophones/Snare Drum, Modern 3.sfz",
        "909 Snare",
        {"tune": 300},
    ),
    "Acoustic Clap": (
        "Drums/Clap",
        "acoustic, clap",
        "Idiophones/Struck Idiophones/Claps.sfz",
        "808 Clap",
        {},
    ),
    "Acoustic Hat": (
        "Drums/Hat",
        "acoustic, hat",
        "Idiophones/Struck Idiophones/Hi-Hat Cymbal.sfz",
        "808 Closed Hat",
        {},
    ),
    "Crash": (
        "Drums/Cymbal",
        "acoustic, crash",
        "Idiophones/Struck Idiophones/Clash Cymbals 1.sfz",
        "808 Cymbal",
        {},
    ),
    "Suspended Cymbal": (
        "Drums/Cymbal",
        "acoustic, cymbal, roll",
        "Idiophones/Struck Idiophones/Suspended Cymbal 1.sfz",
        "808 Cymbal",
        {},
    ),
    "Tambourine": (
        "Drums/Perc",
        "acoustic, tambourine",
        "Idiophones/Struck Idiophones/Tambourine 1.sfz",
        "Shaker",
        {},
    ),
    "Woodblock": (
        "Drums/Perc",
        "acoustic, woodblock",
        "Idiophones/Struck Idiophones/Woodblock.sfz",
        "808 Clave",
        {},
    ),
    "Agogo": (
        "Drums/Perc",
        "acoustic, agogo, bell",
        "Idiophones/Struck Idiophones/Agogo Bells.sfz",
        "808 Cowbell",
        {},
    ),
    "Triangle": (
        "Drums/Perc",
        "acoustic, triangle",
        "Idiophones/Struck Idiophones/Triangles.sfz",
        "808 Cymbal",
        {},
    ),
}

# --- the 808 basses and the synthesized transitions ---------------------------------------

#: VA presets: name -> (category, tags, params). Waves: 0 sine, 1 saw, 2 square.
SYNTHS: dict[str, tuple[str, str, dict[str, float]]] = {
    "808 Clean": (
        "Bass/808",
        "808, bass, glide",
        {
            "osc1.wave": 0,
            "osc2.level": 0,
            "env.attack": 0.002,
            "env.decay": 1.8,
            "env.sustain": 0,
            "env.release": 0.2,
            "pitchEnv.amount": 7,
            "pitchEnv.time": 0.05,
            "voice.mode": 2,
            "glide": 0.06,
            "filter.cutoff": 20000,
        },
    ),
    "808 Long Glide": (
        "Bass/808",
        "808, bass, glide, long",
        {
            "osc1.wave": 0,
            "osc2.level": 0,
            "env.attack": 0.002,
            "env.decay": 1.8,
            "env.sustain": 0.7,
            "env.release": 0.2,
            "pitchEnv.amount": 7,
            "pitchEnv.time": 0.05,
            "voice.mode": 2,
            "glide": 0.12,
            "glide.mode": 0,
            "filter.cutoff": 20000,
        },
    ),
    "Sub Sine": (
        "Bass/Sub",
        "sub, sine, clean",
        {
            "osc1.wave": 0,
            "osc2.level": 0,
            "env.attack": 0.005,
            "env.decay": 0.3,
            "env.sustain": 1,
            "env.release": 0.08,
            "filter.cutoff": 20000,
        },
    ),
    "Pitch Riser": (
        "FX/Riser",
        "riser, pitch, build",
        {
            "osc1.wave": 1,
            "unison.voices": 8,
            "unison.detune": 30,
            "pitchEnv.amount": -24,
            "pitchEnv.time": 2.0,
            "pitchEnv.curve": 1,
            "env.attack": 0.5,
            "env.sustain": 1,
            "env.release": 0.5,
            "filter.type": 2,
            "filter.cutoff": 2500,
            "filter.keyTrack": 1,
        },
    ),
    "Downlifter": (
        "FX/Riser",
        "downlifter, noise, sweep",
        {
            "osc1.level": 0,
            "osc2.level": 0,
            "noise.level": 1,
            "filter.cutoff": 200,
            "filter.envAmount": 5.3,
            "filterEnv.attack": 0.001,
            "filterEnv.decay": 4.0,
            "filterEnv.sustain": 0,
            "env.attack": 0.001,
            "env.decay": 4.0,
            "env.sustain": 0,
            "env.release": 0.3,
        },
    ),
    "Sub Drop": (
        "FX/Impact",
        "sub, drop, impact",
        {
            "osc1.wave": 0,
            "osc2.level": 0,
            "pitchEnv.amount": 24,
            "pitchEnv.time": 1.5,
            "pitchEnv.curve": 1,
            "osc1.octave": -2,
            "env.attack": 0.002,
            "env.decay": 1.5,
            "env.sustain": 0,
            "env.release": 0.3,
            "filter.cutoff": 20000,
        },
    ),
    "Laser Zap": (
        "FX/Impact",
        "laser, zap",
        {
            "osc1.wave": 1,
            "osc2.level": 0,
            "pitchEnv.amount": 36,
            "pitchEnv.time": 0.12,
            "env.attack": 0.001,
            "env.decay": 0.25,
            "env.sustain": 0,
            "env.release": 0.1,
        },
    ),
}

# --- racks: kits, layered voices, basses, transitions and chains -------------------------


@dataclass
class Layer:
    name: str
    preset: str = ""  # core/<name>, or empty with type
    type: str = ""
    keys: str = "0..127"
    gain: float = 0.0
    params: dict[str, float] = field(default_factory=dict)
    slots: list[str] = field(default_factory=list)


@dataclass
class Rack:
    name: str
    category: str
    tags: str
    layers: list[Layer] = field(default_factory=list)
    chain: list[str] = field(default_factory=list)
    sends: dict[str, tuple[float, list[str]]] = field(default_factory=dict)
    macros: dict[str, list[str]] = field(default_factory=dict)
    fallback: str = ""
    tempo: int = 0
    root: str = ""


def kit(
    name: str,
    voices: dict[str, str],
    chain: list[str],
    tags: str,
    params: dict[str, float] | None = None,
    synth: bool = True,
) -> Rack:
    """A GM-mapped kit: one layer per voice on its keys, the Drum Bus, Drums' macros
    (taxonomy.MACRO_SLOTS). `params` go on every layer; a sampled kit gets Drive only."""
    layers = [
        Layer(layer, preset=f"core/{preset}", keys=keys, params=dict(params or {}))
        for layer, (preset, keys) in ((k, tuple(v.split("@"))) for k, v in voices.items())
    ]
    if not synth:
        return Rack(
            name,
            "Drums/Kit",
            tags,
            layers,
            chain + DRUM_BUS,
            macros={"Drive": [f"chain.{len(chain) + 2}.drive range=0..12"]},
            tempo=96,
            root="C2",
        )
    macros = {
        "Pitch": [
            f"layer.{lay.name}.tune range=-12..12"
            for lay in layers
            if "Kick" in lay.preset or "Snare" in lay.preset or "Tom" in lay.preset
        ],
        "Decay": [
            f"layer.{lay.name}.decay range=0.05..1.2 curve=exponential(0.3)"
            for lay in layers
            if lay.name in ("kick", "snare")
        ],
        "Drive": [f"chain.{len(chain) + 2}.drive range=0..12"],
        "Tone": [
            f"layer.{lay.name}.tone range=0.2..0.95"
            for lay in layers
            if lay.name in ("snare", "hat")
        ],
    }
    return Rack(
        name, "Drums/Kit", tags, layers, chain + DRUM_BUS, macros=macros, tempo=96, root="C2"
    )


DRUM_BUS = [
    "Compressor threshold=-12 ratio=4 attack=10 release=80",
    "Saturation model=0 drive=4",
    "Clipper mode=0 drive=0 ceiling=-1",
]

# General MIDI's toms sit between its hats (41, 43, 45, 47, 48, 50), so each is its own
# layer and the hat layer keeps 42..46 to itself.
TOMS = {
    "tomfloor": "808 Tom Low@41..41",
    "tomfloorhigh": "808 Tom Low@43..43",
    "tomlow": "808 Tom Mid@45..45",
    "tommid": "808 Tom Mid@47..48",
    "tomhigh": "808 Tom High@50..50",
}
KIT_808 = {
    "kick": "808 Kick Short@35..36",
    "snare": "808 Snare@37..38",
    "clap": "808 Clap@39..39",
    "hat": "808 Hat Pair@42..46",
    **TOMS,
    "cymbal": "808 Cymbal@49..49",
    "ride": "808 Cymbal@51..51",
    "cowbell": "808 Cowbell@56..56",
    "conga": "808 Conga Mid@60..64",
    "clave": "808 Clave@75..77",
}
KIT_909 = {
    **KIT_808,
    "kick": "909 Kick@35..36",
    "snare": "909 Snare@37..38",
    "hat": "909 Hat@42..46",
}
# The Trap Snare rack's two voices, as two layers on the snare's keys (a layer names a
# preset, not a rack).
KIT_TRAP = {
    **KIT_808,
    "kick": "Trap Kick@35..36",
    "snare": "909 Snare@37..38",
    "snareclap": "808 Clap@37..38",
    "hat": "Trap Hat@42..46",
}

RACKS: list[Rack] = [
    Rack(
        "909 Kick Hard",
        "Drums/Kick",
        "909, kick, hard",
        [Layer("kick", "core/909 Kick")],
        ["Clipper mode=0 drive=9", "ParametricEq band1.type=1 band1.freq=60 band1.gain=3"],
    ),
    Rack(
        "Techno Rumble Kick",
        "Drums/Kick",
        "techno, kick, rumble",
        [Layer("kick", "core/909 Kick")],
        sends={
            "rumble": (
                -6.0,
                [
                    "Hall size=0.6 decay=2.5",
                    "Filter type=0 cutoff=180",
                    "VolumeShaper length=1 shape=0 depth=0.9",
                ],
            )
        },
        tempo=132,
    ),
    Rack(
        "Trap Snare",
        "Drums/Snare",
        "trap, snare, layered",
        [Layer("snare", "core/909 Snare"), Layer("clap", "core/808 Clap", gain=-4)],
        ["Clipper mode=0 drive=4"],
    ),
    Rack(
        "Stacked Clap",
        "Drums/Clap",
        "clap, layered",
        [
            Layer("synth", "core/808 Clap"),
            Layer("real", "core/Acoustic Clap", gain=-3),
            Layer("snap", "core/Snap", gain=-6),
        ],
        sends={"room": (-16.0, ["Hall size=0.3 decay=0.6"])},
        fallback="core/808 Clap",
    ),
    kit("808 Kit", KIT_808, [], "808, kit"),
    kit("909 Kit", KIT_909, [], "909, kit"),
    kit("707 Machine Kit", KIT_808, [], "707, kit", params={"character": 2}),
    kit("Trap Kit", KIT_TRAP, [], "trap, kit"),
    kit(
        "Lo-Fi Kit",
        KIT_808,
        ["Tape age=0.6 hiss=0.1 wow.depth=0.2", "Bitcrush bits=12"],
        "lofi, kit, tape",
    ),
    kit(
        "Acoustic Studio Kit",
        {
            "kick": "Acoustic Kick@35..36",
            "snare": "Acoustic Snare@37..38",
            "clap": "Acoustic Clap@39..39",
            "hat": "Acoustic Hat@42..46",
            **TOMS,
            "cymbal": "Crash@49..49",
            "ride": "Suspended Cymbal@51..51",
            "tambourine": "Tambourine@54..54",
        },
        [],
        "acoustic, kit",
        synth=False,
    ),
    kit("Hyper Kit", KIT_909, ["Clipper mode=1 drive=10", "OTT depth=0.3"], "hyperpop, kit, hard"),
    Rack(
        "808 Distorted",
        "Bass/808",
        "808, bass, distorted",
        [Layer("808", "core/808 Clean")],
        [
            "Overdrive drive=20 tone=3500",
            "Clipper mode=0 drive=6",
            "ParametricEq band1.type=3 band1.freq=25",
        ],
        macros={"Drive": ["chain.1.drive range=6..30"]},
        root="C1",
    ),
    Rack(
        "808 Punchy",
        "Bass/808",
        "808, bass, punchy",
        [Layer("808", "core/808 Kick Long", params={"decay": 1.2, "click": 0.5})],
        ["Saturation model=1 drive=6"],
        root="C1",
    ),
    Rack(
        "808 Reese",
        "Bass/Reese",
        "808, reese, bass",
        [
            Layer("808", "core/808 Clean"),
            Layer(
                "reese",
                type="va",
                gain=-10,
                params={
                    "osc1.wave": 1,
                    "osc2.wave": 1,
                    "osc2.fine": 12,
                    "osc1.fine": -12,
                    "filter.cutoff": 400,
                    "env.sustain": 1,
                },
            ),
        ],
        root="C1",
    ),
    Rack(
        "Noise Riser 8",
        "FX/Riser",
        "riser, noise, 8 bars",
        [
            Layer(
                "noise",
                type="va",
                params={
                    "osc1.level": 0,
                    "osc2.level": 0,
                    "noise.level": 1,
                    "filter.type": 3,
                    "filter.cutoff": 200,
                    "filter.envAmount": 5.3,
                    "filterEnv.attack": 10.0,
                    "filterEnv.sustain": 1,
                    "env.attack": 2.0,
                    "env.sustain": 1,
                    "env.release": 1.0,
                },
            )
        ],
        sends={"space": (-8.0, ["Hall size=0.8 decay=3"])},
        tempo=128,
    ),
    Rack(
        "Noise Riser 16",
        "FX/Riser",
        "riser, noise, 16 bars",
        [
            Layer(
                "noise",
                type="va",
                params={
                    "osc1.level": 0,
                    "osc2.level": 0,
                    "noise.level": 1,
                    "filter.type": 3,
                    "filter.cutoff": 200,
                    "filter.envAmount": 5.3,
                    "filterEnv.attack": 10.0,
                    "filterEnv.sustain": 1,
                    "env.attack": 4.0,
                    "env.sustain": 1,
                    "env.release": 1.0,
                },
            )
        ],
        sends={"space": (-8.0, ["Hall size=0.8 decay=3"])},
        tempo=128,
    ),
    Rack(
        "Impact",
        "FX/Impact",
        "impact, boom",
        [
            Layer("drop", "core/Sub Drop"),
            Layer("burst", "core/Shaker", params={"decay": 0.4}, gain=-6),
            Layer("crash", "core/Crash", gain=-8),
        ],
        sends={"space": (-6.0, ["Hall size=0.9 decay=6"])},
        fallback="core/Sub Drop",
    ),
    Rack(
        "Uplifter",
        "FX/Riser",
        "uplifter, resonant",
        [
            Layer(
                "noise",
                type="va",
                params={
                    "osc1.level": 0,
                    "osc2.level": 0,
                    "noise.level": 1,
                    "env.attack": 3.0,
                    "env.sustain": 1,
                    "env.release": 0.5,
                },
            )
        ],
        [
            "Resonator voices=3 note1=0 note2=7 note3=12 feedback=0.85",
            "Filter type=1 cutoff=400",
            "Saturation model=0 drive=0 output=12",
        ],
    ),
    Rack(
        "White Noise Wash",
        "FX/Texture",
        "noise, wash, breakdown",
        [
            Layer(
                "noise",
                type="va",
                params={
                    "osc1.level": 0,
                    "osc2.level": 0,
                    "noise.level": 1,
                    "env.attack": 2.0,
                    "env.sustain": 1,
                    "env.release": 3.0,
                },
            )
        ],
        ["Filter type=2 cutoff=1200 lfo.rate=0.2 lfo.depth=0.6"],
        sends={"space": (-4.0, ["Hall size=1 decay=8"])},
    ),
    Rack(
        "Laser Delay",
        "FX/Impact",
        "laser, delay",
        [Layer("zap", "core/Laser Zap")],
        ["Delay sync=1 timeMs=280 feedback=0.4"],
    ),
]

#: Effect-only racks (4.5): name -> (category, slots).
CHAINS: dict[str, tuple[str, list[str]]] = {
    "Drum Bus": ("Chain/Bus", DRUM_BUS),
    "Parallel Crush": (
        "Chain/Bus",
        [
            "Distortion drive=12",
            "Compressor threshold=-20 ratio=10",
            "ParametricEq band1.type=3 band1.freq=100",
        ],
    ),
    "Kick Pump 1-4": ("Chain/Kick", ["VolumeShaper length=1 shape=0 depth=0.8"]),
    "Kick Pump 1-8": ("Chain/Kick", ["VolumeShaper length=0.5 shape=0 depth=0.8"]),
    "Lo-Fi Bus": ("Chain/Bus", ["Tape age=0.6 hiss=0.1", "Bitcrush bits=12"]),
    "Room 0.6 s": ("Chain/Space", ["Hall size=0.3 decay=0.6"]),
    "Glue Master": (
        "Chain/Master",
        [
            "Compressor threshold=-8 ratio=2 attack=30 release=200",
            "Clipper mode=0 drive=2",
            "Limiter ceiling=-0.3 truePeak=1",
        ],
    ),
    "Trance Gate 16": ("Chain/Bus", ["VolumeShaper length=4 shape=0 steps=46517 depth=1"]),
    # 4.4's Tape Stop: GrossBeat's tape-stop pattern over a bar, as a chain to drop on a bus.
    "Tape Stop": ("Chain/Bus", ["GrossBeat pattern=4 length=3 depth=1"]),
}

# --- patterns (4.6): name -> (tempo, bars, {key: step string, 16 steps per bar}) ------------

PATTERNS: dict[str, tuple[int, int, dict[int, str]]] = {
    "four_on_floor": (124, 1, {36: "x...x...x...x...", 42: "..x...x...x...x."}),
    "house_offbeat_hats": (
        124,
        1,
        {36: "x...x...x...x...", 39: "....x.......x...", 46: "..x...x...x...x."},
    ),
    "trap_hats_rolls": (
        140,
        2,
        {
            36: "x.....x...x.....x.......x.x.....",
            38: "........x...............x.......",
            42: "x.x.x.x.xxxxx.x.x.x.x.x.xxxxxxxx",
        },
    ),
    "boom_bap": (90, 1, {36: "x.....x...x.....", 38: "....x.......x...", 42: "x.x.x.x.x.x.x.x."}),
    "breakbeat_funky": (
        100,
        1,
        {36: "x.....x..x......", 38: "....x..x.x..x..x", 42: "x.x.x.x.x.x.x.x."},
    ),
    "half_time": (
        140,
        2,
        {
            36: "x.......x.......x.........x.....",
            38: "........x...............x.......",
            42: "x...x...x...x...x...x...x...x...",
        },
    ),
    "dnb_two_step": (
        174,
        1,
        {36: "x.........x.....", 38: "....x.......x...", 42: "x.x.x.x.x.x.x.x."},
    ),
    "roll_build": (
        140,
        8,
        {38: "x...x...x...x..." * 2 + "x.x.x.x.x.x.x.x." * 2 + "xxxxxxxxxxxxxxxx" * 4},
    ),
    "gabber_pump": (180, 1, {36: "x...x...x...x...", 46: "..x...x...x...x."}),
    "phonk_cowbell": (
        130,
        2,
        {36: "x.....x.....x...x.....x...x.....", 56: "x..x..x.x..x..x.x..x..x.x.x.x..."},
    ),
    "jersey_club": (140, 1, {36: "x..x..x...x.x...", 38: "....x.......x..."}),
    "uk_garage_2step": (
        132,
        1,
        {36: "x......x..x.....", 38: "....x.......x...", 42: "..x...xx..x...x."},
    ),
    "amapiano_log": (
        112,
        1,
        {36: "x...x...x...x...", 62: "...x..x...x..x..", 75: "..x...x...x...x."},
    ),
    "hardstyle_kick_bass": (150, 1, {36: "x...x...x...x...", 46: "..x...x...x...x."}),
    "happy_hardcore": (
        170,
        1,
        {36: "x...x...x...x...", 38: "....x.......x...", 46: "..x...x...x...x."},
    ),
    "breakcore_7_4": (180, 1, {36: "x..x....x.x...", 38: "....x.x....x.x", 42: "xxxxxxxxxxxxxx"}),
}

KEY_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(key: int) -> str:
    return f"{KEY_NAMES[key % 12]}{key // 12 - 1}"


def fmt(v: float) -> str:
    return str(int(v)) if float(v).is_integer() else f"{v:g}"


def header(lines: list[str]) -> str:
    return "\n".join(lines) + "\n"


def write_preset(
    folder: str,
    name: str,
    type_: str,
    category: str,
    tags: str,
    params: dict[str, float],
    comment: str,
    fallback: str = "",
) -> Path:
    path = PACK / folder / (slug(name) + ".adxpreset")
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        f"# {comment}",
        "[PRESET]",
        f'NAME="{name}"',
        f"TYPE={type_}",
        "PACK=core",
        f"CATEGORY={category}",
        f"TAGS={tags}",
    ]
    if fallback:
        lines.append(f'FALLBACK="core/{fallback}"')
    lines += ["", "[PARAMS]"] + [f"{k}={fmt(v)}" for k, v in params.items()]
    path.write_text(header(lines), encoding="utf-8", newline="\n")
    return path


def slug(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def write_rack(folder: str, rack: Rack) -> Path:
    path = PACK / folder / (slug(rack.name) + ".adxrack")
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        f"# phase_13.md 4: {rack.name}.",
        "[RACK]",
        f'NAME="{rack.name}"',
        "PACK=core",
        f"CATEGORY={rack.category}",
        f"TAGS={rack.tags}",
    ]
    if rack.tempo:
        lines.append(f"TEMPO_HINT={rack.tempo}")
    if rack.root:
        lines.append(f"ROOT_HINT={rack.root}")
    if rack.fallback:
        lines.append(f'FALLBACK="{rack.fallback}"')
    lines.append("VERSION=1")
    for layer in rack.layers:
        lines += ["", f"[LAYER {layer.name}]"]
        lines.append(f'PRESET="{layer.preset}"' if layer.preset else f"TYPE={layer.type}")
        lines.append(f"KEYS={layer.keys}")
        if layer.gain:
            lines.append(f"GAIN={fmt(layer.gain)}")
        lines += [f"PARAM {k}={fmt(v)}" for k, v in layer.params.items()]
        lines += [f"SLOT {s}" for s in layer.slots]
    if rack.chain:
        lines += ["", "[CHAIN]"] + [f"SLOT {s}" for s in rack.chain]
    for send, (level, slots) in rack.sends.items():
        lines += ["", f"[SEND {send}]", f"LEVEL={fmt(level)}"] + [f"SLOT {s}" for s in slots]
    for macro, targets in rack.macros.items():
        targets = [t for t in targets if t]
        if targets:
            lines += ["", f"[MACRO {macro}]", "VALUE=0.5"] + [f"TARGET {t}" for t in targets]
    path.write_text(header(lines), encoding="utf-8", newline="\n")
    return path


def write_chain(name: str, category: str, slots: list[str]) -> Path:
    path = PACK / "chains" / (slug(name) + ".adxrack")
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        f"# phase_13.md 4.5: {name}, an effect-only rack.",
        "[RACK]",
        f'NAME="{name}"',
        "PACK=core",
        f"CATEGORY={category}",
        "TAGS=chain",
        "VERSION=1",
        "",
        "[CHAIN]",
    ]
    lines += [f"SLOT {s}" for s in slots]
    path.write_text(header(lines), encoding="utf-8", newline="\n")
    return path


def write_pattern(name: str, tempo: int, bars: int, rows: dict[int, str]) -> Path:
    """A drum pattern fragment: one Drums channel on the 808 Kit's GM keys."""
    path = PACK / "patterns" / f"{name}.adx"
    path.parent.mkdir(parents=True, exist_ok=True)
    steps = max(len(r) for r in rows.values())
    per_bar = steps // bars
    lines = [
        f"# phase_13.md 4.6: {name}. Paste it onto a kit channel; the keys are General MIDI's.",
        "[PROJECT]",
        "ADX_VERSION=2",
        "",
        "[TEMPO]",
        f"0:0:0 {tempo}",
        "",
        "[CHANNEL Drums]",
        "INSTRUMENT=drumsynth",
        "PARAM model=5",
        "OUTPUT=insert.1",
        "",
        f"[PATTERN {name}]",
        f"LENGTH={bars}:0:0",
        "NOTES Drums",
    ]
    for key, row in sorted(rows.items()):
        for i, c in enumerate(row):
            if c != "x":
                continue
            bar, step = divmod(i, per_bar)
            beat, sixteenth = divmod(step * 16 // per_bar, 4)
            ticks = sixteenth * 960
            accent = 110 if step % 4 == 0 else 90
            lines.append(f"  {note_name(key)} {bar}:{beat}:{ticks} 0:0:480 {accent}")
    lines += ["", "[MIXER]", 'INSERT 1 name="Master"']
    path.write_text(header(lines), encoding="utf-8", newline="\n")
    return path


#: The General MIDI key each sampled voice is moved to, so a kit's layer finds it there.
GM_KEY = {
    "Acoustic Kick": 36,
    "Acoustic Snare": 38,
    "Rope Snare": 38,
    "Piccolo Snare": 38,
    "Acoustic Clap": 39,
    "Crash": 49,
    "Suspended Cymbal": 51,
    "Tambourine": 54,
    "Woodblock": 76,
    "Agogo": 67,
    "Triangle": 81,
}


def move_to_gm_key(text: str, target: int) -> str:
    """Shift every zone's keys and root so the key most zones use lands on `target`: the
    articulations and round robins keep their spacing, and their pitch (root moves too)."""
    keys = [int(m) for m in re.findall(r" key=(\d+)", text)]
    if not keys:
        return text
    modal = max(set(keys), key=keys.count)
    shift = target - modal

    def moved(m: re.Match[str]) -> str:
        lo, hi = int(m.group(2)), int(m.group(4) or m.group(2))
        lo, hi = max(0, min(127, lo + shift)), max(0, min(127, hi + shift))
        return f" {m.group(1)}={lo}" + (f"..{hi}" if m.group(4) else "")

    text = re.sub(r" (key)=(\d+)(\.\.(\d+))?", moved, text)
    return re.sub(r" (root)=(\d+)(\.\.(\d+))?", moved, text)


def import_sampled(
    name: str, category: str, tags: str, sfz: str, fallback: str, extra: dict[str, float]
) -> Path | None:
    sys.path.insert(0, str(REPO / "tools" / "packs"))
    from fetch_libraries import LIBRARIES, store_root

    source = store_root() / LIBRARIES["vcsl"].folder / sfz
    if not source.is_file():
        print(f"  skipped {name}: no {source}")
        return None
    out = PACK / "drums"
    out.mkdir(parents=True, exist_ok=True)
    done = subprocess.run(
        [
            PY,
            str(REPO / "tools/packs/import_sfz.py"),
            str(source),
            "--pack",
            "core",
            "--category",
            category,
            "--out",
            str(out),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    made = out / (source.stem + ".adxpreset")
    if not made.is_file():
        print(f"  import failed for {name}: {done.stdout}{done.stderr}")
        return None
    target = out / (slug(name) + ".adxpreset")
    text = made.read_text(encoding="utf-8")
    text = re.sub(r'NAME="[^"]*"', f'NAME="{name}"', text, count=1)
    text = text.replace(
        "[PRESET]\n", f"# phase_13.md 4.1: {name}, VCSL's {source.stem} (CC0).\n[PRESET]\n", 1
    )
    text = text.replace(
        f"CATEGORY={category}\n",
        f'CATEGORY={category}\nTAGS={tags}\nFALLBACK="core/{fallback}"\n',
        1,
    )
    if extra:
        text = text.replace(
            "[PARAMS]\n", "[PARAMS]\n" + "".join(f"{k}={fmt(v)}\n" for k, v in extra.items()), 1
        )
    if name in GM_KEY:
        text = move_to_gm_key(text, GM_KEY[name])
    # The audition plays on the voice's key: its GM key, or the hat's closed hat.
    root = GM_KEY.get(name, 42 if "Hat" in name else 0)
    if root:
        text = text.replace("\nTAGS=", f"\nROOT_HINT={note_name(root)}\nTAGS=", 1)
    made.unlink()
    report = made.with_suffix(".import.txt")
    if report.is_file():
        report.replace(target.with_suffix(".import.txt"))
    target.write_text(text, encoding="utf-8", newline="\n")
    return target


def write_manifest() -> None:
    PACK.mkdir(parents=True, exist_ok=True)
    (PACK / "pack.adxpack").write_text(
        header(
            [
                "# phase_13.md: drums, kits, 808s, transitions, chains and patterns",
                "# that every later pack builds on.",
                "[PACK]",
                "ID=core",
                'NAME="Core Drums & Machines"',
                "VERSION=1",
                "REQUIRES=adx>=0.1",
                "LIBRARIES=vcsl",
                "LICENCE=CC0-1.0",
                "",
                "[ASSETS]",
                'ASSET "lib://vcsl/" licence=CC0-1.0 redistributable=yes '
                'credit="Versilian Studios"',
            ]
        ),
        encoding="utf-8",
        newline="\n",
    )


def build() -> list[Path]:
    written: list[Path] = []
    write_manifest()
    for name, (category, tags, params) in DRUMS.items():
        written.append(
            write_preset(
                "drums", name, "drumsynth", category, tags, params, f"phase_13.md 4.1: {name}."
            )
        )
    for name, (category, tags, params) in SYNTHS.items():
        folder = "bass" if category.startswith("Bass") else "fx"
        written.append(
            write_preset(
                folder, name, "va", category, tags, params, f"phase_13.md 4.3/4.4: {name}."
            )
        )
    for name, spec in SAMPLED.items():
        made = import_sampled(name, *spec)
        if made:
            written.append(made)
    for rack in RACKS:
        folder = {"Drums/Kit": "kits", "Bass/808": "bass", "Bass/Reese": "bass"}.get(
            rack.category, "fx" if rack.category.startswith("FX") else "drums"
        )
        written.append(write_rack(folder, rack))
    for name, (category, slots) in CHAINS.items():
        written.append(write_chain(name, category, slots))
    for name, (tempo, bars, rows) in PATTERNS.items():
        written.append(write_pattern(name, tempo, bars, rows))
    return written


# --- loudness calibration -------------------------------------------------------------------

LEVEL_PARAM = {"drumsynth": "level", "va": "level", "sampler": "gain"}


def calibrate(paths: list[Path], passes: int = 5) -> dict[str, float]:
    sys.path.insert(0, str(REPO / "tools" / "packs"))
    sys.path.insert(0, str(REPO / "app"))
    import audition

    from adx import taxonomy

    out: dict[str, float] = {}
    for path in paths:
        if path.suffix not in (".adxpreset", ".adxrack") or path.parent.name == "chains":
            continue
        for _ in range(passes):
            result = audition.audition(path, out=REPO / "build" / "core-audition")
            target = taxonomy.category(result["category"]).loudness_target
            error = target - float(result["lufs"])
            out[path.name] = float(result["lufs"])
            if abs(error) <= 0.5:
                break
            adjust(path, max(-24.0, min(24.0, error)))
    return out


def adjust(path: Path, db: float) -> None:
    """Move a sound's level by `db`: a preset's level parameter, or every layer's GAIN."""
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".adxpreset":
        type_ = re.search(r"^TYPE=(\S+)", text, re.M)
        param = LEVEL_PARAM.get(type_.group(1) if type_ else "", "level")
        default = -6.0 if param == "level" else 0.0
        m = re.search(rf"^{re.escape(param)}=(\S+)$", text, re.M)
        now = float(m.group(1)) if m else default
        new = max(-48.0, min(12.0, now + db))
        line = f"{param}={new:.2f}"
        text = (
            re.sub(rf"^{re.escape(param)}=\S+$", line, text, flags=re.M)
            if m
            else text.replace("[PARAMS]\n", f"[PARAMS]\n{line}\n", 1)
        )
    else:

        def bump(block: re.Match[str]) -> str:
            body = block.group(0)
            g = re.search(r"^GAIN=(\S+)$", body, re.M)
            now = float(g.group(1)) if g else 0.0
            new = f"GAIN={max(-48.0, min(24.0, now + db)):.2f}"
            if g:
                return re.sub(r"^GAIN=\S+$", new, body, flags=re.M)
            return re.sub(r"^(KEYS=\S+)$", rf"\1\n{new}", body, count=1, flags=re.M)

        text = re.sub(r"^\[LAYER [^\]]+\]\n(?:(?!^\[).*\n?)*", bump, text, flags=re.M)
    path.write_text(text, encoding="utf-8", newline="\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--calibrate", action="store_true")
    args = parser.parse_args(argv)
    written = build()
    print(f"wrote {len(written)} files under {PACK.relative_to(REPO)}")
    if args.calibrate:
        levels = calibrate(written)
        (REPO / "build" / "core-levels.json").write_text(
            json.dumps(levels, indent=2) + "\n", encoding="utf-8"
        )
        print(f"calibrated {len(levels)} sounds; build/core-levels.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
