"""The `core` pack (phase_13.md §5): it loads, its kits sit on General MIDI's keys, its
pattern templates paste as one undo step, a missing library plays the fallback, and
every audition lands in its category's loudness window."""

from __future__ import annotations

import pathlib
import re
import sys

import numpy as np
import pytest

from adx import engine_bridge
from adx.engine_bridge import Project

REPO = pathlib.Path(__file__).resolve().parents[2]
CORE = REPO / "engine" / "preset" / "packs" / "core"
PACKS = str(REPO / "engine" / "preset" / "packs")
BLANK = '[PROJECT]\nADX_VERSION=2\n\n[MIXER]\nINSERT 1 name="Master"\n'

#: What each kit must have on which keys (General MIDI's percussion map).
GM_KEYS = {"kick": [36], "snare": [38], "hat": [42, 46], "cymbal": [49]}


def files(pattern: str) -> list[pathlib.Path]:
    return sorted(CORE.rglob(pattern))


def layers_of(rack: pathlib.Path) -> dict[str, dict[str, str]]:
    """Each [LAYER name] section's keys, as the file writes them."""
    out: dict[str, dict[str, str]] = {}
    current: dict[str, str] | None = None
    for line in rack.read_text(encoding="utf-8").splitlines():
        if m := re.match(r"\[LAYER (.+)\]$", line):
            current = out.setdefault(m.group(1), {})
        elif line.startswith("["):
            current = None
        elif current is not None and "=" in line and not line.startswith(("PARAM ", "SLOT ")):
            key, _, value = line.partition("=")
            current[key] = value.strip('"')
    return out


def params_of(preset: pathlib.Path) -> dict[str, float]:
    """A preset file's [PARAMS] values."""
    text = preset.read_text(encoding="utf-8").split("[PARAMS]", 1)[-1].split("\n[", 1)[0]
    return {k: float(v.split()[0]) for k, v in re.findall(r"^([\w.]+)=(\S+)", text, re.M)}


def keys_of(text: str) -> set[int]:
    lo, _, hi = text.partition("..")
    return set(range(int(lo), int(hi or lo) + 1))


def test_pack_core_contents() -> None:
    manifest = (CORE / "pack.adxpack").read_text(encoding="utf-8")
    assert "ID=core" in manifest
    assert "LIBRARIES=vcsl" in manifest
    presets = engine_bridge.scan_presets([str(CORE)])
    racks = engine_bridge.scan_racks([str(CORE)])
    assert len(presets) >= 40
    assert len([r for r in racks if r["category"] == "Drums/Kit"]) >= 7
    assert len([r for r in racks if str(r["category"]).startswith("Chain")]) == 9
    assert len(files("patterns/*.adx")) == 16
    assert all(p["pack"] == "core" for p in presets + racks)


@pytest.mark.parametrize("rack", files("*.adxrack"), ids=lambda p: p.stem)
def test_pack_core_racks_insert(rack: pathlib.Path) -> None:
    """pack_core_loads, for racks: each inserts into an empty project with no diagnostic."""
    project, _ = Project.loads(BLANK)
    result = project.insert_rack(str(rack), [PACKS])
    assert not result.get("diagnostics"), result.get("diagnostics")
    assert not result.get("unresolved"), result.get("unresolved")


@pytest.mark.parametrize("kit", files("kits/*.adxrack"), ids=lambda p: p.stem)
def test_pack_core_kits_gm_map(kit: pathlib.Path) -> None:
    layers = layers_of(kit)
    for role, keys in GM_KEYS.items():
        assert role in layers, f"{kit.stem} has no {role} layer"
        covered = keys_of(layers[role]["KEYS"])
        assert set(keys) <= covered, f"{kit.stem}: {role} is on {sorted(covered)}"
    # The closed and open hat are one layer, so one voice can choke the other; a synth
    # kit's hat preset turns the choke on.
    hat = layers["hat"]["PRESET"].split("/", 1)[1]
    presets = {p["name"]: p for p in engine_bridge.scan_presets([str(CORE)])}
    params = params_of(pathlib.Path(str(presets[hat]["path"])))
    if presets[hat]["type"] == "drumsynth":
        assert float(params.get("hat.choke", 0)) == 1.0
        assert float(params.get("hat.openDecay", 0)) > float(params.get("decay", 0.4))
    # No two layers answer the same GM key unless they are meant to stack (a layered
    # snare): each key's layers are named for one role.
    seen: dict[int, list[str]] = {}
    for name, layer in layers.items():
        for key in keys_of(layer["KEYS"]):
            seen.setdefault(key, []).append(name)
    for key in (36, 42, 46, 49):
        assert len(seen.get(key, [])) == 1, f"{kit.stem}: key {key} on {seen.get(key)}"


@pytest.mark.parametrize("fragment", files("patterns/*.adx"), ids=lambda p: p.stem)
def test_pattern_templates_parse(fragment: pathlib.Path) -> None:
    """Each parses clean and pastes onto a kit channel as one undo step."""
    source, diagnostics = Project.load(str(fragment))
    assert not diagnostics, diagnostics
    pattern = source.patterns()[0]
    notes = source.notes(pattern, source.pattern_channels(pattern)[0])
    assert len(notes) >= 4
    project, _ = Project.loads(BLANK)
    project.insert_rack(str(CORE / "kits" / "808-kit.adxrack"), [PACKS])
    channel = project.channels()[0]
    project.apply(engine_bridge.commands.build("AddPatternNew", "Pasted", 3840 * 4 * 8, ""))
    depth = project.undo_depth()
    notes["id"] = 0
    project.apply(
        engine_bridge.commands.edit_notes(
            "Pasted",
            channel,
            np.zeros(0, dtype=np.uint32),
            engine_bridge.empty_notes(),
            notes,
            "Paste notes",
        )
    )
    assert project.undo_depth() == depth + 1
    assert len(project.notes("Pasted", channel)) == len(notes)


#: Run in its own process: library lookups are remembered per process, so the empty
#: store has to be the only one it has seen.
_FALLBACK_PROBE = """
import json, sys
sys.path.insert(0, sys.argv[1])
from adx import engine_bridge
from adx.engine_bridge import Project
blank = ('[PROJECT]\\nADX_VERSION=2\\n\\n'
         '[CHANNEL Drum]\\nINSTRUMENT=sampler\\nOUTPUT=insert.1\\n\\n'
         '[MIXER]\\nINSERT 1 name="Master"\\n')
out = {}
for preset in engine_bridge.scan_presets([sys.argv[2]]):
    if preset["type"] != "sampler":
        continue
    project, _ = Project.loads(blank)
    diagnostics = project.apply_preset(int(project.channels_info()[0]["id"]), str(preset["path"]))
    out[preset["name"]] = [d.get("code") for d in diagnostics]
print(json.dumps(out))
"""


def test_pack_core_fallbacks(tmp_path: pathlib.Path) -> None:
    """With no library store, every sampled preset names a synthesized core fallback, and
    loading it raises exactly one diagnostic: the missing library (ADX3006)."""
    import json
    import os
    import subprocess

    sampled = [p for p in engine_bridge.scan_presets([str(CORE)]) if p["type"] == "sampler"]
    assert len(sampled) >= 10
    synth = {p["name"] for p in engine_bridge.scan_presets([str(CORE)]) if p["type"] != "sampler"}
    for preset in sampled:
        fallback = str(preset["fallback"])
        assert fallback.startswith("core/"), preset["name"]
        assert fallback.split("/", 1)[1] in synth, f"{preset['name']} falls back to {fallback}"
    env = {**os.environ, "ADX_LIBRARY_STORE": str(tmp_path / "empty")}
    done = subprocess.run(
        [sys.executable, "-c", _FALLBACK_PROBE, str(REPO / "app"), str(CORE)],
        capture_output=True,
        text=True,
        env=env,
        check=True,
        timeout=300,
    )
    codes = json.loads(done.stdout.strip().splitlines()[-1])
    assert set(codes) == {str(p["name"]) for p in sampled}
    for name, found in codes.items():
        assert found == ["ADX3006"], f"{name}: {found}"


def _uses_sampled(rack: pathlib.Path) -> bool:
    sampled = {p["name"] for p in engine_bridge.scan_presets([str(CORE)]) if p["type"] == "sampler"}
    return any(f'core/{name}"' in rack.read_text(encoding="utf-8") for name in sampled)


@pytest.mark.parametrize(
    "sound",
    [*files("*.adxpreset"), *[r for r in files("*.adxrack") if r.parent.name != "chains"]],
    ids=lambda p: p.stem,
)
def test_pack_loudness_window(sound: pathlib.Path) -> None:
    """Every audition within the taxonomy's window of its category's target. A sound
    that samples a library is measured only where the library is installed (not CI)."""
    sys.path.insert(0, str(REPO / "tools" / "packs"))
    import audition
    from fetch_libraries import LIBRARIES, store_root

    from adx import taxonomy

    sampled = "lib://" in sound.read_text(encoding="utf-8") or _uses_sampled(sound)
    if sampled and not (store_root() / LIBRARIES["vcsl"].folder).is_dir():
        pytest.skip("samples VCSL, which is not in the library store")

    result = audition.audition(sound, out=REPO / "build" / "core-audition")
    target = taxonomy.category(result["category"]).loudness_target
    assert abs(float(result["lufs"]) - target) <= taxonomy.LOUDNESS_WINDOW, (
        f"{sound.stem}: {result['lufs']:.1f} LUFS, target {target}"
    )


def test_pack_core_demo_renders(tmp_path: pathlib.Path) -> None:
    """docs/examples/core_demo.adx: 32 bars from core alone, loads clean, and renders
    offline bit-identically to the realtime engine's capture of the same project."""
    import time

    from adx.engine_bridge import Engine
    from adx.testing import glitch
    from adx.testing.wav import read_wav

    demo = REPO / "docs" / "examples" / "core_demo.adx"
    project, diagnostics = Project.load(str(demo))
    assert not diagnostics, diagnostics
    assert project.info()["length_seconds"] == pytest.approx(60.0, abs=0.01)
    seconds = 12.0
    path = tmp_path / "offline.wav"
    engine_bridge.render_to_file(project, str(path), tail_seconds=0.0)
    offline, _rate = read_wav(path)
    engine = Engine(loopback=0)
    engine.set_project(project)
    engine.start()
    try:
        engine.capture_start()
        engine.play()
        rendered = 0
        for block in range(int(seconds * 48000 / 256)):
            engine.render_offline(256)
            rendered += 256
            if block % 16 == 15:
                deadline = time.monotonic() + 5.0
                while engine.capture_frames() < rendered and time.monotonic() < deadline:
                    time.sleep(0.002)
        result = engine.capture_stop()
        assert int(result["lost"]) == 0
    finally:
        engine.stop()
    captured = np.asarray(result["samples"], dtype=np.float32).reshape(-1, 2)
    assert float(np.abs(captured).max()) > 0.01
    assert glitch.bit_identical(captured, offline[: len(captured)])
