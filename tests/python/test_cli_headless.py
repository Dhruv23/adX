"""cli_headless_commands (phase_10.5.md 4.4): each agent-testing command, its exit codes,
and that none of them imports Qt.

Every command runs in its own interpreter, as a shell would run it, with a check after
it that no PySide6 module was loaded."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys

import numpy as np
import pytest

from adx.testing import midi
from adx.testing.wav import write_wav

ROOT = pathlib.Path(__file__).resolve().parents[2]
GOLDEN = ROOT / "tests" / "golden"
PRESETS = ("windows-x64-release", "windows-x64-relwithdebinfo", "windows-x64-debug")

RUN = (
    "import sys, json\n"
    "from adx.cli import main\n"
    "status = main(json.loads(sys.argv[1]))\n"
    "assert not [m for m in sys.modules if m.startswith('PySide6')], 'Qt was imported'\n"
    "sys.exit(status)\n"
)


def adx(*args: str, timeout: float = 180.0) -> subprocess.CompletedProcess[str]:
    run = subprocess.run(
        [sys.executable, "-c", RUN, json.dumps(list(args))],
        capture_output=True,
        text=True,
        check=False,
        timeout=timeout,
        cwd=ROOT,
    )
    assert "Qt was imported" not in run.stderr, run.stderr
    return run


def sine(
    path: pathlib.Path, hz: float = 440.0, seconds: float = 2.0, gain: float = 0.5
) -> np.ndarray:
    rate = 48000
    t = np.arange(int(rate * seconds)) / rate
    tone = (gain * np.sin(2 * np.pi * hz * t)).astype(np.float32)
    fade = np.minimum(1.0, np.minimum(t, t[::-1]) / 0.01).astype(np.float32)
    audio = np.stack([tone * fade, tone * fade], axis=1)
    write_wav(path, audio, rate)
    return audio


def test_analyze_a_project_and_a_file(tmp_path: pathlib.Path) -> None:
    run = adx("analyze", "key", str(GOLDEN / "genre_c418_seed1.adx"))
    assert run.returncode == 0, run.stderr
    result = json.loads(run.stdout)
    assert result["ok"]
    assert result["key"]
    run = adx("analyze", "chords", str(GOLDEN / "genre_c418_seed1.adx"))
    assert run.returncode == 0, run.stderr
    assert json.loads(run.stdout)["segments"]
    run = adx("analyze", "bpm", str(GOLDEN / "genre_stakillaz_seed1.adx"))
    assert run.returncode == 0, run.stderr
    assert "bpm" in json.loads(run.stdout)
    missing = adx("analyze", "key", str(tmp_path / "nothing.wav"))
    assert missing.returncode == 1
    assert adx("analyze", "tempo", "x.wav").returncode == 2  # a usage error


def test_describe_and_compare(tmp_path: pathlib.Path) -> None:
    a, b, quiet = tmp_path / "a.wav", tmp_path / "b.wav", tmp_path / "quiet.wav"
    sine(a)
    sine(b)
    sine(quiet, gain=0.1)
    run = adx("describe", str(a))
    assert run.returncode == 0, run.stderr
    facts = json.loads(run.stdout)
    assert abs(facts["f0_median_hz"] - 440.0) < 5.0
    assert abs(facts["peak_db"] - 20 * np.log10(0.5)) < 0.2
    same = adx("compare", str(a), str(b))
    assert same.returncode == 0, same.stdout + same.stderr
    differ = adx("compare", str(a), str(quiet))
    assert differ.returncode == 1
    assert "loudness" in differ.stdout or "peak" in differ.stdout
    # A looser tolerance lets the same pair through.
    assert (
        adx(
            "compare", str(a), str(quiet), "--band-db", "20", "--lufs", "20", "--peak-db", "20"
        ).returncode
        == 0
    )
    assert adx("describe", str(tmp_path / "none.wav")).returncode == 1


def test_glitch_finds_a_planted_click(tmp_path: pathlib.Path) -> None:
    clean = tmp_path / "clean.wav"
    audio = sine(clean, hz=220.0, gain=0.3)
    assert adx("glitch", str(clean)).returncode == 0
    broken = audio.copy()
    at = 48000
    broken[at] += 0.5
    dirty = tmp_path / "dirty.wav"
    write_wav(dirty, broken, 48000)
    run = adx("glitch", str(dirty))
    assert run.returncode == 1
    assert f"click at frame {at}" in run.stdout
    # Against itself the clean file has no residual either.
    assert adx("glitch", str(clean), "--reference", str(clean)).returncode == 0


def _built(pattern: str) -> pathlib.Path | None:
    for preset in PRESETS:
        found = sorted((ROOT / "build" / preset).glob(pattern))
        if found:
            return found[0]
    return None


def test_plugins_scan_list_probe(tmp_path: pathlib.Path) -> None:
    host = _built("engine/plugin/adx_plugin_host.exe")
    fixture = _built("fixtures/plugins/adx_test_plugin*.clap")
    if host is None or fixture is None:
        pytest.skip("no plugin host or adx_test_plugin built")
    cache = tmp_path / "cache.json"
    common = ("--cache", str(cache), "--host", str(host))
    run = adx("plugins", "scan", str(fixture.parent), "--force", *common)
    assert run.returncode == 0, run.stderr
    run = adx("plugins", "list", "--json", *common)
    assert run.returncode == 0, run.stderr
    names = {p["name"] for p in json.loads(run.stdout)}
    assert "adX Test Effect" in names
    run = adx("plugins", "probe", str(fixture), *common)
    assert run.returncode == 0, run.stderr
    probed = json.loads(run.stdout)
    effect = next(p for p in probed["plugins"] if p["name"] == "adX Test Effect")
    assert [p["key"] for p in effect["params"]] == ["gain", "mode", "latency", "seed"]
    assert "latency" in effect
    assert "editor" in effect
    assert adx("plugins", "probe", str(tmp_path / "x.txt"), *common).returncode == 1


def test_midi_script_parsing() -> None:
    events = midi.parse_script(
        "0.5 note_off 1 60\n0 note_on 1 60 100  # first\n\n0.25 raw B0 40 7F\n"
    )
    assert [(e.at, e.data.hex()) for e in events] == [
        (0.0, "903c64"),
        (0.25, "b0407f"),
        (0.5, "803c00"),
    ]
    assert midi.message("pitch", ["2", "0"]) == bytes([0xE1, 0x00, 0x40])
    with pytest.raises(midi.MidiError, match="line 1"):
        midi.parse_script("0 note_on 17 60 100")
    with pytest.raises(midi.MidiError):
        midi.message("cc", ["1", "200", "1"])


@pytest.mark.skipif(sys.platform != "win32", reason="winmm")
def test_midi_commands(tmp_path: pathlib.Path) -> None:
    run = adx("midi", "ports")
    assert run.returncode == 0, run.stderr
    ports = json.loads(run.stdout)
    assert set(ports) == {"inputs", "outputs"}
    assert adx("midi", "send", "no such port at all", "note_on", "1", "60", "100").returncode == 1
    script = tmp_path / "riff.txt"
    script.write_text("0 note_on 1 60 100\n0.1 note_off 1 60\n", encoding="utf-8")
    loop = [p for p in ports["outputs"] if "loopmidi" in p.lower()]
    if not loop:
        return  # no loopMIDI port on this machine; parsing is covered above
    run = adx("midi", "play", loop[0], str(script))
    assert run.returncode == 0, run.stderr
    assert "played 2 message(s)" in run.stdout


def test_drive_against_a_running_app(tmp_path: pathlib.Path) -> None:
    assert adx("drive", "state").returncode == 1  # no control file
    run = adx("drive", "launch", "--via", "gui", "--project", str(GOLDEN / "single_tone.adx"))
    assert run.returncode == 0, run.stderr
    control = run.stdout.strip().splitlines()[-1]
    try:
        state = adx("drive", "state", "--control-file", control)
        assert state.returncode == 0, state.stderr
        assert json.loads(state.stdout)["project"]["open"]
        assert adx("drive", "trigger", "transport.play", "--control-file", control).returncode == 0
        waited = adx(
            "drive", "wait_for", "transport.playing", "timeout=10", "--control-file", control
        )
        assert waited.returncode == 0, waited.stderr
        refused = adx("drive", "trigger", "no.such.action", "--control-file", control)
        assert refused.returncode == 1
    finally:
        adx("drive", "quit", "save=false", "--control-file", control)
