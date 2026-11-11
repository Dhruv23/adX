"""The library store and pack tools (phase_12.md 4.8, 4.9, 5): fetch_verifies_hashes,
licence_gate, kick_lab_deterministic, and the SFZ and multisample tools."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "packs"))

import fetch_libraries  # noqa: E402
import kick_lab  # noqa: E402
import licence_report  # noqa: E402
import render_multisample  # noqa: E402


def _git(args: list[str], cwd: Path) -> str:
    done = subprocess.run(
        ["git", "-c", "user.name=t", "-c", "user.email=t@t", *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=True,
    )
    return done.stdout.strip()


@pytest.fixture
def upstream(tmp_path: Path) -> fetch_libraries.Library:
    """A one-commit git repository standing in for a library's upstream."""
    origin = tmp_path / "origin"
    (origin / "Strings").mkdir(parents=True)
    (origin / "Strings" / "a.wav").write_bytes(b"RIFF-a" * 100)
    (origin / "b.sfz").write_text("<region> sample=Strings/a.wav\n", encoding="utf-8")
    _git(["init", "-q"], origin)
    _git(["add", "-A"], origin)
    _git(["commit", "-q", "-m", "library"], origin)
    # uploadpack.allowReachableSHA1InWant lets a shallow fetch ask for the commit by id.
    _git(["config", "uploadpack.allowReachableSHA1InWant", "true"], origin)
    commit = _git(["rev-parse", "HEAD"], origin)
    return fetch_libraries.Library(
        "testlib", "Test", origin.as_uri(), commit, "CC0-1.0", True, 0.0, "nobody"
    )


def test_fetch_verifies_hashes(tmp_path: Path, upstream: fetch_libraries.Library) -> None:
    store = tmp_path / "store"
    manifests = tmp_path / "manifests"
    first = fetch_libraries.fetch(upstream, store, manifests, write=True)
    assert first.ok
    assert first.checked == 2
    folder = store / upstream.folder
    manifest = fetch_libraries.manifest_path(upstream, manifests)
    assert fetch_libraries.read_manifest(manifest).keys() == {"Strings/a.wav", "b.sfz"}

    # A corrupted file is detected and restored from the clone; the store is whole again.
    (folder / "Strings" / "a.wav").write_bytes(b"damaged")
    result = fetch_libraries.verify(upstream, folder, manifest)
    assert result.repaired == ["Strings/a.wav"]
    assert result.ok
    assert (folder / "Strings" / "a.wav").read_bytes() == b"RIFF-a" * 100

    # Without the clone to restore from, the mismatch fails the check.
    (folder / "b.sfz").write_text("changed", encoding="utf-8")
    assert fetch_libraries.verify(upstream, folder, manifest, repair=False).failed == ["b.sfz"]

    # index.json says where each library is: what lib:// resolves through.
    index = (store / "index.json").read_text(encoding="utf-8")
    assert upstream.folder in index


def test_pack_manifest_parses() -> None:
    pack = licence_report.parse_pack(
        '[PACK]\nID=classical\nNAME="Classical"\nVERSION=1\nLIBRARIES=vsco2ce, vcsl\n'
        "LICENCE=CC0-1.0\n\n[ASSETS]\n"
        'ASSET "lib://vsco2ce/" licence=CC0-1.0 redistributable=yes credit="Versilian Studios"\n'
        'ASSET "lib://iowa-mis/" licence="free to use" redistributable=no\n'
    )
    assert pack.id == "classical"
    assert pack.libraries == ["vsco2ce", "vcsl"]
    assert [a.redistributable for a in pack.assets] == [True, False]
    assert pack.assets[0].credit == "Versilian Studios"
    assert pack.assets[1].licence == "free to use"


def test_licence_gate(tmp_path: Path) -> None:
    packs = tmp_path / "packs"
    (packs / "classical").mkdir(parents=True)
    (packs / "classical" / "pack.adxpack").write_text(
        '[PACK]\nID=classical\n\n[ASSETS]\nASSET "lib://iowa-mis/" licence=x redistributable=no\n',
        encoding="utf-8",
    )
    stage = tmp_path / "stage"
    (stage / "examples").mkdir(parents=True)
    (stage / "examples" / "ok.wav").write_bytes(b"fine")
    assert licence_report.gate(stage, packs) == []

    (stage / "examples" / "iowa-mis" / "Violin").mkdir(parents=True)
    (stage / "examples" / "iowa-mis" / "Violin" / "C4.aif").write_bytes(b"x")
    (stage / "gm.dls").write_bytes(b"DLS ")
    problems = licence_report.gate(stage, packs)
    assert len(problems) == 2
    assert any("gm.dls" in p for p in problems)
    assert any("iowa-mis" in p for p in problems)
    # The notice lists the fetchable libraries.
    assert "Salamander" in licence_report.section(packs)


def test_import_sfz_writes_a_preset_and_report(tmp_path: Path) -> None:
    import import_sfz

    samples = tmp_path / "lib" / "Strings"
    samples.mkdir(parents=True)
    render_multisample.write_wav(samples / "a.wav", [[0.0, 0.0]] * 10, 48000)
    sfz = tmp_path / "lib" / "violin.sfz"
    sfz.write_text(
        "<group> sw_lokey=24 sw_hikey=25 sw_last=24\n"
        "<region> sample=Strings/a.wav lokey=55 hikey=70 pitch_keycenter=60 fil_type=lpf_2p\n",
        encoding="utf-8",
    )
    out = tmp_path / "presets"
    result = import_sfz.import_one(sfz, out, "test", "Strings")
    assert result["regions"] == 1
    assert result["unknown"] == 1
    text = (out / "violin.adxpreset").read_text(encoding="utf-8")
    assert "CATEGORY=Strings" in text
    assert 'ZONE "../lib/Strings/a.wav"' in text
    assert "sw=24..25:24" in text
    assert "fil_type" in (out / "violin.import.txt").read_text(encoding="utf-8")


KICK = """[RACK]
NAME="Lab Kick"
PACK=test
CATEGORY=Drums/Kick

[LAYER body]
TYPE=drumsynth
PARAM model=0
PARAM keyTrack=1
PARAM decay=0.2

[CHAIN]
SLOT Clipper mode=1 drive=9
"""


def test_kick_lab_deterministic(tmp_path: Path) -> None:
    rack = tmp_path / "kick.adxrack"
    rack.write_text(KICK, encoding="utf-8")
    runs = []
    for name in ("one", "two"):
        result = kick_lab.lab(rack, tmp_path / name, "Lab", "test", 40, 41, 2, 0.4)
        runs.append([Path(f).read_bytes() for f in result["files"]])
        preset = Path(result["preset"]).read_text(encoding="utf-8")
        assert 'ZONE "lib://rendered/test/Lab/040.wav" key=40 root=40' in preset
    assert len(runs[0]) == 2
    assert runs[0] == runs[1]
    # The second pass went through the chain: the two keys are different renders.
    assert runs[0][0] != runs[0][1]


def test_render_multisample_layout(tmp_path: Path) -> None:
    preset = tmp_path / "saw.adxpreset"
    preset.write_text('[PRESET]\nNAME="Saw"\nTYPE=va\nPACK=test\nCATEGORY=Lead\n', encoding="utf-8")
    result = render_multisample.render(
        preset, tmp_path / "out", "Saw R", "test", 48, 60, 6, [60, 127], 1, 0.5, 0.2
    )
    assert len(result["files"]) == 3 * 2
    text = Path(result["preset"]).read_text(encoding="utf-8")
    assert "key=0..51 root=48 vel=1..60" in text
    assert "key=58..127 root=60 vel=61..127" in text
    assert render_multisample.zone_ranges([48, 54, 60]) == [(0, 51), (52, 57), (58, 127)]


def test_bank_fixtures_are_what_the_generator_writes(tmp_path: Path) -> None:
    import make_bank_fixtures

    for path in make_bank_fixtures.generate(tmp_path):
        committed = REPO / "tests" / "data" / "banks" / path.name
        assert committed.read_bytes() == path.read_bytes(), path.name
        assert path.stat().st_size < 200_000
