"""Gathers a pack's audition renders into a folder for the human gate, one subfolder per
category, ready for tools/packs/listen_gui.py.

    python tools/packs/listening_pack.py core [--out build/phase13-listening]

Each sound is auditioned afresh (its category's phrase, tools/packs/audition.py), so the
folder holds what the current files sound like. LISTEN.md lists each with its loudness.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(REPO / "app"))

import audition  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("pack")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)
    pack = REPO / "engine" / "preset" / "packs" / args.pack
    out: Path = (args.out or REPO / "build" / f"{args.pack}-listening").resolve()
    if out.exists():
        shutil.rmtree(out)
    rows: list[tuple[str, str, float]] = []
    sounds = sorted([*pack.rglob("*.adxpreset"), *pack.rglob("*.adxrack")])
    for sound in sounds:
        if sound.parent.name == "chains":
            continue
        result = audition.audition(sound, out=REPO / "build" / f"{args.pack}-audition")
        category = str(result["category"]).replace("/", " - ")
        folder = out / category
        folder.mkdir(parents=True, exist_ok=True)
        shutil.copy2(result["wav"], folder / f"{result['name']}.wav")
        rows.append((category, str(result["name"]), float(result["lufs"])))
    lines = [
        f"# {args.pack}: listening pass",
        "",
        "One folder per category. Open them with `python tools/packs/listen_gui.py "
        f"{out.relative_to(REPO).as_posix()}`;",
        "notes save to listening_notes.json as you type. The gate wants a verdict per",
        "category, in your words, and the 808/909 kits A/B'd against references you choose.",
        "",
        "| Category | Sound | LUFS |",
        "|---|---|---|",
    ]
    lines += [f"| {c} | {n} | {lufs:.1f} |" for c, n, lufs in sorted(rows)]
    (out / "LISTEN.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(rows)} sounds in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
