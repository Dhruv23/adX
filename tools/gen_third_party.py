"""Write installer/THIRD-PARTY.md from the dependency manifest (phase_10.md 4.8).

    py -3.12 tools/gen_third_party.py [--check]

Every FetchContent_Declare in cmake/AdxDependencies.cmake carries an ``# adx-license:``
line (name | SPDX | what it is for) right above it; one without fails this, which CMake
runs at configure time, so a dependency cannot arrive unlicensed. The Python runtime and
the packages the installer bundles come from pyproject.toml's dependencies, with the
licences recorded below. ``--check`` exits 1 when the committed page is stale.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "cmake" / "AdxDependencies.cmake"
PAGE = ROOT / "installer" / "THIRD-PARTY.md"
#: What the installer bundles on the Python side (pyproject.toml's dependencies and the
#: embedded interpreter), with their licences.
PYTHON_SIDE = (
    ("Python 3.12 (embeddable)", "3.12", "PSF-2.0", "the interpreter the app runs on"),
    (
        "PySide6 (Qt for Python)",
        "6.11.2",
        "LGPL-3.0-only",
        "the user interface; shipped as separate DLLs",
    ),
    ("NumPy", ">=1.26", "BSD-3-Clause", "arrays across the engine boundary"),
)


def manifest() -> list[tuple[str, str, str, str, str]]:
    """(key, name, version, SPDX, use) per FetchContent entry; exits on a missing licence."""
    text = MANIFEST.read_text(encoding="utf-8")
    lines = text.splitlines()
    out, missing = [], []
    for number, line in enumerate(lines):
        declared = re.match(r"\s*FetchContent_Declare\((\w+)", line)
        if not declared:
            continue
        key = declared.group(1)
        above = lines[number - 1].strip() if number else ""
        licence = re.match(r"# adx-license: (\w+) \| ([^|]+) \| ([^|]+) \| (.+)", above)
        if not licence or licence.group(1) != key:
            missing.append(key)
            continue
        body = "\n".join(lines[number : number + 8])
        tag = re.search(r"GIT_TAG\s+(\S+)", body)
        url = (
            re.search(r"/v?(\d+\.\d+(?:\.\d+)?)", re.search(r"URL\s+(\S+)", body).group(1))
            if not tag
            else None
        )
        version = tag.group(1) if tag else (url.group(1) if url else "")
        out.append(
            (
                key,
                licence.group(2).strip(),
                version,
                licence.group(3).strip(),
                licence.group(4).strip(),
            )
        )
    if missing:
        raise SystemExit(
            f"gen_third_party: no '# adx-license:' line above {', '.join(missing)} in {MANIFEST.name}"
        )
    return out


def render() -> str:
    rows = manifest()
    lines = [
        "# Third-party software in adX",
        "",
        "Generated from `cmake/AdxDependencies.cmake` by `tools/gen_third_party.py`; edit the",
        "`# adx-license:` lines there, not this file. Each project's own licence text is in",
        "its source, under the build's `_deps/` folder.",
        "",
        "**Rubber Band** is GPL-2.0-or-later: an adX binary that links it is distributed under",
        "the GPL (FINAL_PLAN §8). **shine** is LGPL and linked statically: on request, the object",
        "files needed to relink adX against a modified shine are provided.",
        "",
        "## Engine (C++)",
        "",
        "| Project | Version | Licence | Used for |",
        "|---|---|---|---|",
    ]
    lines += [f"| {name} | {version} | {spdx} | {use} |" for _key, name, version, spdx, use in rows]
    lines += [
        "",
        "## Runtime (Python)",
        "",
        "| Project | Version | Licence | Used for |",
        "|---|---|---|---|",
    ]
    lines += [
        f"| {name} | {version} | {spdx} | {use} |" for name, version, spdx, use in PYTHON_SIDE
    ]
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = render()
    if args.check:
        if not PAGE.exists() or PAGE.read_text(encoding="utf-8") != text:
            print(
                "installer/THIRD-PARTY.md is stale: py -3.12 tools/gen_third_party.py",
                file=sys.stderr,
            )
            return 1
        return 0
    PAGE.parent.mkdir(parents=True, exist_ok=True)
    PAGE.write_text(text, encoding="utf-8", newline="\n")
    print(PAGE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
