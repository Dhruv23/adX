"""The FM-1 scenarios (phase_11.5.md 5): adX launched through adX.cmd, the FM-1 played by
tools/virtual_fm1.py on a virtual port named "FM-1 Midi", the user's steps through the
panel's controls, the outcome read back from the app and from the emulated device.

    scenario fm1_import_bank             Bank Export -> librarian -> dx7 channel -> plays
                                         -> edited -> saved -> reopened -> exported
    scenario fm1_live_edit               editor changes arrive as DX7 parameter changes
    scenario fm1_firmware_install_emulated   catalog -> verify -> typed confirm -> served
                                         -> done -> identity re-read; and interrupted

They need teVirtualMIDI (installed with loopMIDI), so they run locally and skip on CI.
They do not cover a real flash, real USB re-enumeration, audio, or Bluetooth.
"""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import random
import sys
import time
from collections.abc import Iterator
from typing import Any

import pytest

from adx.engine_bridge import Project
from adx.hardware import dx7
from adx.testing.driver import Driver

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import virtual_fm1  # noqa: E402

FIXTURES = ROOT / "tests" / "fixtures" / "dx7"

pytestmark = [
    pytest.mark.skipif(sys.platform != "win32", reason="adX.cmd is the Windows launcher"),
    pytest.mark.skipif(
        bool(os.environ.get("CI")) or not virtual_fm1.available(),
        reason="needs teVirtualMIDI (loopMIDI's driver); not on CI",
    ),
]


def until(app: Driver, code: str, timeout: float = 15.0) -> Any:  # noqa: ANN401 - JSON
    """Run ``code`` (which sets ``result``) in the app until it is truthy."""
    deadline = time.monotonic() + timeout
    last: Any = None
    while time.monotonic() < deadline:
        last = app.script(code)
        if last:
            return last
        time.sleep(0.2)
    raise AssertionError(f"not within {timeout} s: {code!r} -> {last!r}")


def launch(evidence: pathlib.Path) -> Driver:
    return Driver.launch(
        via="adX.cmd",
        settings_dir=evidence / "settings",
        env={"ADX_FM1": "1", "ADX_FM1_CACHE": str(evidence / "fm1-cache")},
    )


CONNECTED = "result = window.fm1.device is not None and window.fm1.identity is not None"


@pytest.fixture
def device() -> Iterator[virtual_fm1.VirtualFm1]:
    fm1 = virtual_fm1.VirtualFm1(model="FM-1_016")
    yield fm1
    fm1.close()


def test_fm1_import_bank(
    device: virtual_fm1.VirtualFm1, evidence: pathlib.Path, tmp_path: pathlib.Path
) -> None:
    with launch(evidence) as app:
        until(app, CONNECTED)
        assert app.script("result = window.fm1.identity.model") == "FM-1_016"
        app.trigger("view.hardware")
        # Receive: the emulator ignores the dump request, so the panel asks for Bank
        # Export; the user presses it on the device.
        app.click("fm1.librarian.receive")
        until(app, "result = window.fm1.waiting_for_export", timeout=6)
        device.bank_export()
        until(app, "result = window.hardware.librarian.list.count() == 32")
        voices = app.script("result = window.hardware.librarian.voices")
        assert voices == device.bank
        # Open the fourth as a dx7 channel.
        app.script("window.hardware.librarian.list.setCurrentRow(3)")
        app.click("fm1.librarian.open")
        name = until(
            app,
            "c = [c for c in window.project.channels_info() if c['instrument'] == 'dx7']\n"
            "result = c[-1]['name'] if c else None",
        )
        # It plays.
        app.script(f"window.audition_channel({name!r})")
        take = app.capture(1.0, evidence / "audition.wav")
        from adx.testing.wav import read_wav

        audio, _rate = read_wav(take["path"])
        assert float(abs(audio).max()) > 0.001, "the voice did not sound"
        # Edited: a parameter turned as a knob would turn it.
        app.script(
            "from adx.panels.params import channel_path\n"
            f"window.session.set_param(channel_path({name!r}, 'op1.level'), 77.0)\n"
            "window.session.seal()"
        )
        edited = list(voices[3])
        edited[dx7.op_byte(1, "level")] = 77
        saved = tmp_path / "fm1.adx"
        app.script(
            f"window.project.save({str(saved)!r})\nwindow.saved_revision = "
            "window.project.revision()"
        )
    # Reopened: the same voice.
    project, diagnostics = Project.load(str(saved))
    assert not [d for d in diagnostics if d["severity"] == "error"]
    info = next(c for c in project.channels_info() if c["name"] == name)
    held = {str(p[0]): float(p[1]) for p in info["params"]}
    defaults = dx7.init_voice()
    from adx import engine_bridge

    base = engine_bridge.dx7_voice_to_params(defaults)
    names = dx7.param_names()[:156]
    reopened = engine_bridge.dx7_params_to_voice(
        [held.get(n, base[i]) for i, n in enumerate(names)]
    )
    assert reopened == edited
    # Exported .syx equals the edited voice.
    out = dx7.write_voice(tmp_path / "edited.syx", reopened)
    assert dx7.read_file(out)["voices"] == [edited]
    (evidence / "result.json").write_text(json.dumps({"channel": name}), encoding="utf-8")


def test_fm1_import_from_syx(device: virtual_fm1.VirtualFm1, evidence: pathlib.Path) -> None:
    with launch(evidence) as app:
        until(app, CONNECTED)
        app.trigger("view.hardware")
        count = app.script(
            f"result = window.hardware.librarian.import_file({str(FIXTURES / 'random.syx')!r})"
        )
        assert count == 32
        app.script("window.hardware.librarian.list.setCurrentRow(0)")
        app.click("fm1.librarian.send_voice")
        expected = dx7.read_file(FIXTURES / "random.syx")["voices"][0]
        deadline = time.monotonic() + 10
        while device.edit != expected and time.monotonic() < deadline:
            time.sleep(0.1)
        assert device.edit == expected


def test_fm1_live_edit(device: virtual_fm1.VirtualFm1, evidence: pathlib.Path) -> None:
    with launch(evidence) as app:
        until(app, CONNECTED)
        voice = dx7.read_file(FIXTURES / "random.syx")["voices"][9]
        name = app.script(
            f"from adx.hardware import dx7\nresult = dx7.open_as_channel(window.session, {voice!r})"
        )
        cid = app.script(
            f"result = [int(c['id']) for c in window.project.channels_info() "
            f"if c['name'] == {name!r}][0]"
        )
        # "Send to FM-1 while editing", as the editor's checkbox turns it on.
        app.script(
            "from adx.panels.instrument.dx7 import editor\n"
            f"[hook({cid}, True) for hook in editor.LIVE_EDIT_HOOK]"
        )
        deadline = time.monotonic() + 10
        while device.edit != voice and time.monotonic() < deadline:
            time.sleep(0.1)
        assert device.edit == voice, "the whole voice was not sent"
        before = device.param_changes
        rng = random.Random(4)
        edits = {}
        for _ in range(12):
            byte = rng.choice(
                [
                    dx7.op_byte(op, f)
                    for op in range(1, 7)
                    for f in ("level", "eg.r1", "coarse", "kvs")
                ]
            )
            edits[byte] = rng.randint(0, 31 if byte % 21 == 18 else 7 if byte % 21 == 15 else 99)
        script = "from adx.panels.params import channel_path\n"
        for byte, value in edits.items():
            pname = dx7.param_names()[byte]
            script += (
                f"window.session.set_param(channel_path({name!r}, {pname!r}), {float(value)})\n"
            )
        app.script(script)
        expected = app.script(
            f"from adx.hardware import dx7\nresult = dx7.channel_voice(window.session, {cid})"
        )
        deadline = time.monotonic() + 10
        while device.edit != expected and time.monotonic() < deadline:
            time.sleep(0.1)
        assert device.param_changes > before, "no parameter changes arrived"
        assert device.edit == expected


def write_catalog(cache: pathlib.Path, image: bytes) -> pathlib.Path:
    package = cache / "skiff.bin"
    package.write_bytes(image)
    manifest = cache / "manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "version": 1,
                "entries": [
                    {
                        "name": "Skiff",
                        "version": "9.9",
                        "source": "test",
                        "url": package.as_uri(),
                        "sha256": hashlib.sha256(image).hexdigest(),
                        "license": "MIT",
                        "open_source": True,
                        "risk": "Test image.",
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    return manifest


def install(app: Driver, manifest: pathlib.Path) -> None:
    app.trigger("view.hardware")
    app.script(
        "tab = window.hardware.firmware\n"
        "import urllib.request\n"
        "tab.catalog.fetch = lambda url: urllib.request.urlopen(url).read()\n"
        f"tab.url.setText({manifest.as_uri()!r})\n"
        "result = tab.load()"
    )
    assert app.script(
        "tab = window.hardware.firmware\ntab.list.setCurrentRow(0)\nresult = tab.prepare()"
    )
    # Typed wrong: nothing sent. Typed right: enter-upgrade goes out.
    assert not app.script("result = window.hardware.firmware.confirm_and_start('skiff')")
    assert app.script(
        "tab = window.hardware.firmware\nresult = tab.prepare() and tab.confirm_and_start('Skiff')"
    )


def test_fm1_firmware_install_emulated(
    device: virtual_fm1.VirtualFm1, evidence: pathlib.Path, tmp_path: pathlib.Path
) -> None:
    image = bytes(random.Random(11).randrange(256) for _ in range(96 * 1024))
    device.model = "FM-1_015"
    device.model_after = "FM-1_016"
    device.expect_image(len(image))
    manifest = write_catalog(tmp_path, image)
    with launch(evidence) as app:
        until(app, CONNECTED)
        install(app, manifest)
        until(app, "result = window.fm1.flasher.state.name == 'DONE'", timeout=120)
        message = app.script("result = window.fm1.flasher.message")
        assert "FM-1_016" in message
    assert bytes(device.image) == image
    assert device.ota_state == "done"


def test_fm1_firmware_install_interrupted(
    device: virtual_fm1.VirtualFm1, evidence: pathlib.Path, tmp_path: pathlib.Path
) -> None:
    image = bytes(random.Random(12).randrange(256) for _ in range(96 * 1024))
    device.disconnect_at = 40 * 1024
    device.expect_image(len(image))
    manifest = write_catalog(tmp_path, image)
    with launch(evidence) as app:
        until(app, CONNECTED)
        install(app, manifest)
        until(app, "result = window.fm1.flasher.state.name == 'INTERRUPTED'", timeout=60)
        guide = until(app, "result = window.hardware.firmware.guide.text()", timeout=5)
        assert "adX does not retry" in guide
        served = app.script("result = window.fm1.flasher.server.served")
        time.sleep(3)
        assert app.script("result = window.fm1.flasher.server.served") == served  # no retry
    assert device.ota_state == "disconnected"


def test_fm1_external_part(device: virtual_fm1.VirtualFm1, evidence: pathlib.Path) -> None:
    """A part's notes, program, CCs and FX reach the device. (The audio return's latency
    is measured through a loopback stand-in: test_fm1_panels.py's ping test; a real USB
    return is the final review's.)"""
    with launch(evidence) as app:
        until(app, CONNECTED)
        app.trigger("view.hardware")
        app.script("window.hardware.tabs.setCurrentWidget(window.hardware.parts)")
        app.click("fm1.parts.add_channels")
        until(app, "result = len(window.fm1.part_channels()) == 8")
        # Part 2's program, bend range and level, through the table; an effect by name.
        app.script(
            "program, bend, level = window.hardware.parts.spins[1]\n"
            "program.setValue(9)\nbend.setValue(7)\nlevel.setValue(88)\n"
            "window.hardware.fx.sliders['REVERB_MIX'].setValue(70)"
        )
        # Notes on part 2, played by the transport.
        app.script(
            "from adx.scripting.api import project\n"
            "p = project()\n"
            "with p.undo_group('part 2 riff'):\n"
            "    riff = p.add_pattern('Riff', bars=1)\n"
            "    notes = riff.notes('FM-1 part 2')\n"
            "    for k, pitch in enumerate((60, 64, 67, 72)):\n"
            "        notes.add(pitch, at=k * 1920, length=960, velocity=90 + k)\n"
            "    p.playlist.track(1).place(riff, at='0:0:0')"
        )
        app.trigger("transport.play")
        deadline = time.monotonic() + 15
        while len([n for n in device.notes if n[0] == 1 and n[2] > 0]) < 4:
            assert time.monotonic() < deadline, f"notes did not arrive: {device.notes}"
            time.sleep(0.1)
        app.trigger("transport.stop")
    ons = [n for n in device.notes if n[0] == 1 and n[2] > 0]
    assert [n[1] for n in ons[:4]] == [60, 64, 67, 72]
    assert [n[2] for n in ons[:4]] == [90, 91, 92, 93]
    assert (1, 9) in device.programs
    assert device.bend_ranges.get(1) == 7
    assert (1, 7, 88) in device.ccs
    assert (1, 93, 70) in device.ccs  # REVERB_MIX on the effect channel (2)


def test_fm1_capture_sequencer(device: virtual_fm1.VirtualFm1, evidence: pathlib.Path) -> None:
    """adX sends clock; the device's pattern records as a take on the beat; a fixed
    velocity is flagged."""
    device.pattern = [(0, 60, 100), (4, 62, 100), (8, 64, 100), (12, 65, 100)]
    device.steps = 16
    with launch(evidence) as app:
        until(app, CONNECTED)
        name = app.script(
            "from adx.hardware import dx7\n"
            "result = dx7.open_as_channel(window.session, dx7.init_voice(), 'Capture')"
        )
        app.script(
            "from adx.selection import Kind\n"
            f"cid = [int(c['id']) for c in window.project.channels_info() "
            f"if c['name'] == {name!r}][0]\n"
            "window.selection.select(Kind.CHANNELS, [cid])\nwindow.live_midi.follow()\n"
            "window.fm1_follows(True)"
        )
        app.click("record_midi")
        app.trigger("transport.play")
        app.wait_for("transport.position_seconds", predicate="gt", value=8.2, timeout=20)
        app.trigger("transport.stop")
        app.click("record_midi")
        take = until(
            app,
            "c = [c for c in window.project.channels_info() if c['name'] == "
            f"{name!r}][0]\n"
            "ps = [p for p in window.project.patterns() if p != 'P1' and "
            f"len(window.project.notes(p, {name!r}))]\n"
            f"result = [[int(s), int(v), int(k)] for s, v, k in zip("
            f"window.project.notes(ps[-1], {name!r})['start'], "
            f"window.project.notes(ps[-1], {name!r})['velocity'], "
            f"window.project.notes(ps[-1], {name!r})['pitch'])] if ps else None",
        )
        flagged = app.script("result = window.fm1.probe.sends_velocity")
    starts = sorted(s for s, _v, _k in take)
    assert len(starts) >= 12, take
    ppq = 3840
    # Every note on a beat. Measured 2026-11-09 against the emulator: the whole take sits
    # about 90 ticks (11.7 ms at 120 bpm) late - adX's clock-out latency plus the MIDI
    # input's one block - and the onsets spread over about 11 ticks (1.4 ms), from the
    # sender thread's 1 ms loop and a Python device. The plan's 1 ms is the hardware's
    # to check (final review); compensating the constant part is STATE.md P11.5-2.
    offsets = [s % ppq if s % ppq < ppq / 2 else s % ppq - ppq for s in starts]
    shift = sorted(offsets)[len(offsets) // 2]
    spread = max(abs(o - shift) for o in offsets)
    (evidence / "capture.json").write_text(
        json.dumps({"starts": starts, "shift_ticks": shift, "spread_ticks": spread}), "utf-8"
    )
    assert spread <= 2 * 7.68, f"onsets wander {spread} ticks: {offsets}"  # 2 ms
    assert abs(shift) <= 20 * 7.68, f"the take sits {shift} ticks off the beat"  # 20 ms
    assert flagged is False  # 16 notes at velocity 100: "device sent fixed velocity"


SESSION_SET = """[PROJECT]
ADX_VERSION=2

[TEMPO]
0:0:0 120

[CHANNEL Lead]
INSTRUMENT=testtone
OUTPUT=insert.1

[CHANNEL Bass]
INSTRUMENT=testtone
OUTPUT=insert.1

[PATTERN A]
LENGTH=1:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100

[PATTERN B]
LENGTH=1:0:0
NOTES Bass
  C2 0:0:0 0:1:0 100

[PLAYLIST]
TRACK 1 name="Lead"
TRACK 2 name="Bass"

[MIXER]
INSERT 1 name="Master"

[SESSION]
QUANTIZE=1/4
SCENE "One"
SCENE "Two"
TRACK Lead linked=1
  1 A
  2 A
TRACK Bass linked=2
  1 B
"""


def test_fm1_keys_launch_and_light(
    device: virtual_fm1.VirtualFm1, evidence: pathlib.Path, tmp_path: pathlib.Path
) -> None:
    """With "Use keys as pads" the keys launch clips (key 48 is track 1, scene 1) and the
    V16 key LEDs (notes 0-26 on the effect channel) show what plays."""
    project = tmp_path / "set.adx"
    project.write_text(SESSION_SET, encoding="utf-8")
    with launch(evidence) as app:
        app.open(project)
        until(app, CONNECTED)
        app.script("window.fm1_keys_as_pads(True)")
        until(app, "result = bool(window.fm1.port_in) and window.fm1.port_in in window.pads.attached")
        app.trigger("transport.play")
        device.play([(48, 110)])  # Lead, scene 1
        until(app, "result = window.engine.clip_state(0, 0) == 'playing'", timeout=10)
        deadline = time.monotonic() + 5
        while not device.leds.get(0) and time.monotonic() < deadline:
            time.sleep(0.05)
        assert device.leds.get(0), f"key 1's LED is not lit: {device.leds}"
        assert not device.leds.get(1)  # Bass, scene 1: stopped
        app.trigger("transport.stop")
        app.script("window.fm1_keys_as_pads(False)")
        until(app, "result = window.fm1.port_in not in window.pads.attached")
