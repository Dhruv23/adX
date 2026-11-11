"""Audition presets and racks with their category's standard phrase (phase_12.md 4.11).

    python tools/packs/audition.py PRESET_OR_RACK... [--out DIR] [--packs DIR...]

Each file is played at its hinted tempo and root: leads a two-bar melody, pads a held
four-voice chord, kicks a four-on-the-floor bar that moves its root, and so on. The tool
writes the render as a WAV, a spectrum PNG and a JSON with integrated LUFS (the engine's
BS.1770-4 meter), true peak (4x oversampled) and whether the loudness sits within
taxonomy.LOUDNESS_WINDOW of the category's target. The browser plays the cached WAV on
click without loading the preset. Default output: %LOCALAPPDATA%/adX/audition/<pack>/.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "app"))

from adx import engine_bridge, taxonomy  # noqa: E402

TPB = 960  # ticks per beat
NOTE_NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


@dataclass(frozen=True)
class Hit:
    beat: float
    offset: int  # semitones from the root, or an absolute key when the phrase is a kit
    beats: float
    velocity: int = 100


def _melody() -> list[Hit]:
    line = [
        (0, 0, 1),
        (1, 4, 1),
        (2, 7, 1),
        (3, 12, 1),
        (4, 11, 0.5),
        (4.5, 7, 0.5),
        (5, 4, 1),
        (6, 0, 2),
    ]
    return [Hit(b, o, d) for b, o, d in line]


def _chord(beats: float) -> list[Hit]:
    return [Hit(0, o, beats, 90) for o in (0, 4, 7, 11)]


def _arpeggio() -> list[Hit]:
    return [Hit(i * 0.5, (0, 7, 12, 16, 19, 16, 12, 7)[i % 8], 0.5) for i in range(16)]


def _bass() -> list[Hit]:
    roots = (0, 0, 0, 0, 5, 5, 7, 7)
    return [Hit(i * 0.5, roots[i], 0.45, 110) for i in range(8)] + [
        Hit(4 + (i * 0.5), roots[i] - 2 if i >= 4 else roots[i], 0.45, 110) for i in range(8)
    ]


def _kick() -> list[Hit]:
    moves = (0, 0, 5, 7)
    return [Hit(b, moves[b // 4 % 4] if b >= 4 else 0, 0.9, 120) for b in range(8)]


def _kit() -> list[Hit]:
    hits = [Hit(b, 36, 0.25, 120) for b in (0, 2.5, 4, 6.5)]
    hits += [Hit(b, 38, 0.25, 110) for b in (1, 3, 5, 7)]
    hits += [Hit(i * 0.5, 42, 0.2, 80) for i in range(16)]
    return hits


#: Category -> (phrase, beats long, the phrase is in absolute keys).
PHRASES: dict[str, tuple[list[Hit], float, bool]] = {
    "Drums/Kick": (_kick(), 8, False),
    "Drums/Snare": ([Hit(b, 0, 0.5, 110) for b in (1, 3, 5, 7)], 8, False),
    "Drums/Clap": ([Hit(b, 0, 0.5, 110) for b in (1, 3, 5, 7)], 8, False),
    "Drums/Hat": ([Hit(i * 0.5, 0, 0.25, 90) for i in range(16)], 8, False),
    "Drums/Cymbal": ([Hit(0, 0, 4, 110)], 8, False),
    "Drums/Perc": ([Hit(i * 0.75, 0, 0.25, 100) for i in range(10)], 8, False),
    "Drums/Kit": (_kit(), 8, True),
    "Drums/Break": (_kit(), 8, True),
    "Bass": (_bass(), 8, False),
    "Lead": (_melody(), 8, False),
    "Pluck": (_arpeggio(), 8, False),
    "Guitar": (_arpeggio(), 8, False),
    "Keys": (
        [*_chord(3.5), *[Hit(4 + h.beat, h.offset + 5, 3.5, 90) for h in _chord(3.5)]],
        8,
        False,
    ),
    "Pad": (_chord(15), 16, False),
    "Strings": (_chord(7.5), 8, False),
    "Brass": (_chord(3.5), 8, False),
    "Woodwind": (_melody(), 8, False),
    "Choir": (_chord(7.5), 8, False),
    "FX": ([Hit(0, 0, 7, 110)], 8, False),
    "Chain": (_melody(), 8, False),
}

DEFAULT_ROOT = {"Bass": "C2", "Drums": "C2", "Lead": "C4", "Pad": "C3", "Keys": "C3"}


def midi_of(name: str) -> int:
    """C4 = 60; a plain number is taken as is."""
    if re.fullmatch(r"\d+", name):
        return int(name)
    match = re.fullmatch(r"([A-Ga-g])([#b]?)(-?\d+)", name.strip())
    if match is None:
        raise ValueError(f"not a note name: {name!r}")
    letter, accidental, octave = match.groups()
    pitch = NOTE_NAMES[letter.upper()] + (
        1 if accidental == "#" else -1 if accidental == "b" else 0
    )
    return (int(octave) + 1) * 12 + pitch


def phrase_for(category_path: str) -> tuple[list[Hit], float, bool]:
    cat = taxonomy.category(category_path)
    return PHRASES.get(cat.path, PHRASES.get(cat.top, PHRASES["Lead"]))


def _ticks(beats: float) -> str:
    return f"0:0:{round(beats * TPB)}"


def notes_text(channels: list[str], hits: list[Hit], root: int, absolute: bool) -> str:
    lines = []
    for channel in channels:
        lines.append(
            f'NOTES "{channel}"' if " " in channel or "/" in channel else f"NOTES {channel}"
        )
        for hit in hits:
            key = hit.offset if absolute else root + hit.offset
            key = max(0, min(127, key))
            lines.append(f"  {key} {_ticks(hit.beat)} {_ticks(hit.beats)} {hit.velocity}")
    return "\n".join(lines)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def header_value(text: str, key: str) -> str:
    match = re.search(rf"^{key}=(.*)$", text, re.MULTILINE)
    return match.group(1).strip().strip('"') if match else ""


@dataclass
class Prepared:
    """A preset or rack on fresh channels: the project's text without patterns."""

    text: str
    channels: list[str]
    meta: dict[str, Any]


def with_notes(prepared: Prepared, notes: str, bars: int, tempo: float | None = None) -> str:
    """`prepared`'s text with one pattern of `notes` placed at the start."""
    current = re.sub(r"^\[TEMPO\][^\[]*", "", prepared.text, flags=re.MULTILINE)
    bpm = tempo if tempo is not None else float(prepared.meta["tempo"])
    return current + (
        f"\n[TEMPO]\n0:0:0 {bpm:g}\n\n[PATTERN Audition]\nLENGTH={bars}:0:0\n"
        f"{notes}\n\n[PLAYLIST]\nTRACK 1\n  PATTERN Audition 0:0:0\n"
    )


def prepare(path: Path, packs: list[str]) -> Prepared:
    """The preset (instrument or effect) or rack in a project of its own."""
    project, meta = _applied(path, packs)
    return Prepared(
        text=project, channels=re.findall(r"^\[CHANNEL (.+)\]$", project, re.MULTILINE), meta=meta
    )


def build_project(path: Path, packs: list[str]) -> tuple[engine_bridge.Project, dict[str, Any]]:
    """The audition project: the preset or rack on fresh channels, the phrase on them."""
    prepared = prepare(path, packs)
    meta = prepared.meta
    hits, beats, absolute = phrase_for(meta["category"])
    top = taxonomy.category(meta["category"]).top
    root = midi_of(meta["root"] or DEFAULT_ROOT.get(top, "C3"))
    bars = max(1, int(np.ceil(beats / 4)))
    current = with_notes(prepared, notes_text(prepared.channels, hits, root, absolute), bars)
    loaded, diagnostics = engine_bridge.Project.loads(current, str(path.parent))
    meta["diagnostics"] += [dict(d) for d in diagnostics if d.get("severity") == "error"]
    return loaded, meta


def _applied(path: Path, packs: list[str]) -> tuple[str, dict[str, Any]]:
    text = read_text(path)
    meta = {
        "name": header_value(text, "NAME") or path.stem,
        "pack": header_value(text, "PACK"),
        "category": header_value(text, "CATEGORY"),
        "tempo": float(header_value(text, "TEMPO_HINT") or 120),
        "root": header_value(text, "ROOT_HINT"),
    }
    base = '[PROJECT]\nADX_VERSION=2\n\n[MIXER]\nINSERT 1 name="Master"\n'
    effect = False
    if path.suffix == ".adxrack":
        project, _ = engine_bridge.Project.loads(base)
        result = dict(project.native.insert_rack(str(path), packs))
        meta["diagnostics"] = [dict(d) for d in result["diagnostics"]]
        channel_ids = list(result["channels"])
        if not channel_ids:
            # A chain rack: a reference VA plays into it.
            effect = True
    else:
        kind = dict(engine_bridge.read_preset(str(path)))["kind"]
        effect = kind == "effect"
        source = (
            "[CHANNEL Source]\nINSTRUMENT=va\nOUTPUT=insert.1\n\n"
            if effect
            else ("[CHANNEL Audition]\nINSTRUMENT=additive\nOUTPUT=insert.1\n\n")
        )
        project, _ = engine_bridge.Project.loads(source + base)
        if effect:
            _, diagnostics = project.apply_effect_preset(1, 0, str(path))
        else:
            diagnostics = project.apply_preset(1, str(path))
        meta["diagnostics"] = [dict(d) for d in diagnostics]
    current = project.dumps()
    if path.suffix == ".adxrack" and effect:
        insert = int(result["insert"])
        current = current.replace(
            "[MIXER]", f"[CHANNEL Source]\nINSTRUMENT=va\nOUTPUT=insert.{insert}\n\n[MIXER]", 1
        )
    if not meta["category"]:
        meta["category"] = "Chain/Bus" if effect else "Lead"
    meta["effect"] = effect
    return current, meta


def read_wav_float(path: Path) -> tuple[np.ndarray, int]:
    """The engine's 32-bit float WAV as (frames, channels)."""
    blob = path.read_bytes()
    offset = 12
    channels, rate, data = 2, 48000, b""
    while offset + 8 <= len(blob):
        chunk, size = struct.unpack_from("<4sI", blob, offset)
        body = blob[offset + 8 : offset + 8 + size]
        if chunk == b"fmt ":
            channels, rate = struct.unpack_from("<HI", body, 2)
        elif chunk == b"data":
            data = body
        offset += 8 + size + (size & 1)
    samples = np.frombuffer(data, dtype="<f4").reshape(-1, channels)
    return samples.astype(np.float32), int(rate)


def true_peak_db(audio: np.ndarray) -> float:
    """4x oversampled peak (BS.1770-4 annex 2's method, by FFT interpolation)."""
    if audio.size == 0:
        return -200.0
    n = audio.shape[0]
    spectrum = np.fft.rfft(audio, axis=0)
    up = np.fft.irfft(spectrum, n * 4, axis=0) * 4
    peak = float(np.max(np.abs(up)))
    return 20 * np.log10(peak) if peak > 0 else -200.0


def measure(audio: np.ndarray, rate: int) -> dict[str, float]:
    loudness = engine_bridge.measure_loudness(np.ascontiguousarray(audio, dtype=np.float32), rate)
    return {"lufs": loudness["integrated"], "true_peak_db": true_peak_db(audio)}


def spectrum_png(audio: np.ndarray, rate: int, path: Path) -> None:
    """Average magnitude spectrum, 20 Hz - 20 kHz on a log axis, -100..0 dB."""
    from PIL import Image, ImageDraw

    mono = audio.mean(axis=1) if audio.ndim == 2 else audio
    size = 4096
    window = np.hanning(size)
    frames = [mono[i : i + size] * window for i in range(0, max(1, len(mono) - size), size // 2)]
    frames = [f for f in frames if len(f) == size] or [np.zeros(size)]
    power = np.mean([np.abs(np.fft.rfft(f)) ** 2 for f in frames], axis=0)
    db = 10 * np.log10(power / (np.sum(window) ** 2 / 4) + 1e-20)
    freqs = np.fft.rfftfreq(size, 1 / rate)
    width, height = 640, 240
    image = Image.new("RGB", (width, height), (24, 26, 30))
    draw = ImageDraw.Draw(image)
    for hz in (100, 1000, 10000):
        x = width * np.log10(hz / 20) / np.log10(1000)
        draw.line([(x, 0), (x, height)], fill=(60, 64, 70))
    points = []
    for f, level in zip(freqs[1:], db[1:], strict=True):
        if 20 <= f <= 20000:
            x = width * np.log10(f / 20) / np.log10(1000)
            y = height * (min(0.0, max(-100.0, float(level))) / -100.0)
            points.append((float(x), float(y)))
    if len(points) > 1:
        draw.line(points, fill=(120, 200, 255), width=2)
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)


def default_out() -> Path:
    local = os.environ.get("LOCALAPPDATA")
    return (Path(local) if local else Path.home() / ".cache") / "adX" / "audition"


def safe(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9._ -]+", "_", name).strip() or "untitled"


def audition(path: Path, out: Path | None = None, packs: list[str] | None = None) -> dict[str, Any]:
    """Render, measure and cache one preset or rack. The JSON's fields, returned."""
    out = out or default_out()
    packs = packs or [str(REPO / "engine" / "preset" / "packs")]
    project, meta = build_project(path, packs)
    folder = out / safe(meta["pack"] or "user")
    stem = safe(meta["name"])
    wav = folder / f"{stem}.wav"
    folder.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        rendered = Path(scratch) / "render.wav"
        stats = engine_bridge.render_to_file(project, str(rendered), tail_seconds=2.0)
        audio, rate = read_wav_float(rendered)
        wav.write_bytes(rendered.read_bytes())
    levels = measure(audio, rate)
    target = taxonomy.category(meta["category"]).loudness_target
    result = {
        **{k: v for k, v in meta.items() if k != "diagnostics"},
        **levels,
        "target_lufs": target,
        "within_window": abs(levels["lufs"] - target) <= taxonomy.LOUDNESS_WINDOW,
        "frames": int(audio.shape[0]),
        "rate": rate,
        "hash": stats.get("hash", ""),
        "wav": str(wav),
        "png": str(folder / f"{stem}.png"),
        "source": str(path),
        "diagnostics": meta["diagnostics"],
    }
    spectrum_png(audio, rate, folder / f"{stem}.png")
    (folder / f"{stem}.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--packs", nargs="*", default=None)
    args = parser.parse_args(argv)
    ok = True
    for file in args.files:
        result = audition(file, args.out, args.packs)
        mark = "ok" if result["within_window"] else "OUT OF WINDOW"
        print(
            f"{result['name']}: {result['lufs']:.1f} LUFS (target {result['target_lufs']:.0f}), "
            f"true peak {result['true_peak_db']:.1f} dBTP - {mark}"
        )
        ok = ok and result["within_window"]
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
