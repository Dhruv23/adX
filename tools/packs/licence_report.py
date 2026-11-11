"""Every sound asset's licence, and the gate that keeps unshippable ones out (phase_12.md 4.8).

    python tools/packs/licence_report.py            the section, printed
    python tools/packs/licence_report.py --gate DIR fail if DIR holds a file it may not

The section lists each fetchable library (fetch_libraries.LIBRARIES) and each pack's
`pack.adxpack` assets with their licence and credit; tools/gen_third_party.py appends it
to installer/THIRD-PARTY.md.

The gate walks a staged installer folder. It fails on any file that comes from an asset
marked `redistributable=no` - matched by path prefix and, for fetched libraries, by
content hash against the library's manifest - and on any copy of the Windows GM bank
(`gm.dls`), which is Microsoft's and is only ever read where Windows put it.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PACKS = REPO / "engine" / "preset" / "packs"
sys.path.insert(0, str(Path(__file__).resolve().parent))

import fetch_libraries  # noqa: E402

AUDIO = {".wav", ".flac", ".ogg", ".mp3", ".aif", ".aiff", ".sf2", ".sf3", ".dls"}


@dataclass
class Asset:
    prefix: str
    licence: str
    redistributable: bool
    credit: str = ""


@dataclass
class Pack:
    id: str
    name: str = ""
    version: str = ""
    requires: str = ""
    libraries: list[str] = field(default_factory=list)
    licence: str = ""
    assets: list[Asset] = field(default_factory=list)
    source: Path | None = None


_PAIR = re.compile(r'(\w+)=("(?:[^"\\]|\\.)*"|\S+)')


def _unquote(value: str) -> str:
    return value[1:-1].replace('\\"', '"') if value.startswith('"') else value


def parse_pack(text: str, source: Path | None = None) -> Pack:
    """A pack.adxpack (docs/adx-format-v2.md 13.3)."""
    pack = Pack(id="")
    section = ""
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip() if not raw.lstrip().startswith("ASSET") else raw.strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().upper()
            continue
        if section == "PACK" and "=" in line:
            key, value = (part.strip() for part in line.split("=", 1))
            value = _unquote(value)
            if key == "ID":
                pack.id = value
            elif key == "NAME":
                pack.name = value
            elif key == "VERSION":
                pack.version = value
            elif key == "REQUIRES":
                pack.requires = value
            elif key == "LIBRARIES":
                pack.libraries = [v.strip() for v in value.split(",") if v.strip()]
            elif key == "LICENCE":
                pack.licence = value
        elif section == "ASSETS" and line.startswith("ASSET"):
            rest = line[len("ASSET") :].strip()
            match = re.match(r'"((?:[^"\\]|\\.)*)"', rest)
            if match is None:
                continue
            fields = {k: _unquote(v) for k, v in _PAIR.findall(rest[match.end() :])}
            pack.assets.append(
                Asset(
                    prefix=match.group(1),
                    licence=fields.get("licence", pack.licence),
                    redistributable=fields.get("redistributable", "yes").lower()
                    in ("yes", "true", "1"),
                    credit=fields.get("credit", ""),
                )
            )
    pack.source = source
    return pack


def packs(root: Path = PACKS) -> list[Pack]:
    found = []
    for path in sorted(root.glob("*/pack.adxpack")):
        found.append(parse_pack(path.read_text(encoding="utf-8"), path))
    return found


def section(root: Path = PACKS) -> str:
    lines = [
        "## Sound libraries",
        "",
        "Fetched by `tools/packs/fetch_libraries.py` into the user's library store at a "
        "pinned commit, never bundled in the installer; presets name their files as "
        "`lib://` paths.",
        "",
        "| Library | Commit | Licence | Credit |",
        "|---|---|---|---|",
    ]
    for lib in fetch_libraries.LIBRARIES.values():
        lines.append(f"| {lib.title} | {lib.commit[:12]} | {lib.licence} | {lib.credit} |")
    pack_list = packs(root)
    assets = [(p, a) for p in pack_list for a in p.assets]
    if assets:
        lines += ["", "| Pack | Asset | Licence | Shipped | Credit |", "|---|---|---|---|---|"]
        for pack, asset in assets:
            shipped = "yes" if asset.redistributable else "no (fetched only)"
            lines.append(
                f"| {pack.id} | `{asset.prefix}` | {asset.licence} | {shipped} | {asset.credit} |"
            )
    lines.append("")
    return "\n".join(lines)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def forbidden_hashes() -> dict[str, str]:
    """Content hashes of every file of every library that may not be shipped."""
    out: dict[str, str] = {}
    for lib in fetch_libraries.LIBRARIES.values():
        manifest = fetch_libraries.manifest_path(lib)
        if lib.redistributable or not manifest.exists():
            continue
        for name, digest in fetch_libraries.read_manifest(manifest).items():
            out[digest] = f"lib://{lib.id}/{name}"
    return out


def gate(stage: Path, root: Path = PACKS) -> list[str]:
    """Why each file under `stage` may not be shipped; empty when all may."""
    problems: list[str] = []
    prefixes = [a for p in packs(root) for a in p.assets if not a.redistributable]
    hashes = forbidden_hashes()
    for path in sorted(stage.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(stage).as_posix()
        if path.name.lower() == "gm.dls":
            problems.append(f"{relative}: the Windows GM bank is Microsoft's; never ship it")
            continue
        for asset in prefixes:
            bare = asset.prefix.removeprefix("lib://")
            if bare and bare.rstrip("/") in relative:
                problems.append(
                    f"{relative}: under {asset.prefix} ({asset.licence}), not shippable"
                )
                break
        else:
            if path.suffix.lower() in AUDIO and hashes:
                origin = hashes.get(_sha256(path))
                if origin is not None:
                    problems.append(f"{relative}: a copy of {origin}, not shippable")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gate", type=Path, metavar="DIR")
    args = parser.parse_args(argv)
    if args.gate is not None:
        problems = gate(args.gate)
        for problem in problems:
            print(problem, file=sys.stderr)
        print(f"licence gate: {len(problems)} problem(s) in {args.gate}")
        return 1 if problems else 0
    print(section())
    return 0


if __name__ == "__main__":
    sys.exit(main())
