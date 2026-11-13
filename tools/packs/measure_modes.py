"""Measure mode ratios from sampled instruments, for physical's mode tables (phase_12.md
4.4.3, mode_tables_match_references).

    python tools/packs/measure_modes.py [--table NAME]

Each table with a reference in the library store is measured from that library's
samples: for every region of the reference SFZ, the fundamental is the strongest peak
within a semitone of the region's root key, and each further mode the strongest peak
within 15 % of the textbook ratio it is seeded with (not the table's, so the
measurement does not move with the table). The reported ratio is the median over regions.
Tables whose reference is analytic (a free bar) or not in any fetched library are
listed with their source and not measured.
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "packs"))
TABLES = REPO / "engine" / "instruments" / "physical" / "ModeTables.h"


@dataclass(frozen=True)
class Reference:
    library: str
    sfz: str
    #: The textbook ratios each mode is looked for near, fixed here so what is measured
    #: does not depend on the table being checked.
    seeds: tuple[float, ...]
    #: Only regions whose sample path contains this (a mallet, a dynamic).
    match: str = ""


#: Table name -> its reference in the library store.
REFERENCES: dict[str, Reference] = {
    "marimba": Reference(
        "vcsl",
        "Idiophones/Struck Idiophones/Marimba.sfz",
        (1.0, 4.0, 10.0),
        "med",
    ),
    "xylophone": Reference(
        "vcsl",
        "Idiophones/Struck Idiophones/Xylophone - Medium Mallets.sfz",
        (1.0, 3.0, 6.0),
    ),
    "vibraphone": Reference(
        "vcsl",
        "Idiophones/Struck Idiophones/Vibraphone - Hard Mallets.sfz",
        (1.0, 4.0, 10.0),
    ),
    "glockenspiel": Reference(
        "vcsl",
        "Idiophones/Struck Idiophones/Glockenspiel.sfz",
        (1.0, 2.76, 5.40),
    ),
    "kalimba": Reference(
        "vcsl",
        "Idiophones/Plucked Idiophones/Kalimba, Kenya.sfz",
        (1.0, 5.4, 13.5),
    ),
    "timpani": Reference(
        "vcsl",
        "Membranophones/Struck Membranophones/Timpani 1 - Hit.sfz",
        (1.0, 1.5, 1.98, 2.44, 2.94),
    ),
    # Not measurable from these libraries: a tubular bell's sounding pitch is not its
    # first mode, so a region's root key does not find mode 1; VCSL's wine glasses are
    # bowed, which drives a harmonic series rather than the bending modes.
}


def tables() -> dict[str, list[float]]:
    """name -> ratios, read from ModeTables.h."""
    text = TABLES.read_text(encoding="utf-8")
    out: dict[str, list[float]] = {}
    for name, count, ratios in re.findall(
        r'\{"([^"]+)",\s*(\d+),\s*\{([^}]*)\}', text, flags=re.MULTILINE
    ):
        values = [float(v.strip().rstrip("F")) for v in ratios.split(",") if v.strip()]
        out[name] = values[: int(count)]
    return out


def library_folder(library: str) -> Path | None:
    from fetch_libraries import LIBRARIES, store_root

    lib = LIBRARIES.get(library)
    if lib is None:
        return None
    folder = store_root() / lib.folder
    return folder if folder.is_dir() else None


def regions(sfz: Path, match: str) -> list[tuple[Path, int]]:
    """(sample, root key) for each region of an SFZ: sample= and pitch_keycenter=."""
    base = sfz.parent
    found: list[tuple[Path, int]] = []
    sample: str | None = None
    key: int | None = None
    for raw in sfz.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.split("//", 1)[0].strip()
        if line.startswith("<region>"):
            if sample is not None and key is not None:
                found.append((base / sample.replace("\\", "/"), key))
            sample, key = None, None
        for k, v in re.findall(r"(\w+)=(.+?)(?=\s+\w+=|$)", line):
            if k == "sample":
                sample = v.strip()
            elif k == "pitch_keycenter":
                key = int(v)
    if sample is not None and key is not None:
        found.append((base / sample.replace("\\", "/"), key))
    return [(p, k) for p, k in found if match in p.name and p.is_file()]


def read_mono(path: Path) -> tuple[np.ndarray, int]:
    from wavio import read_any

    audio, rate = read_any(path)
    return audio.mean(axis=1) if audio.ndim == 2 else audio, rate


def spectrum(x: np.ndarray, rate: int) -> tuple[np.ndarray, np.ndarray]:
    onset = int(np.argmax(np.abs(x) > 0.5 * np.max(np.abs(x))))
    seg = x[onset + int(0.01 * rate) : onset + int(0.6 * rate)]
    n = 1 << int(np.ceil(np.log2(len(seg) * 8)))
    mag = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n))
    return np.fft.rfftfreq(n, 1.0 / rate), mag


def peak(freqs: np.ndarray, mag: np.ndarray, lo: float, hi: float) -> float | None:
    band = (freqs >= lo) & (freqs <= hi)
    if not band.any():
        return None
    i = int(np.argmax(np.where(band, mag, 0.0)))
    if i <= 0 or i >= len(mag) - 1:
        return float(freqs[i])
    a, b, c = np.log(mag[i - 1 : i + 2] + 1e-30)
    offset = 0.5 * (a - c) / (a - (2 * b) + c)
    return float(freqs[i] + (offset * (freqs[1] - freqs[0])))


def measure(name: str) -> list[float] | None:
    """The median measured ratio of each mode, or None without the reference."""
    ref = REFERENCES.get(name)
    folder = library_folder(ref.library) if ref else None
    if ref is None or folder is None:
        return None
    per_mode: list[list[float]] = [[] for _ in ref.seeds]
    for sample, key in regions(folder / ref.sfz, ref.match):
        x, rate = read_mono(sample)
        freqs, mag = spectrum(x, rate)
        nominal = 440.0 * 2.0 ** ((key - 69) / 12.0)
        f0 = peak(freqs, mag, nominal / 2 ** (1 / 12), nominal * 2 ** (1 / 12))
        if f0 is None:
            continue
        for m, r in enumerate(ref.seeds):
            hi = f0 * r * 1.15
            if hi > rate * 0.45:
                continue
            f = peak(freqs, mag, f0 * r * 0.85, hi)
            if f is not None:
                per_mode[m].append(f / f0)
    if not per_mode[0]:
        return None
    return [statistics.median(v) if v else float("nan") for v in per_mode]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--table")
    args = parser.parse_args(argv)
    for name, ratios in tables().items():
        if args.table and name != args.table:
            continue
        got = measure(name)
        if got is None:
            print(f"{name:14} table {ratios}  (no reference in the store)")
            continue
        shown = ", ".join(f"{g:.3f}" for g in got)
        print(f"{name:14} table {ratios}  measured [{shown}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
