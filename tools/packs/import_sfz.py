"""SFZ files as sampler presets (phase_12.md 4.7.2).

    python tools/packs/import_sfz.py FILE.sfz... [--pack NAME] [--out DIR] [--category C]

A file inside the library store keeps its samples as `lib://<library>/...` paths, so the
preset works on any machine with the library fetched; any other file's samples are made
relative to the preset. Each preset is written beside an `.import.txt` report listing
every opcode that was not applied, with its line and column. Exit 1 when an opcode was
unknown or a sample is missing.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "app"))

from adx import engine_bridge  # noqa: E402


def library_prefix(sfz: Path) -> str:
    """`lib://<id>/<folder>/` when `sfz` is inside the store; empty otherwise."""
    native = engine_bridge._native
    store = Path(native.library_store()).resolve()
    try:
        relative = sfz.resolve().parent.relative_to(store)
    except ValueError:
        return ""
    parts = relative.parts
    if not parts:
        return ""
    library = parts[0].split("@", 1)[0]
    rest = "/".join(parts[1:])
    return f"lib://{library}/" + (f"{rest}/" if rest else "")


def import_one(
    sfz: Path, out: Path, pack: str, category: str, name: str | None = None
) -> dict[str, Any]:
    native = engine_bridge._native
    prefix = library_prefix(sfz)
    if not prefix:
        # Relative to where the preset will be written.
        prefix = Path(os.path.relpath(sfz.parent, out)).as_posix() + "/"
    result = dict(native.import_sfz(str(sfz), name or sfz.stem, pack, prefix))
    text = str(result["preset"])
    if category:
        text = text.replace("TYPE=sampler\n", f"TYPE=sampler\nCATEGORY={category}\n", 1)
    out.mkdir(parents=True, exist_ok=True)
    preset = out / f"{sfz.stem}.adxpreset"
    preset.write_text(text, encoding="utf-8", newline="\n")
    report = out / f"{sfz.stem}.import.txt"
    report.write_text(
        f"{sfz}\n{result['regions']} regions, {result['missing_samples']} samples missing, "
        f"{result['unknown']} unknown, {result['ignored']} ignored, "
        f"{result['approximated']} approximated\n\n{result['report']}",
        encoding="utf-8",
        newline="\n",
    )
    result["path"] = str(preset)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--pack", default="")
    parser.add_argument("--category", default="")
    parser.add_argument("--out", type=Path, default=Path.cwd())
    args = parser.parse_args(argv)
    clean = True
    for sfz in args.files:
        result = import_one(sfz, args.out, args.pack, args.category)
        print(
            f"{sfz.name}: {result['regions']} regions -> {result['path']} "
            f"({result['unknown']} unknown, {result['missing_samples']} missing)"
        )
        clean = clean and result["unknown"] == 0 and result["missing_samples"] == 0
    return 0 if clean else 1


if __name__ == "__main__":
    sys.exit(main())
