"""The optimization pass's benchmark project (phase_10.md 4.5), generated.

    py -3.12 bench/make_project.py [OUT_DIR]

Deliberately larger than anything reasonable: 64 channels across every instrument type,
24 inserts with 6 effects each, 8 plugin instances, 200,000 notes, 30 warped audio clips,
40 automation lanes, 10 minutes. Seeded, so every run writes the same bytes; the
generator is what is committed, not its 7 MB of output (phase_10.md 11).

Writes OUT_DIR/full_project.adx and its samples (default build/bench/).
"""

from __future__ import annotations

import random
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))

from adx.engine_bridge import PPQ, type_catalog  # noqa: E402
from adx.testing.wav import write_wav  # noqa: E402

SEED = 10
BPM = 120
BAR = 4 * PPQ
BARS = 300  # 10 minutes at 120 bpm
CHANNELS = 64
INSERTS = 24
EFFECTS_PER_INSERT = 6
PLUGINS = 8
NOTES = 200_000
PATTERN_BARS = 10
AUDIO_CLIPS = 30
LANES = 40
#: Instruments that play samples: each gets one, as a ZONE.
SAMPLED = {"sampler", "slicer", "granular", "pool"}
#: Effects left out of the rotation, and why.
SKIP_EFFECTS = {"Convolution": "needs an impulse file", "Vocoder": "needs a sidechain to do work"}

PRESETS = ("windows-x64-release", "windows-x64-relwithdebinfo", "windows-x64-debug")


def fixture_plugin() -> Path | None:
    for preset in PRESETS:
        found = ROOT / "build" / preset / "fixtures" / "plugins" / "adx_test_plugin.clap"
        if found.exists():
            return found
    return None


def position(ticks: int) -> str:
    bar, rest = divmod(ticks, BAR)
    beat, tick = divmod(rest, PPQ)
    return f"{bar}:{beat}:{tick}"


def samples(folder: Path, rng: random.Random) -> list[str]:
    """Thirty 2-second sources: decaying tones and noise bursts, 48 kHz stereo."""
    folder.mkdir(parents=True, exist_ok=True)
    rate = 48000
    t = np.arange(2 * rate) / rate
    names = []
    for i in range(AUDIO_CLIPS):
        if i % 3 == 0:
            tone = np.sin(2 * np.pi * (110 * 2 ** (rng.randrange(24) / 12)) * t) * np.exp(-3 * t)
        else:
            tone = np.random.default_rng(i).standard_normal(len(t)) * np.exp(-6 * (t % 0.5))
        audio = np.stack([tone, tone * 0.9], axis=1).astype(np.float32) * 0.3
        name = f"samples/clip{i:02d}.wav"
        write_wav(folder.parent / name, audio, rate)
        names.append(name)
    return names


def build(out: Path, bars: int = BARS) -> Path:
    """``bars`` shorter than BARS keeps the note density (for a quick look)."""
    rng = random.Random(SEED)
    catalog = type_catalog()
    instruments = [t["name"] for t in catalog if t["kind"] == "instrument"]
    effects = [
        t["name"] for t in catalog if t["kind"] == "effect" and t["name"] not in SKIP_EFFECTS
    ]
    sources = samples(out / "samples", rng)
    plugin = fixture_plugin()

    lines = [
        "[PROJECT]",
        "ADX_VERSION=2",
        'TITLE="Benchmark: the full project"',
        "",
        "[TEMPO]",
        f"0:0:0 {BPM}",
        "",
        "[METER]",
        "0:0:0 4/4",
        "",
    ]
    channel_names = []
    for c in range(CHANNELS):
        kind = instruments[c % len(instruments)]
        name = f"Ch{c + 1:02d}"
        channel_names.append(name)
        lines += [
            f"[CHANNEL {name}]",
            f"INSTRUMENT={kind}",
            f"OUTPUT=insert.{2 + c % INSERTS}",
            "POLYPHONY=16",
            "VOLUME=0.3",
        ]
        if kind in SAMPLED:
            lines.append(f'ZONE "{sources[c % len(sources)]}" root=60')
        lines.append("")

    # Patterns: ten bars each, every channel in each, the notes spread evenly.
    patterns = bars // PATTERN_BARS
    total = NOTES * bars // BARS
    per_cell = total // (patterns * CHANNELS)
    extra = total - per_cell * patterns * CHANNELS
    for p in range(patterns):
        lines += [f"[PATTERN S{p + 1:02d}]", f"LENGTH={PATTERN_BARS}:0:0"]
        for c, name in enumerate(channel_names):
            count = per_cell + (1 if p * CHANNELS + c < extra else 0)
            lines.append(f"NOTES {name}")
            base = 36 + (c * 7) % 48
            starts = sorted(rng.randrange(PATTERN_BARS * 16) * (PPQ // 4) for _ in range(count))
            for start in starts:
                length = rng.choice((PPQ // 4, PPQ // 2, PPQ))
                pitch = base + rng.randrange(12)
                lines.append(
                    f"  {pitch} {position(start)} {position(length)} {rng.randint(60, 120)}"
                )
        lines.append("")

    lines += ["[PLAYLIST]", 'TRACK 1 name="Music"']
    lines += [f"  PATTERN S{p + 1:02d} {p * PATTERN_BARS}:0:0" for p in range(patterns)]
    lines.append('TRACK 2 name="Audio"')
    for i, source in enumerate(sources):
        at = i * bars // AUDIO_CLIPS
        lines.append(f'  AUDIO "{source}" {at}:0:0 length=2:0:0 stretch={1.0 + (i % 5) * 0.1:g}')
        lines.append("    WARP 0 0:0:0")
        lines.append(f"    WARP 1 {1 + i % 2}:0:0")
    for lane in range(LANES):
        insert = 2 + lane % INSERTS
        lines.append(f'TRACK {3 + lane} name="Lane {lane + 1}"')
        lines.append(f"  AUTOMATION insert.{insert}.gain 0:0:0")
        for point in range(0, bars + 1, 8):
            lines.append(f"    {point}:0:0 {0.4 + 0.5 * rng.random():.3f} linear")
    lines.append("")

    lines += ["[MIXER]", 'INSERT 1 name="Master"']
    hosted = 0
    for i in range(INSERTS):
        lines.append(f'INSERT {i + 2} name="Bus {i + 1}"')
        for s in range(EFFECTS_PER_INSERT):
            slot = i * EFFECTS_PER_INSERT + s + 1  # slot ids are unique across the file
            if (
                s == EFFECTS_PER_INSERT - 1
                and plugin is not None
                and hosted < PLUGINS
                and i % 3 == 0
            ):
                path = str(plugin).replace("\\", "/")
                lines.append(
                    f'  SLOT {slot} clap plugin="com.adx.test.effect" name="adX Test Effect" path="{path}"'
                )
                hosted += 1
            else:
                lines.append(f"  SLOT {slot} {effects[(slot - 1) % len(effects)]}")
        lines.append(f"ROUTE insert.{i + 2} -> insert.1")
    lines.append("")
    project = out / "full_project.adx"
    project.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    return project


def main() -> int:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "bench"
    out.mkdir(parents=True, exist_ok=True)
    project = build(out)
    print(project)
    return 0


if __name__ == "__main__":
    sys.exit(main())
