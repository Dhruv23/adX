"""Measure the key detector on the GiantSteps Key corpus (tools/fetch_key_corpus.py).

Prints exact, exact-or-relative, and the MIREX weighted score (exact 1, fifth 0.5,
relative 0.3, parallel 0.2), the measure published results on this corpus use. Writes
every track's estimate to a JSON report when asked. Phase 10's numbers are in
phase_10.md §11.

    py -3.12 tools/measure_key_corpus.py [--report out.json]
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from fetch_key_corpus import default_cache

from adx import engine_bridge

NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
FLATS = {"Db": "C#", "Eb": "D#", "Gb": "F#", "Ab": "G#", "Bb": "A#", "Cb": "B", "Fb": "E"}


def parse(label: str) -> tuple[int, str] | None:
    """(pitch class, "major"/"minor") from "Ab minor"; None for anything else."""
    parts = label.strip().split()
    if len(parts) != 2 or parts[1] not in ("major", "minor"):
        return None
    tonic = FLATS.get(parts[0], parts[0])
    return (NAMES.index(tonic), parts[1]) if tonic in NAMES else None


def weight(truth: tuple[int, str], guess: tuple[int, str]) -> float:
    (t, tm), (g, gm) = truth, guess
    if truth == guess:
        return 1.0
    if tm == gm and (g - t) % 12 in (5, 7):
        return 0.5
    if tm == "major" and gm == "minor" and (g - t) % 12 == 9:
        return 0.3
    if tm == "minor" and gm == "major" and (g - t) % 12 == 3:
        return 0.3
    if t == g:
        return 0.2
    return 0.0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", type=pathlib.Path, default=default_cache())
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args(argv)
    tracks = []
    for key_file in sorted((args.cache / "key").glob("*.key")):
        audio = args.cache / "audio" / f"{key_file.stem}.mp3"
        truth = parse(key_file.read_text(encoding="utf-8"))
        if audio.exists() and truth is not None:
            tracks.append((key_file.stem, audio, truth))
    if not tracks:
        print("measure_key_corpus: no tracks; run tools/fetch_key_corpus.py first", file=sys.stderr)
        return 1

    def one(track: tuple[str, pathlib.Path, tuple[int, str]]) -> dict[str, object]:
        name, audio, truth = track
        result = engine_bridge.analyze("key", {"path": str(audio)})
        guess = parse(str(result["key"]["name"])) if result["ok"] else None
        return {
            "track": name,
            "truth": f"{NAMES[truth[0]]} {truth[1]}",
            "estimate": result["key"]["name"] if result["ok"] else None,
            "confidence": result["key"]["confidence"] if result["ok"] else None,
            "weight": weight(truth, guess) if guess else 0.0,
        }

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        rows = list(pool.map(one, tracks))
    n = len(rows)
    exact = sum(1 for r in rows if r["weight"] == 1.0)
    relative = sum(1 for r in rows if r["weight"] in (1.0, 0.3))
    score = sum(float(r["weight"]) for r in rows)  # type: ignore[arg-type]
    fifth = sum(1 for r in rows if r["weight"] == 0.5)
    parallel = sum(1 for r in rows if r["weight"] == 0.2)
    print(
        f"measure_key_corpus: {n} tracks - exact {exact / n:.1%}, with relatives "
        f"{relative / n:.1%}, fifths {fifth / n:.1%}, parallel {parallel / n:.1%}, "
        f"MIREX weighted {score / n:.1%}"
    )
    if args.report:
        args.report.write_text(json.dumps(rows, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
