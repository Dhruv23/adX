"""The hardstyle kick resample loop, automated (phase_12.md 4.9).

    python tools/packs/kick_lab.py KICK.adxrack [--low E1] [--high E2] [--passes 2]
        [--seconds 0.8] [--out DIR] [--name NAME] [--pack PACK]

Producers render a kick, reprocess it, and transpose it per note - and the loop can run
more than once. This does that per semitone instead of resampling one render: the rack is
rendered once at every key from `low` to `high`, so each tail is tuned by construction,
and each render then goes back through the rack's own chain (its [CHAIN], without the
layers) `passes - 1` more times. The result is a sampler preset with one zone per key,
written to the library store as `lib://rendered/<pack>/<name>/`. Deterministic: two runs
on the same rack write the same bytes (kick_lab_deterministic).
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path
from typing import Any

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import audition  # noqa: E402
import render_multisample  # noqa: E402
from wavio import write_wav  # noqa: E402

from adx import engine_bridge  # noqa: E402


def chain_only(rack_text: str) -> str:
    """The rack with its layers removed: what the kick goes back through."""
    return re.sub(r"^\[LAYER [^\]]*\][^\[]*", "", rack_text, flags=re.MULTILINE)


def through_chain(chain_rack: Path, audio: np.ndarray, seconds: float) -> np.ndarray:
    """`audio` played into the chain rack's insert from a playlist track."""
    base = '[PROJECT]\nADX_VERSION=2\n\n[MIXER]\nINSERT 1 name="Master"\n'
    project, _ = engine_bridge.Project.loads(base)
    result = dict(project.native.insert_rack(str(chain_rack), []))
    insert = int(result["insert"])
    with tempfile.TemporaryDirectory() as scratch:
        source = Path(scratch) / "pass.wav"
        write_wav(source, np.clip(audio, -1, 1), 48000)
        text = project.dumps() + (
            f"\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\nTRACK 1 out=insert.{insert}\n"
            f'  AUDIO "{source.as_posix()}" 0:0:0\n'
        )
        loaded, _ = engine_bridge.Project.loads(text, scratch)
        rendered = Path(scratch) / "out.wav"
        engine_bridge.render_to_file(loaded, str(rendered), tail_seconds=0.5)
        out, _ = audition.read_wav_float(rendered)
    length = round(seconds * 48000)
    out = out[:length]
    if len(out) < length:
        out = np.concatenate([out, np.zeros((length - len(out), out.shape[1]))])
    return out


def lab(
    rack: Path, out: Path, name: str, pack: str, low: int, high: int, passes: int, seconds: float
) -> dict[str, Any]:
    prepared = audition.prepare(rack, [str(audition.REPO / "engine/preset/packs")])
    folder = out / audition.safe(pack) / audition.safe(name)
    folder.mkdir(parents=True, exist_ok=True)
    chain_text = chain_only(str(engine_bridge._native.read_rack(str(rack))["text"]))
    lines = [
        "[PRESET]",
        f'NAME="{name}"',
        "TYPE=sampler",
        f"PACK={pack}",
        "CATEGORY=Drums/Kick",
        "",
        "[PARAMS]",
        "env.attack=0",
        "env.release=0.01",
        "oneShot=1",
        "",
        "[ZONES]",
    ]
    files = []
    with tempfile.TemporaryDirectory() as scratch:
        chain_rack = Path(scratch) / "chain.adxrack"
        chain_rack.write_text(chain_text, encoding="utf-8", newline="\n")
        has_chain = "[CHAIN]" in chain_text
        for key in range(low, high + 1):
            audio = render_multisample.render_note(prepared, key, 127, seconds, 0.0, rack.parent)
            for _ in range(max(0, passes - 1) if has_chain else 0):
                audio = through_chain(chain_rack, audio, seconds)
            file = f"{key:03d}.wav"
            write_wav(folder / file, np.clip(audio, -1, 1), 48000)
            files.append(folder / file)
            lines.append(
                f'ZONE "lib://rendered/{audition.safe(pack)}/{audition.safe(name)}/{file}" '
                f"key={key} root={key}"
            )
    preset = folder / f"{audition.safe(name)}.adxpreset"
    preset.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return {"preset": str(preset), "files": [str(f) for f in files]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("rack", type=Path)
    parser.add_argument("--low", default="E1")
    parser.add_argument("--high", default="E2")
    parser.add_argument("--passes", type=int, default=2)
    parser.add_argument("--seconds", type=float, default=0.8)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--name", default=None)
    parser.add_argument("--pack", default=None)
    args = parser.parse_args(argv)
    meta = audition.prepare(args.rack, [str(audition.REPO / "engine/preset/packs")]).meta
    result = lab(
        args.rack,
        args.out or render_multisample.default_out(),
        args.name or f"{meta['name']} (kick lab)",
        args.pack or meta["pack"] or "user",
        audition.midi_of(args.low),
        audition.midi_of(args.high),
        max(1, args.passes),
        args.seconds,
    )
    print(f"{len(result['files'])} keys, preset {result['preset']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
