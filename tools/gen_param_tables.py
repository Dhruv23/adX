"""Generates the long, regular parameter tables that are tedious and error-prone by hand.

Run from the repository root:  python tools/gen_param_tables.py

Writes engine/instruments/additive/AdditiveParams.h. The output is committed; the
build never runs this. Re-run it after editing the table below, and review the diff.
"""

from __future__ import annotations

from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
HARMONICS = 64

# name, min, max, default, unit, scale, rate
# rate: S = Sample (read per frame), B = Block (control grid)
ADDITIVE_HEAD = [
    ("env.attack", 0.0, 10.0, 0.01, "Seconds", "Logarithmic", "B"),
    ("env.decay", 0.0, 10.0, 0.2, "Seconds", "Logarithmic", "B"),
    ("env.sustain", 0.0, 1.0, 0.7, "Normalized", "Linear", "B"),
    ("env.release", 0.0, 10.0, 0.2, "Seconds", "Logarithmic", "B"),
    ("drive", 0.0, 48.0, 0.0, "Decibels", "Linear", "S"),
    ("filter.cutoff", 20.0, 20000.0, 20000.0, "Hertz", "Logarithmic", "B"),
    ("filter.lfoRate", 0.0, 40.0, 0.0, "Hertz", "Logarithmic", "B"),
    ("filter.lfoDepth", 0.0, 1.0, 0.0, "Normalized", "Linear", "B"),
    ("sub.level", 0.0, 1.0, 0.0, "Normalized", "Linear", "S"),
    ("sub.wave", 0.0, 1.0, 0.0, "Count", "Stepped", "B"),
    ("sub.dropSemitones", 0.0, 48.0, 0.0, "Semitones", "Linear", "B"),
    ("sub.dropMs", 0.0, 1000.0, 50.0, "Milliseconds", "Logarithmic", "B"),
    ("noise.level", 0.0, 1.0, 0.0, "Normalized", "Linear", "S"),
    ("noise.type", 0.0, 1.0, 0.0, "Count", "Stepped", "B"),
    ("osc.wave", 0.0, 3.0, 0.0, "Count", "Stepped", "B"),
    ("osc.unison", 1.0, 7.0, 1.0, "Count", "Stepped", "B"),
    ("osc.detuneCents", 0.0, 100.0, 0.0, "Cents", "Linear", "B"),
    ("osc.pulseWidth", 0.0, 1.0, 0.5, "Normalized", "Linear", "B"),
    ("resfilter.type", 0.0, 2.0, 0.0, "Count", "Stepped", "B"),
    ("resfilter.cutoff", 20.0, 20000.0, 20000.0, "Hertz", "Logarithmic", "B"),
    ("resfilter.resonance", 0.0, 1.0, 0.0, "Normalized", "Linear", "B"),
    ("resfilter.envAmount", -1.0, 1.0, 0.0, "Normalized", "Linear", "B"),
    ("resfilter.keyTrack", 0.0, 1.0, 0.0, "Normalized", "Linear", "B"),
    ("filterEnv.attack", 0.0, 10.0, 0.0, "Seconds", "Logarithmic", "B"),
    ("filterEnv.decay", 0.0, 10.0, 0.0, "Seconds", "Logarithmic", "B"),
    ("filterEnv.sustain", 0.0, 1.0, 1.0, "Normalized", "Linear", "B"),
    ("filterEnv.release", 0.0, 10.0, 0.0, "Seconds", "Logarithmic", "B"),
    ("formant.vowelA", 0.0, 4.0, 0.0, "Count", "Stepped", "B"),
    ("formant.vowelB", 0.0, 4.0, 0.0, "Count", "Stepped", "B"),
    ("formant.morph", 0.0, 1.0, 0.0, "Normalized", "Linear", "B"),
    ("formant.amount", 0.0, 1.0, 0.0, "Normalized", "Linear", "B"),
    ("vibrato.rate", 0.0, 20.0, 0.0, "Hertz", "Logarithmic", "B"),
    ("vibrato.depthCents", 0.0, 200.0, 0.0, "Cents", "Linear", "B"),
    ("vibrato.delayMs", 0.0, 5000.0, 0.0, "Milliseconds", "Logarithmic", "B"),
    ("glide.ms", 0.0, 5000.0, 0.0, "Milliseconds", "Logarithmic", "B"),
    ("harmonics.series", 0.0, 4.0, 0.0, "Count", "Stepped", "B"),
    ("morph.target", 0.0, 4.0, 0.0, "Count", "Stepped", "B"),
    ("morph.amount", 0.0, 1.0, 0.0, "Normalized", "Linear", "B"),
]

CURVED = ["env.attack", "env.decay", "env.release"]
CURVE_PARTS = [
    ("Kind", 0.0, 6.0, 0.0),
    ("Tension", -1.0, 1.0, 0.0),
    ("C1x", 0.0, 1.0, 0.33),
    ("C1y", -1.0, 2.0, 0.0),
    ("C2x", 0.0, 1.0, 0.67),
    ("C2y", -1.0, 2.0, 1.0),
]


def enum_name(name: str) -> str:
    out = []
    for part in name.replace("[", ".").replace("]", "").split("."):
        out.append(part[:1].upper() + part[1:])
    return "".join(out)


def row(
    name: str,
    lo: float,
    hi: float,
    default: float,
    unit: str,
    scale: str,
    rate: str,
    curve: str = "None",
) -> str:
    rate_name = {"S": "Sample", "B": "Block"}[rate]
    return (
        f'    {{"{name}", {lo!r}F, {hi!r}F, {default!r}F, project::Unit::{unit}, '
        f"project::ScaleKind::{scale}, project::RateClass::{rate_name}, "
        f"project::CurvePart::{curve}}},"
    )


def additive() -> str:
    rows: list[str] = []
    enums: list[str] = []
    for entry in ADDITIVE_HEAD:
        rows.append(row(*entry))
        enums.append(enum_name(entry[0]))
    for owner in CURVED:
        for part, lo, hi, default in CURVE_PARTS:
            rows.append(
                row(
                    owner,
                    lo,
                    hi,
                    default,
                    "Count" if part == "Kind" else "Normalized",
                    "Stepped" if part == "Kind" else "Linear",
                    "B",
                    part,
                )
            )
            enums.append(enum_name(owner) + "Curve" + part)
    for h in range(1, HARMONICS + 1):
        level = 1.0 if h == 1 else 0.0
        rows.append(row(f"harmonic.{h}", 0.0, 1.0, level, "Normalized", "Linear", "B"))
    enums.append("Harmonic1")
    enums.append(f"HarmonicLevelEnd = Harmonic1 + {HARMONICS} - 1")
    for h in range(1, HARMONICS + 1):
        rows.append(row(f"harmonic.{h}.detune", -100.0, 100.0, 0.0, "Cents", "Linear", "B"))
    for h in range(1, HARMONICS + 1):
        rows.append(row(f"harmonic.{h}.phase", 0.0, 1.0, 0.0, "Normalized", "Linear", "B"))

    enum_lines = []
    for e in enums:
        if e.startswith("HarmonicLevelEnd"):
            continue
        enum_lines.append(f"    {e},")
    return f"""// GENERATED by tools/gen_param_tables.py - edit the generator, not this file.
//
// The Additive instrument's parameters, in the order its node reads them. The first
// block is iteration one's patch, named as the v1 shim names it (phase_4.md §4.3);
// the harmonics extend v1's sixteen to {HARMONICS}, each with a level, a detune in cents and
// a start phase. The three curve blocks are not parameters of their own: they carry
// the `curve=` of env.attack, env.decay and env.release to a node that only sees
// floats (ParamDescriptor.h, CurvePart).
#pragma once

#include <array>
#include <cstdint>

#include "engine/project/ParamDescriptor.h"

namespace adx::instruments {{

inline constexpr std::uint32_t kAdditiveHarmonics = {HARMONICS};

enum class AdditiveParam : std::uint32_t {{
{chr(10).join(enum_lines)}
    HarmonicDetune1 = Harmonic1 + kAdditiveHarmonics,
    HarmonicPhase1 = HarmonicDetune1 + kAdditiveHarmonics,
    Count = HarmonicPhase1 + kAdditiveHarmonics,
}};

// NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table.
inline constexpr auto kAdditiveParams = std::to_array<project::ParamDescriptor>({{
{chr(10).join(rows)}
}});
// NOLINTEND(modernize-use-designated-initializers)

static_assert(kAdditiveParams.size() == static_cast<std::size_t>(AdditiveParam::Count));

}} // namespace adx::instruments
"""


def main() -> None:
    target = REPO / "engine" / "instruments" / "additive" / "AdditiveParams.h"
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(target, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(additive())
    print(f"wrote {target.relative_to(REPO)}")


if __name__ == "__main__":
    main()
