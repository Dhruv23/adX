"""The FM-1 firmware catalog (phase_11.5.md 4.11, 5): fetch, cache, hash check; offline
uses the cache; closed entries are not installable."""

from __future__ import annotations

import hashlib
import json
import pathlib
import random

from adx.hardware.fm1.firmware import FirmwareCatalog

IMAGE = bytes(random.Random(3).randrange(256) for _ in range(80000))
MANIFEST = {
    "version": 1,
    "source": "test",
    "entries": [
        {
            "name": "Skiff", "version": "1.2", "source": "thefirmwarehub.com",
            "url": "https://example.invalid/skiff.bin",
            "sha256": hashlib.sha256(IMAGE).hexdigest(), "license": "MIT",
            "open_source": True, "risk": "Adds clock out.",
        },
        {"name": "FM-1+VA", "version": "3", "source": "thefirmwarehub.com",
         "open_source": False, "page": "https://example.invalid/va"},
        {"name": "", "version": "1"},
    ],
}  # fmt: skip


class Net:
    def __init__(self, files: dict[str, bytes]) -> None:
        self.files = files
        self.calls: list[str] = []
        self.offline = False

    def __call__(self, url: str) -> bytes:
        self.calls.append(url)
        if self.offline or url not in self.files:
            raise OSError("offline")
        return self.files[url]


def test_firmware_catalog(tmp_path: pathlib.Path) -> None:
    net = Net({
        "https://hub/manifest.json": json.dumps(MANIFEST).encode(),
        "https://example.invalid/skiff.bin": IMAGE,
    })  # fmt: skip
    catalog = FirmwareCatalog(cache=tmp_path, fetch=net)
    entries = catalog.load("https://hub/manifest.json")
    assert catalog.origin == "network"
    assert [e.name for e in entries] == ["Skiff", "FM-1+VA"]
    skiff, closed = entries
    assert skiff.installable
    assert not closed.installable
    data, problem = catalog.download(skiff)
    assert problem == ""
    assert data == IMAGE
    # Closed: a link card, never downloaded.
    data, problem = catalog.download(closed)
    assert data is None
    assert "link only" in problem
    # Offline: the cached manifest and the cached package, still hash-checked.
    net.offline = True
    again = FirmwareCatalog(cache=tmp_path, fetch=net)
    assert [e.name for e in again.load("https://hub/manifest.json")] == ["Skiff", "FM-1+VA"]
    assert again.origin == "cache"
    data, problem = again.download(again.entries[0])
    assert data == IMAGE
    # A cached file that has changed is not offered.
    again.package_path(skiff).write_bytes(IMAGE[:-1] + b"\x00")
    data, problem = again.download(skiff)
    assert data is None
    assert problem
    # A hash mismatch from the network is refused too.
    net.offline = False
    net.files["https://example.invalid/skiff.bin"] = IMAGE + b"x"
    data, problem = FirmwareCatalog(cache=tmp_path, fetch=net).download(skiff)
    assert data is None
    assert "SHA-256" in problem
    # No network and no cache: an empty catalog, not an error.
    empty = FirmwareCatalog(cache=tmp_path / "none", fetch=Net({}))
    assert empty.load("https://hub/manifest.json") == []
    assert empty.origin == ""
    # The last package flashed is kept.
    kept = catalog.remember_flashed(skiff, IMAGE)
    assert kept.read_bytes() == IMAGE
