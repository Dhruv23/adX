"""The Hardware panel's tabs (phase_11.5.md 5): test_fm1_librarian, test_fm1_parts and
test_fm1_fx, through the real widgets, with the FM-1's port replaced by a recorder."""

from __future__ import annotations

import pathlib

import pytest

from adx.hardware import dx7
from adx.hardware.fm1 import profile
from adx.hardware.fm1.effects import as_params
from adx.mainwindow import MainWindow
from adx.selection import Kind

FIXTURES = pathlib.Path(__file__).resolve().parents[1] / "fixtures" / "dx7"


@pytest.fixture
def wire(studio: MainWindow, monkeypatch: pytest.MonkeyPatch) -> dict[str, list[bytes]]:
    """What the app sends the FM-1, with a V16 on the other end."""
    sent: dict[str, list[bytes]] = {"short": [], "sysex": []}
    monkeypatch.setattr(studio.fm1, "send", lambda m: sent["short"].append(bytes(m)))
    monkeypatch.setattr(studio.fm1, "send_sysex", lambda m, _gap=0: sent["sysex"].append(bytes(m)))
    studio.fm1.port_in = studio.fm1.port_out = "FM-1 Midi (test)"
    v16 = profile.for_model("FM-1_016")
    assert v16 is not None
    studio.fm1.use_profile(v16)
    return sent


def test_fm1_librarian(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    tab = studio.hardware.librarian
    source = FIXTURES / "random.syx"
    assert tab.import_file(source) == 32
    assert tab.list.count() == 32
    assert "RANDOM 00" in tab.list.item(0).text()
    # Browse and rename.
    tab.list.setCurrentRow(5)
    tab.rename("RENAMED")
    assert dx7.name_of(tab.voices[5]) == "RENAMED   "
    tab.rename("RANDOM 05")
    # Export is the source, byte for byte.
    out = tab.export(tmp_path / "back.syx")
    assert out.read_bytes() == source.read_bytes()
    # Open as a channel; it is the voice.
    name = tab.open_selected()
    cid = next(int(c["id"]) for c in studio.project.channels_info() if c["name"] == name)
    assert dx7.channel_voice(studio.session, cid) == tab.voices[5]
    # Audition opens and plays it.
    played: list[str] = []
    tab.audition_hook = played.append
    tab.audition()
    assert len(played) == 1
    # A single voice exported is that voice.
    one = tab.export(tmp_path / "one.syx", only_selected=True)
    assert dx7.read_file(one)["voices"] == [tab.voices[5]]


def test_librarian_bad_checksum_asks(studio: MainWindow, monkeypatch: pytest.MonkeyPatch) -> None:
    tab = studio.hardware.librarian
    monkeypatch.setattr(tab, "_open_anyway", lambda _bad: False)
    assert tab.import_file(FIXTURES / "bad_checksum.syx") == 0
    monkeypatch.setattr(tab, "_open_anyway", lambda _bad: True)
    assert tab.import_file(FIXTURES / "bad_checksum.syx") == 32
    assert "bad checksum" in tab.status.text()
    assert tab.import_file(FIXTURES / "sloopdx_256.syx") == 256


def test_librarian_receives_a_dump(studio: MainWindow, wire: dict[str, list[bytes]]) -> None:
    tab = studio.hardware.librarian
    tab.receive()
    assert wire["sysex"][-1] == bytes([0xF0, 0x43, 0x20, 0x09, 0xF7])
    # Unanswered: after the wait it asks for Bank Export.
    studio.fm1._dump_at -= 10
    studio.fm1.tick()
    assert studio.fm1.probe.answers_dump is False
    assert "Bank Export" in tab.status.text()
    # The export arrives.
    bank = (FIXTURES / "algorithms.syx").read_bytes()
    studio.fm1.dispatch("FM-1 Midi (test)", bank)
    assert tab.list.count() == 32
    assert "ALG 01" in tab.list.item(0).text()
    # Sent back: a voice and a bank, and the slots are named.
    tab.list.setCurrentRow(2)
    tab.send_voice()
    assert len(wire["sysex"][-1]) == 163
    tab.send_bank()
    assert wire["sysex"][-1] == bank
    assert studio.fm1.device is not None
    assert studio.fm1.device.slot_names[0] == "ALG 01    "


def test_fm1_parts(studio: MainWindow, wire: dict[str, list[bytes]]) -> None:
    tab = studio.hardware.parts
    assert tab.table.rowCount() == 8
    assert [p.channel for p in studio.fm1.device.parts] == list(range(1, 9))  # type: ignore[union-attr]
    wire["short"].clear()
    # Part 4: program 7, bend range 12, level 90 - on channel 4.
    program, bend, level = tab.spins[3]
    program.setValue(7)
    bend.setValue(12)
    level.setValue(90)
    assert bytes([0xC3, 7]) in wire["short"]
    assert bytes([0xB3, 6, 12]) in wire["short"]
    assert bytes([0xB3, 7, 90]) in wire["short"]
    # The snapshot carries them.
    wire["short"].clear()
    count = tab.send_snapshot()
    assert count == len(wire["short"])
    assert bytes([0xC3, 7]) in wire["short"]
    # The latency readout.
    tab.show_latency(480, 48000)
    assert "10.00 ms" in tab.latency.text()


def test_fm1_fx(studio: MainWindow, wire: dict[str, list[bytes]]) -> None:
    tab = studio.hardware.fx
    v16 = studio.fm1.profile
    assert v16 is not None
    assert list(tab.sliders) == v16.fx_names()
    wire["short"].clear()
    for index, (name, slider) in enumerate(tab.sliders.items()):
        slider.setValue(10 + index)
        assert wire["short"][-1] == bytes([0xB1, v16.fx_cc[name], 10 + index]), name
    # Match in adX: an Fm1Effects on the dx7 channel's insert, with the same values.
    voice = dx7.read_file(FIXTURES / "random.syx")["voices"][0]
    channel = dx7.open_as_channel(studio.session, voice)
    info = next(c for c in studio.project.channels_info() if c["name"] == channel)
    studio.selection.select(Kind.CHANNELS, [int(info["id"])])
    assert tab.match()
    insert = int(info["output"])
    inserts = studio.project.mixer_info()["inserts"]
    chain = next(i for i in inserts if int(i["id"]) == insert)["slots"]
    fm1 = chain[-1]
    assert fm1["type"] == "Fm1Effects"
    params = {str(k): float(v) for k, v in fm1["params"]}
    assert as_params(studio.fm1.device.fx).items() <= params.items()  # type: ignore[union-attr]


def test_fm1_part_channels_and_ping(
    studio: MainWindow, wire: dict[str, list[bytes]], monkeypatch: pytest.MonkeyPatch
) -> None:
    """8 parts become 8 midiout channels on channels 1-8 playing into the FM-1's port, in
    one undo step; ping measures a known offset in a loopback."""
    from adx.engine_bridge import Engine

    monkeypatch.setattr(studio.fm1, "port_slot", lambda: 2)
    history = len(studio.project.history())
    made = studio.fm1.add_part_channels()
    assert made == [f"FM-1 part {n}" for n in range(1, 9)]
    assert len(studio.project.history()) == history + 1
    parts = studio.fm1.part_channels()
    assert sorted(parts) == list(range(1, 9))
    for part, cid in parts.items():
        info = next(c for c in studio.project.channels_info() if int(c["id"]) == cid)
        assert info["instrument"] == "midiout"
        held = {str(p[0]): float(p[1]) for p in info["params"]}
        assert held["port"] == 2.0
        assert held["channel"] == float(part)
    assert studio.fm1.add_part_channels() == []  # once
    # The device reconnects on another slot: the channels follow.
    monkeypatch.setattr(studio.fm1, "port_slot", lambda: 1)
    studio.fm1.point_part_channels()
    cid = studio.fm1.part_channels()[3]
    info = next(c for c in studio.project.channels_info() if int(c["id"]) == cid)
    assert {str(p[0]): float(p[1]) for p in info["params"]}["port"] == 1.0
    # Ping, through a loopback stand-in 777 samples long.
    loop = Engine(loopback=777, input_channels=2)
    loop.set_project(studio.project)
    loop.start()
    try:
        assert studio.fm1.ping(loop) == 777
    finally:
        loop.stop()
    assert studio.fm1.latency[48000] == 777
