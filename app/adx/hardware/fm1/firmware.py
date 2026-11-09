"""Firmware for the FM-1 (phase_11.5.md 4.11): catalog entries, the catalog itself, and
the image check every package passes before it is offered.

The catalog is a JSON manifest - fetched, then cached outside git - of
``thefirmwarehub.com``'s FM-1 firmwares and M-VAVE's own where M-VAVE publishes the
file. Each open entry carries a download URL, version, licence, the Hub's risk notes and
a SHA-256; a closed one is a link card and is never installable. Nothing is vendored:
packages are downloaded into the cache and checked against the manifest's hash every
time they are read.

    {"version": 1, "source": "<where it came from>", "entries": [
        {"name": "Skiff", "version": "1.2", "source": "thefirmwarehub.com",
         "url": "https://...", "sha256": "...", "license": "MIT",
         "open_source": true, "risk": "...", "page": "https://..."}]}
"""

from __future__ import annotations

import hashlib
import json
import os
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

#: An FM-1 image is a JieLi flash image: no smaller than this, no larger than the flash.
MIN_IMAGE = 64 * 1024
MAX_IMAGE = 8 * 1024 * 1024


@dataclass(frozen=True)
class FirmwarePackage:
    name: str
    version: str
    source: str
    url: str = ""
    sha256: str = ""
    license: str = ""
    open_source: bool = False
    risk: str = ""
    page: str = ""

    @property
    def installable(self) -> bool:
        """Open, with a file and a hash: anything else is a link card."""
        return self.open_source and bool(self.url) and len(self.sha256) == 64


def image_problem(data: bytes, package: FirmwarePackage) -> str:
    """Why ``data`` must not be offered as ``package`` ("" when it may be)."""
    if not package.installable:
        return f"{package.name} is not an open package with a published hash"
    if hashlib.sha256(data).hexdigest() != package.sha256.lower():
        return "the file's SHA-256 does not match the catalog"
    if not MIN_IMAGE <= len(data) <= MAX_IMAGE:
        return f"{len(data)} bytes is not the size of an FM-1 image"
    if data.count(0xFF) == len(data) or data.count(0) == len(data):
        return "the file is blank"
    return ""


def _default_cache() -> Path:
    base = os.environ.get("ADX_FM1_CACHE") or os.path.join(
        os.environ.get("LOCALAPPDATA", str(Path.home())), "adx", "fm1"
    )
    return Path(base)


def _http_get(url: str) -> bytes:
    import urllib.request

    request = urllib.request.Request(url, headers={"User-Agent": "adX firmware catalog"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return bytes(response.read())


@dataclass
class FirmwareCatalog:
    """The manifest, fetched and cached; packages downloaded and hash-checked."""

    cache: Path = field(default_factory=_default_cache)
    fetch: Callable[[str], bytes] = _http_get
    entries: list[FirmwarePackage] = field(default_factory=list)
    #: Where the entries came from: "network", "cache" or "" (none yet).
    origin: str = ""

    @property
    def manifest_path(self) -> Path:
        return self.cache / "catalog.json"

    def load(self, url: str | None = None) -> list[FirmwarePackage]:
        """Fetch ``url`` and cache it; without a url, or when the fetch fails, the cache."""
        text: str | None = None
        if url:
            try:
                text = self.fetch(url).decode("utf-8")
                json.loads(text)  # refuse to cache something that is not JSON
                self.cache.mkdir(parents=True, exist_ok=True)
                self.manifest_path.write_text(text, encoding="utf-8")
                self.origin = "network"
            except (OSError, ValueError):
                text = None
        if text is None:
            if not self.manifest_path.exists():
                self.entries, self.origin = [], ""
                return []
            text = self.manifest_path.read_text(encoding="utf-8")
            self.origin = "cache"
        self.entries = parse_manifest(text)
        return self.entries

    def package_path(self, package: FirmwarePackage) -> Path:
        safe = "".join(c if c.isalnum() or c in "-_." else "_" for c in package.name)
        return self.cache / "packages" / f"{safe}-{package.version}.bin"

    def download(self, package: FirmwarePackage) -> tuple[bytes | None, str]:
        """The package's bytes, checked (from the cache when it is there); (None, why) when
        they may not be offered."""
        if not package.installable:
            return None, f"{package.name} is a link only: {package.page or package.url}"
        path = self.package_path(package)
        data: bytes | None = path.read_bytes() if path.exists() else None
        if data is None or image_problem(data, package):
            try:
                data = self.fetch(package.url)
            except OSError as error:
                return None, f"download failed: {error}"
        problem = image_problem(data, package)
        if problem:
            return None, problem
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return data, ""

    def remember_flashed(self, package: FirmwarePackage, data: bytes) -> Path:
        """Keep the exact package last flashed successfully (the only backup there is)."""
        path = self.cache / "last_flashed.bin"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        (self.cache / "last_flashed.json").write_text(
            json.dumps(
                {"name": package.name, "version": package.version, "sha256": package.sha256}
            ),
            encoding="utf-8",
        )
        return path


def parse_manifest(text: str) -> list[FirmwarePackage]:
    """Entries from a manifest; an entry missing a name or version is skipped."""
    data = json.loads(text)
    out = []
    for entry in data.get("entries", []):
        if not entry.get("name") or not entry.get("version"):
            continue
        out.append(
            FirmwarePackage(
                name=str(entry["name"]),
                version=str(entry["version"]),
                source=str(entry.get("source", "")),
                url=str(entry.get("url", "")),
                sha256=str(entry.get("sha256", "")).lower(),
                license=str(entry.get("license", "")),
                open_source=bool(entry.get("open_source", False)),
                risk=str(entry.get("risk", "")),
                page=str(entry.get("page", "")),
            )
        )
    return out
