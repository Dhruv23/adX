"""Fetch the Qt C++ SDK that the scene-graph plugin builds against (phase_5.md 4.3).

PySide6 ships Qt's runtime DLLs but none of its headers or import libraries, and the
piano roll's QQuickItem is C++ (it must not take the GIL on Qt's render thread). So
the plugin is compiled against a Qt SDK of *exactly* PySide6's version and loaded into
PySide6's Qt at run time. This fetches qtbase and qtdeclarative for that version from
download.qt.io into ``.qt/<version>/`` (gitignored), where CMake looks by default.

    python tools/fetch_qt.py              # the version pyproject.toml pins PySide6 to
    python tools/fetch_qt.py --version 6.11.2

aqtinstall is the usual tool for this, but 3.3.0 cannot read the repository layout
Qt adopted in 6.11 (``qt6_6112/qt6_6112_msvc2022_64/``), so this reads it directly.
Archives are kept in ``.qt/dl/`` so CI can cache 200 MB rather than the extracted 2 GB.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
QT_ROOT = REPO_ROOT / ".qt"
BASE_URL = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop"
MODULES = ("qtbase", "qtdeclarative")
#: Must equal the PySide6 version pyproject.toml pins: a plugin built against a newer
#: Qt than the one that loads it is refused, and the bindings test checks the match.
DEFAULT_VERSION = "6.11.2"


def _package_url(version: str) -> str:
    tag = version.replace(".", "")
    return f"{BASE_URL}/qt6_{tag}/qt6_{tag}_msvc2022_64/qt.qt6.{tag}.win64_msvc2022_64/"


def _archive_names(version: str) -> list[str]:
    url = _package_url(version)
    with urllib.request.urlopen(url, timeout=60) as response:
        listing = response.read().decode("utf-8", "replace")
    names: list[str] = []
    for module in MODULES:
        pattern = (
            rf'href="({re.escape(version)}-0-\d+{module}-Windows-[^"]*MSVC2022[^"]*X86_64\.7z)"'
        )
        found = re.findall(pattern, listing)
        if not found:
            raise SystemExit(f"fetch_qt: no {module} archive for Qt {version} at {url}")
        names.append(found[0])
    return names


def _download(url: str, target: Path) -> None:
    if target.exists():
        return
    print(f"fetch_qt: downloading {target.name}")
    partial = target.with_suffix(".part")
    with urllib.request.urlopen(url, timeout=600) as response, partial.open("wb") as out:
        shutil.copyfileobj(response, out)
    partial.rename(target)


def _extract(archive: Path, destination: Path) -> None:
    seven = shutil.which("7z")
    if seven:
        subprocess.run(
            [seven, "x", "-y", f"-o{destination}", str(archive)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        return
    try:
        import py7zr  # type: ignore[import-not-found,unused-ignore]
    except ImportError as error:
        raise SystemExit("fetch_qt: needs 7z on PATH or `pip install py7zr`") from error
    with py7zr.SevenZipFile(archive) as handle:
        handle.extractall(destination)


def fetch(version: str) -> Path:
    """Fetch and extract Qt `version`; return its prefix. A no-op when already there."""
    prefix = QT_ROOT / version
    marker = prefix / ".complete"
    if marker.exists():
        return prefix
    downloads = QT_ROOT / "dl"
    downloads.mkdir(parents=True, exist_ok=True)
    for name in _archive_names(version):
        archive = downloads / name
        _download(_package_url(version) + name, archive)
        print(f"fetch_qt: extracting {name}")
        _extract(archive, prefix)
    marker.write_text(version, encoding="utf-8")
    return prefix


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="fetch_qt", description=__doc__.splitlines()[0])
    parser.add_argument("--version", default=DEFAULT_VERSION)
    args = parser.parse_args(argv)
    prefix = fetch(str(args.version))
    print(f"fetch_qt: Qt {args.version} at {prefix}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
