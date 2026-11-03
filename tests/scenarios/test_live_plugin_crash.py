"""scenario_live_plugin_crash (phase_10.5.md 4.6): P9-3, live.

adx_test_plugin and a real plugin (Airwindows Consolidated, CLAP, on the surviving
channel's insert) play on separate channels. The fixture's
``mode`` knob is turned to 1 (crash in process) through its generic editor with
synthesized input. Then: the crash notice shows, the surviving channel's sound in the
capture matches its offline render throughout, xruns stay at zero, and after
Edit > Restart crashed plugins the fixture's path is audible again.

The fixture plays at gain 0 until the end, so the master is the surviving channel alone
and the capture can be held to the offline render of the same file. Its gain comes up,
by the same synthesized input, after the restart.

The fixture is marked isolated. Plugins share one sandbox process by default, and a
crash takes everything in that process with it (phase_9.md's
plugin_shared_child_isolation). Isolating a plugin is how adX keeps a bad one from
taking the rest down, so that is the setup this gate holds to.

The real plugin is an effect after the channel's own instrument rather than Dexed or
Surge XT as the instrument: the residual is held to -90 dBFS, and both synths differ
from their own offline render by more than that (test_real_plugins.NOT_DETERMINISTIC).
It runs Airwindows' Density, a static saturator, set through its state. The default,
Galactic, is a modulated reverb whose LFO runs while the transport is stopped, so a
live take never starts at the phase an offline render does (measured 2026-11-03:
-30 dBFS apart from the first block). A processor named without its parameters gets
zeros, which leaves some transparent and others silent, so they are given.
"""

from __future__ import annotations

import base64
import pathlib
import struct
import subprocess
import sys

import numpy as np
import pytest
from wannacry import MATERIAL, VIA, project_for

from adx.engine_bridge import Project, commands
from adx.testing.driver import Driver
from adx.testing.glitch import align, residuals
from adx.testing.wav import read_wav

ROOT = pathlib.Path(__file__).resolve().parents[2]
PRESETS = ("windows-x64-release", "windows-x64-relwithdebinfo", "windows-x64-debug")

pytestmark = [
    pytest.mark.realplugin,
    pytest.mark.skipif(sys.platform != "win32", reason="the Windows launcher and plugin host"),
]


def fixture_plugin() -> pathlib.Path:
    for preset in PRESETS:
        found = ROOT / "build" / preset / "fixtures" / "plugins" / "adx_test_plugin.clap"
        if found.exists():
            return found
    pytest.skip("adx_test_plugin.clap not built")


def airwindows(plugin_folder: pathlib.Path) -> pathlib.Path:
    return plugin_folder / "Airwindows Consolidated" / "Airwindows Consolidated.clap"


@pytest.fixture(scope="session")
def plugin_folder() -> pathlib.Path:
    run = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "fetch_test_plugins.py")],
        capture_output=True,
        text=True,
        check=False,
    )
    assert run.returncode == 0, run.stderr
    return pathlib.Path(run.stdout.strip().splitlines()[-1])


def airwindows_state(processor: str, params: tuple[float, ...]) -> str:
    """Airwindows Consolidated's state (JUCE's binary XML): ``processor`` and its knobs."""
    knobs = " ".join(f'awp_{i}="{v}"' for i, v in enumerate(params))
    xml = (
        '<?xml version="1.0" encoding="UTF-8"?> <awconsolidated streamingVersion="8524" '
        f'currentProcessorName="{processor}" {knobs}/>'
    ).encode()
    blob = b"VC2!" + struct.pack("<I", len(xml) + 1) + xml + bytes(1)
    return "base64:" + base64.b64encode(blob).decode()


def setup(material: str, folder: pathlib.Path, song: pathlib.Path) -> tuple[str, str]:
    """The project: Airwindows on the first channel's insert, the second channel on the
    fixture at gain 0. Returns (surviving channel, fixture channel)."""
    project, _ = Project.load(str(project_for(material)))
    first, second = project.channels_info()[:2]
    project.execute(
        commands.build(
            "AddPluginSlot",
            int(first["output"]),
            "clap",
            "com.airwindows.consolidated",
            str(airwindows(folder)),
            "Airwindows Consolidated",
        )
    )
    project.execute(
        commands.build(
            "SetPluginInstrument",
            int(second["id"]),
            "clap",
            "com.adx.test.synth",
            str(fixture_plugin()),
            "adX Test Synth",
        )
    )
    text = project.dumps()
    slot = text.index("com.airwindows.consolidated")
    slot = text.index("\n", slot) + 1
    text = (
        text[:slot]
        + f'    STATE "{airwindows_state("Density", (0.4, 0.0, 1.0, 1.0))}"\n'
        + text[slot:]
    )
    header = f"[CHANNEL {second['name']}]\n"
    at = text.index(header) + len(header)
    output = text.index("OUTPUT=", at)
    song.write_text(
        text[:output] + "PLUGIN_ISOLATED=yes\nPARAM gain=0\n" + text[output:],
        encoding="utf-8",
        newline="",
    )
    return str(first["name"]), str(second["name"])


def value(app: Driver, path: str) -> float:
    """A plugin parameter the file does not set reads as None: its default, 0 here."""
    read = app.script(f"result = window.project.param_value({path!r})")
    return 0.0 if read is None else float(read)


def turn(app: Driver, path: str, target: float, *, up: bool) -> None:
    """Scroll the parameter's knob a notch at a time until it passes ``target``."""
    for _ in range(80):
        current = value(app, path)
        if (current >= target) if up else (current <= target):
            return
        app.scroll(f"param:{path}", dy=120 if up else -120)
    raise AssertionError(f"{path} stuck at {value(app, path)}")


def residual_db(capture: np.ndarray, reference: np.ndarray) -> float:
    shift = align(capture, reference)
    c, r = (capture[shift:], reference) if shift >= 0 else (capture, reference[-shift:])
    n = min(len(c), len(r))
    diff = c[:n].astype(np.float64) - r[:n]
    return float(10 * np.log10(max(np.mean(diff**2), 1e-30)))


@pytest.mark.parametrize("material", MATERIAL)
def test_live_plugin_crash(
    material: str, plugin_folder: pathlib.Path, tmp_path: pathlib.Path, evidence: pathlib.Path
) -> None:
    song = tmp_path / "song.adx"
    _, victim = setup(material, plugin_folder, song)
    reference = tmp_path / "reference.wav"
    run = subprocess.run(
        [
            sys.executable,
            "-m",
            "adx",
            "render",
            str(song),
            "-o",
            str(reference),
            "--format",
            "wav32f",
        ],
        capture_output=True,
        text=True,
        check=False,
        cwd=ROOT,
    )
    assert run.returncode == 0, run.stderr
    ref, rate = read_wav(reference)
    mode, gain = f"channel.{victim}.mode", f"channel.{victim}.gain"

    with Driver.launch(song, via=VIA, settings_dir=evidence / "settings") as app:
        app.wait_for("project.open", timeout=30)
        app.wait_for(lambda s: len(s["plugins"]["instances"]) >= 2, timeout=60)
        xruns = int(app.state()["audio"]["xruns"])
        app.trigger("view.channel_rack")
        app.click(f"channel_rack.row:{2}.name")
        app.trigger("view.instrument")
        app.capture_start()
        app.trigger("transport.play")
        app.wait_for("transport.position_seconds", predicate="gt", value=2.0, timeout=20)

        app.choose(f"param:{mode}", index=1)  # crash in process
        assert value(app, mode) == 1.0
        crashed = app.wait_for(
            lambda s: any(n["kind"] == "crashed" for n in s["plugins"]["notices"]), timeout=15
        )
        assert any(
            victim in n["text"] or "adX Test Synth" in n["text"]
            for n in crashed["plugins"]["notices"]
        )
        app.wait_for("transport.position_seconds", predicate="gt", value=6.0, timeout=20)
        take = app.capture_stop(evidence / "crash.wav")
        assert int(app.state()["audio"]["xruns"]) == xruns
        assert app.state()["transport"]["playing"]

        # Back: mode 0 first (the project holds the knob, and a restart replays it), then
        # Edit > Restart crashed plugins, then the fixture's gain up.
        app.choose(f"param:{mode}", index=0)
        assert value(app, mode) == 0.0
        app.trigger("edit.restart_plugins")
        app.wait_for(
            lambda s: any(n["kind"] == "restarted" for n in s["plugins"]["notices"]), timeout=30
        )
        turn(app, gain, 0.9, up=True)
        app.capture_start()
        app.wait_for("transport.position_seconds", predicate="gt", value=9.0, timeout=20)
        after = app.capture_stop(evidence / "after.wav")

    captured, _ = read_wav(take["path"])
    assert take["lost"] == 0
    assert float(abs(captured).max()) > 0.01, "the surviving channel is silent"
    findings = residuals(captured, ref, rate)
    assert findings == [], [str(f) for f in findings[:5]]
    # The fixture's path audible again: the master is no longer the survivor alone.
    restored, _ = read_wav(after["path"])
    assert residual_db(restored, ref) > -60.0, "the restarted fixture is not heard"
