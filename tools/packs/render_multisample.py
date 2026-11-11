"""An adX preset or rack, rendered to a multisample (phase_12.md 4.9).

    python tools/packs/render_multisample.py PRESET_OR_RACK [--low C1] [--high C7]
        [--step 3] [--velocities 40,90,127] [--round-robins 2] [--seconds 3] [--tail 2]
        [--out DIR] [--name NAME] [--pack PACK]

Each note every `step` semitones, at each velocity, `round-robins` times, is rendered
through the engine (the round robins differ by their voices' seeds, which come from the
note's identity). The renders are written as 24-bit WAVs and a sampler preset is written
whose zones cover the keyboard between them, velocity-layered, round-robin grouped,
naming the files `lib://rendered/<pack>/<name>/...`. The default output folder is that
place in the library store, so the preset plays at once. The same input renders the same
files, byte for byte.
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path
from typing import Any

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import audition  # noqa: E402
from wavio import write_wav  # noqa: E402

from adx import engine_bridge  # noqa: E402

TPB = audition.TPB


def render_note(
    prepared: audition.Prepared,
    key: int,
    velocity: int,
    seconds: float,
    tail: float,
    base: Path,
    instance: int = 0,
) -> np.ndarray:
    """One held note on every channel, `seconds` at 120 bpm (2 beats a second)."""
    beats = seconds * 2.0
    lead = 0.25 * instance  # a later start gives the round robin another identity
    notes = []
    for channel in prepared.channels:
        quoted = f'"{channel}"' if " " in channel or "/" in channel else channel
        notes.append(f"NOTES {quoted}")
        notes.append(f"  {key} 0:0:{round(lead * TPB)} 0:0:{round(beats * TPB)} {velocity}")
    bars = int(np.ceil((beats + lead) / 4)) + 1
    text = audition.with_notes(prepared, "\n".join(notes), bars, tempo=120.0)
    project, _ = engine_bridge.Project.loads(text, str(base))
    with tempfile.TemporaryDirectory() as scratch:
        out = Path(scratch) / "note.wav"
        engine_bridge.render_to_file(project, str(out), tail_seconds=tail)
        audio, _ = audition.read_wav_float(out)
    start = round(lead * 0.5 * 48000)
    length = round((seconds + tail) * 48000)
    clip = audio[start : start + length]
    if len(clip) < length:
        clip = np.concatenate([clip, np.zeros((length - len(clip), clip.shape[1]))])
    return clip


def zone_ranges(keys: list[int]) -> list[tuple[int, int]]:
    """Each rendered key's range: halfway to its neighbours, the ends to 0 and 127."""
    ranges = []
    for i, key in enumerate(keys):
        low = 0 if i == 0 else (keys[i - 1] + key) // 2 + 1
        high = 127 if i == len(keys) - 1 else (key + keys[i + 1]) // 2
        ranges.append((low, high))
    return ranges


def velocity_ranges(velocities: list[int]) -> list[tuple[int, int]]:
    out = []
    for i, velocity in enumerate(velocities):
        low = 1 if i == 0 else velocities[i - 1] + 1
        high = 127 if i == len(velocities) - 1 else velocity
        out.append((low, high))
    return out


def render(
    path: Path,
    out: Path,
    name: str,
    pack: str,
    low: int,
    high: int,
    step: int,
    velocities: list[int],
    round_robins: int,
    seconds: float,
    tail: float,
    packs: list[str] | None = None,
) -> dict[str, Any]:
    prepared = audition.prepare(path, packs or [str(audition.REPO / "engine/preset/packs")])
    keys = list(range(low, high + 1, step))
    folder = out / audition.safe(pack) / audition.safe(name)
    folder.mkdir(parents=True, exist_ok=True)
    lines = [
        "[PRESET]",
        f'NAME="{name}"',
        "TYPE=sampler",
        f"PACK={pack}",
        f"CATEGORY={prepared.meta['category']}",
        "",
        "[PARAMS]",
        "env.attack=0",
        "env.release=0.2",
        "",
        "[ZONES]",
    ]
    written = []
    for key, (key_low, key_high) in zip(keys, zone_ranges(keys), strict=True):
        for velocity, (vel_low, vel_high) in zip(
            velocities, velocity_ranges(velocities), strict=True
        ):
            for rr in range(round_robins):
                audio = render_note(prepared, key, velocity, seconds, tail, path.parent, rr)
                file = f"{key:03d}_v{velocity:03d}_rr{rr + 1}.wav"
                write_wav(folder / file, np.clip(audio, -1, 1), 48000)
                written.append(folder / file)
                group = f" rr={1 + keys.index(key)}:{rr}" if round_robins > 1 else ""
                lines.append(
                    f'ZONE "lib://rendered/{audition.safe(pack)}/{audition.safe(name)}/{file}" '
                    f"key={key_low}..{key_high} root={key} vel={vel_low}..{vel_high}{group}"
                )
    preset = folder / f"{audition.safe(name)}.adxpreset"
    preset.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return {"preset": str(preset), "files": [str(f) for f in written]}


def default_out() -> Path:
    return Path(str(engine_bridge._native.library_store())) / "rendered"


def parse_velocities(text: str) -> list[int]:
    return sorted({max(1, min(127, int(v))) for v in text.split(",") if v.strip()})


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("file", type=Path)
    parser.add_argument("--low", default="C1")
    parser.add_argument("--high", default="C7")
    parser.add_argument("--step", type=int, default=3)
    parser.add_argument("--velocities", default="40,90,127")
    parser.add_argument("--round-robins", type=int, default=2)
    parser.add_argument("--seconds", type=float, default=3.0)
    parser.add_argument("--tail", type=float, default=2.0)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--name", default=None)
    parser.add_argument("--pack", default=None)
    args = parser.parse_args(argv)
    meta = audition.prepare(args.file, [str(audition.REPO / "engine/preset/packs")]).meta
    result = render(
        args.file,
        args.out or default_out(),
        args.name or f"{meta['name']} (rendered)",
        args.pack or meta["pack"] or "user",
        audition.midi_of(args.low),
        audition.midi_of(args.high),
        max(1, args.step),
        parse_velocities(args.velocities),
        max(1, args.round_robins),
        args.seconds,
        args.tail,
    )
    print(f"{len(result['files'])} files, preset {result['preset']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
