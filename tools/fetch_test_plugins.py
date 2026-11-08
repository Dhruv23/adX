"""Fetch the pinned open-source test plugins (phase_10.5.md 4.6).

    py -3.12 tools/fetch_test_plugins.py [--list]

Reads tools/test_plugins.toml, downloads each archive into the cache
(%LOCALAPPDATA%/adx-test-plugins, or ADX_TEST_PLUGINS), checks its SHA-256 and refuses
a mismatch, then unpacks the listed bundles into <cache>/plugins/<name>/. Already
fetched and verified archives are not downloaded again. Prints the plugins folder, which
the real-plugin scenarios scan.

The plugins are test fixtures: never shipped, never linked, never committed.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import sys
import tomllib
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINS = ROOT / "tools" / "test_plugins.toml"


def cache() -> Path:
    base = os.environ.get("ADX_TEST_PLUGINS") or str(
        Path(os.environ.get("LOCALAPPDATA", Path.home())) / "adx-test-plugins"
    )
    return Path(base)


def pins() -> list[dict[str, object]]:
    with PINS.open("rb") as handle:
        return list(tomllib.load(handle)["plugin"])


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(pin: dict[str, object], root: Path) -> Path:
    url, want = str(pin["url"]), str(pin["sha256"])
    archive = root / "downloads" / url.rsplit("/", 1)[1]
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists() or _sha256(archive) != want:
        partial = archive.with_suffix(archive.suffix + ".part")
        try:
            with urllib.request.urlopen(url, timeout=120) as response, partial.open("wb") as out:
                shutil.copyfileobj(response, out)
        except urllib.error.HTTPError as error:
            partial.unlink(missing_ok=True)
            # Airwindows publishes one rolling build and deletes the old one, so its pin
            # goes stale whenever upstream releases; re-pin it in tools/test_plugins.toml.
            raise SystemExit(
                f"fetch_test_plugins: {pin['name']}: {url} answered {error.code}; "
                "if upstream replaced the build, re-pin it in tools/test_plugins.toml"
            ) from error
        got = _sha256(partial)
        if got != want:
            partial.unlink()
            raise SystemExit(
                f"fetch_test_plugins: {pin['name']}: SHA-256 {got}, pinned {want}; refusing it"
            )
        partial.replace(archive)
    target = root / "plugins" / str(pin["name"])
    marker = target / ".sha256"
    if marker.exists() and marker.read_text(encoding="utf-8") == want:
        return target
    shutil.rmtree(target, ignore_errors=True)
    target.mkdir(parents=True)
    bundles = [str(b) for b in pin["bundles"]]  # type: ignore[attr-defined]
    with zipfile.ZipFile(archive) as zipped:
        for member in zipped.namelist():
            top = member.split("/", 1)[0]
            if top in bundles and ".." not in Path(member).parts:
                zipped.extract(member, target)
    missing = [b for b in bundles if not (target / b).exists()]
    if missing:
        raise SystemExit(
            f"fetch_test_plugins: {pin['name']}: the archive has no {', '.join(missing)}"
        )
    marker.write_text(want, encoding="utf-8")
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", action="store_true", help="print the pins and exit")
    args = parser.parse_args()
    if args.list:
        for pin in pins():
            bundles = ", ".join(pin["bundles"])  # type: ignore[arg-type]
            print(f"{pin['name']} {pin['version']} ({pin['license']}, {pin['role']}): {bundles}")
        return 0
    root = cache()
    for pin in pins():
        folder = fetch(pin, root)
        print(f"{pin['name']} {pin['version']}: {folder}")
    print(root / "plugins")
    return 0


if __name__ == "__main__":
    sys.exit(main())
