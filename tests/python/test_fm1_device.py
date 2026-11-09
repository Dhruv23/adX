"""The FM-1's protocol layer (phase_11.5.md 5): fm1_identity, fm1_profiles,
fm1_device_snapshot, fm1_live_edit_rate_limit, ota_against_mock, flasher_gates,
bootloader_detect, and the ping measurement test_fm1_parts uses.

Headless and device-free: the device side is played by the test itself, with the same
encoders tools/virtual_fm1.py uses.
"""

from __future__ import annotations

import hashlib
import random
from dataclasses import dataclass, field

import numpy as np
import pytest

from adx.hardware.fm1 import identity, ota, profile
from adx.hardware.fm1.device import (
    FIXED_VELOCITY_RUN,
    Fm1Device,
    LiveEdit,
    VelocityWatch,
    ping_latency,
)
from adx.hardware.fm1.firmware import FirmwarePackage
from adx.hardware.fm1.flasher import Flasher, State
from adx.hardware.fm1.recovery import DeviceState, classify, guide

#: The FX names in CC order, entered by hand (phase_11.5.md 4.9: a typo in a profile
#: fails here). The order is adX's reading of M-VAVE's CC page, unverified until the
#: final review; this table and the profile files change together.
FX_ORDER = [
    "FILTER_TYPE", "FILTER_CUTOFF", "FILTER_RESO", "DIST_DRIVE", "DIST_TONE", "DIST_MIX",
    "CHORUS_RATE", "CHORUS_DEPTH", "CHORUS_MIX", "PHASER_RATE", "PHASER_DEPTH",
    "PHASER_FEEDBACK", "PHASER_MIX", "DELAY_TIME", "DELAY_FEEDBACK", "DELAY_TONE",
    "DELAY_SYNC", "DELAY_MIX", "REVERB_TYPE", "REVERB_SIZE", "REVERB_DAMPING",
    "REVERB_PREDELAY", "REVERB_WIDTH", "REVERB_MIX",
]  # fmt: skip
GUIDE = {"FM-1_014": (0, 1, False), "FM-1_015": (0, 1, False), "FM-1_016": (70, 8, True),
         "FM-1_OPEN": (70, 8, True)}  # fmt: skip


def test_fm1_profiles() -> None:
    shipped = {p.match_model: p for p in profile.shipped()}
    assert set(shipped) == set(GUIDE)
    for model, (base, parts, leds) in GUIDE.items():
        p = shipped[model]
        assert p.fx_names() == FX_ORDER
        assert [p.fx_cc[n] for n in FX_ORDER] == list(range(base, base + 24))
        assert p.parts == parts
        assert p.part_channels == list(range(1, parts + 1))
        assert p.effect_channel == 2
        assert (p.led_notes == range(27)) is leds
        assert p.part_cc["MOD"] == 1
        assert p.part_cc["SUSTAIN"] == 64
        assert p.match_device == "FM-1 Midi"
    assert shipped["FM-1_016"].part_cc["VOLUME"] == 7
    assert "VOLUME" not in shipped["FM-1_015"].part_cc
    # A broken file says where.
    with pytest.raises(profile.ProfileError, match="line 4"):
        profile.parse('[DEVICE_PROFILE]\nNAME="x"\n[FX_CC]\nA=200\n')
    with pytest.raises(profile.ProfileError, match="twice"):
        profile.parse(
            '[DEVICE_PROFILE]\nNAME="x"\nMATCH_DEVICE="d"\nMATCH_MODEL="m"\n[FX_CC]\nA=1 B=1\n'
        )


def test_fm1_identity() -> None:
    for model, generation in (("FM-1_014", 14), ("FM-1_015", 15), ("FM-1_016", 16)):
        found = identity.parse_reply(identity.reply_for(model))
        assert found == identity.Identity(model, generation)
        assert identity.fallback_generation(found) == generation
        assert profile.for_model(model) is not None
    odd = identity.parse_reply(identity.reply_for("FM-1_SKIFF"))
    assert odd is not None
    assert odd.generation is None
    assert not odd.known
    assert profile.for_model(odd.model) is None
    # No reply (or the query echoed back): V15, which the user can change.
    assert identity.parse_reply(identity.IDENTITY_QUERY) is None
    assert identity.fallback_generation(None) == 15
    assert not identity.parse_reply(identity.reply_for("FM-1_016")).emulated  # type: ignore[union-attr]
    assert identity.parse_reply(identity.reply_for("FM-1_016", emulated=True)).emulated  # type: ignore[union-attr]
    assert identity.is_fm1_port("FM-1 Midi 1")
    assert identity.is_ota_port("ota-FM-1 3")


@dataclass
class Sent:
    short: list[bytes] = field(default_factory=list)
    sysex: list[bytes] = field(default_factory=list)

    def send(self, message: bytes) -> None:
        self.short.append(message)

    def send_sysex(self, message: bytes, _gap: int) -> None:
        self.sysex.append(message)


def test_fm1_device_snapshot() -> None:
    v16 = profile.for_model("FM-1_016")
    assert v16 is not None
    out = Sent()
    device = Fm1Device(v16, out.send, out.send_sysex)
    device.parts[0].program = 3
    device.parts[2].bend_range = 12
    device.fx["REVERB_MIX"] = 90
    first = device.snapshot()
    assert first == device.snapshot()  # a fixed order
    # Part 1: program, RPN 0 bend range, volume; ... part 8; then 24 FX CCs on channel 2.
    assert first[0] == bytes([0xC0, 3])
    assert first[1:7] == [
        bytes([0xB0, 101, 0]), bytes([0xB0, 100, 0]), bytes([0xB0, 6, 2]),
        bytes([0xB0, 38, 0]), bytes([0xB0, 101, 127]), bytes([0xB0, 100, 127]),
    ]  # fmt: skip
    assert bytes([0xB2, 6, 12]) in first
    fx = first[-24:]
    assert [m[0] for m in fx] == [0xB1] * 24
    assert [m[1] for m in fx] == list(range(70, 94))
    assert fx[-1] == bytes([0xB1, 93, 90])
    assert device.send_snapshot() == len(first)
    assert out.short == first
    # By name, with the state kept.
    assert device.set_fx("FILTER_CUTOFF", 200) == bytes([0xB1, 71, 127])
    assert device.fx["FILTER_CUTOFF"] == 127
    assert device.part_cc(1, "EXPRESSION", 50) == bytes([0xB1, 11, 50])
    # LEDs: on/off notes on the effect channel, keys 0-26 only.
    assert device.leds({0: True, 26: False, 40: True}) == [
        bytes([0x91, 0, 127]),
        bytes([0x81, 26, 0]),
    ]
    v15 = profile.for_model("FM-1_015")
    assert v15 is not None
    assert Fm1Device(v15, out.send, out.send_sysex).leds({0: True}) == []


def test_fm1_live_edit_rate_limit() -> None:
    now = [0.0]
    out = Sent()
    edit = LiveEdit(out.send_sysex, clock=lambda: now[0])
    # A 1 kHz sweep of one knob for two seconds, and a second knob at the same time.
    for i in range(2000):
        now[0] = i / 1000.0
        edit.change(16, i % 100)  # op6 output level
        edit.change(134, i % 32)  # algorithm
        edit.flush()
    now[0] = 2.0
    edit.flush(force=True)
    by_param: dict[int, list[int]] = {}
    for message in out.sysex:
        param = (message[3] & 3) << 7 | message[4]
        by_param.setdefault(param, []).append(message[5])
    for param, values in by_param.items():
        assert len(values) <= 2 * 50 + 1, param
        assert len(values) >= 2 * 50 - 1, param
    assert by_param[16][-1] == 1999 % 100
    assert by_param[134][-1] == 1999 % 32


def test_velocity_watch() -> None:
    watch = VelocityWatch()
    for _ in range(FIXED_VELOCITY_RUN - 1):
        assert not watch.note(100)
    assert watch.note(100)
    real = VelocityWatch()
    assert not any(real.note(v) for v in [100, 90] * 20)


def test_ping_measures_a_known_offset() -> None:
    rng = np.random.default_rng(1)
    capture = rng.normal(0, 0.001, 48000).astype(np.float32)
    capture[1000 + 517 :] += 0.3 * np.sin(np.arange(48000 - 1517) * 0.1).astype(np.float32)
    capture[1517] = 0.3
    assert ping_latency(capture, 1000) == 517
    assert ping_latency(np.zeros(100, np.float32), 10) is None


def test_ota_against_mock() -> None:
    rng = random.Random(5)
    image = bytes(rng.randrange(256) for _ in range(1000))
    assert ota.unpack7(ota.pack7(image), len(image)) == image
    assert all(b < 0x80 for b in ota.pack7(image))
    server = ota.ImageServer(image)
    got = bytearray()
    for address in range(0, len(image), 256):
        request = ota.ReadRequest(1, address, min(256, len(image) - address))
        assert ota.parse_request(ota.encode_request(request)) == request
        reply = server.answer(request)
        assert reply is not None
        parsed = ota.parse_reply(reply)
        assert parsed is not None
        answered, data, ok = parsed
        assert ok
        assert answered.address == address
        assert data == image[address : address + request.length]
        got += data
    assert bytes(got) == image
    assert server.progress == 1.0
    # Outside the image: refused, nothing sent.
    assert server.answer(ota.ReadRequest(1, 990, 20)) is None
    assert server.refused == 1
    # A corrupted reply fails its checksum.
    reply = bytearray(server.answer(ota.ReadRequest(1, 0, 16)) or b"")
    reply[20] ^= 0x01
    corrupted = ota.parse_reply(bytes(reply))
    assert corrupted is not None
    assert not corrupted[2]
    assert server.answer(ota.ReadRequest(1, ota.COMPLETE_ADDRESS, 0)) is None
    assert server.done


@dataclass
class FakeLink:
    names: list[str] = field(default_factory=lambda: ["FM-1 Midi 1"])
    sent: list[tuple[str, bytes]] = field(default_factory=list)
    opened: list[str] = field(default_factory=list)

    def ports(self) -> list[str]:
        return list(self.names)

    def open(self, port: str) -> bool:
        self.opened.append(port)
        return True

    def send(self, port: str, message: bytes) -> None:
        self.sent.append((port, message))


def package_for(data: bytes, name: str = "Skiff") -> FirmwarePackage:
    return FirmwarePackage(
        name=name, version="1.0", source="test", url="file://x",
        sha256=hashlib.sha256(data).hexdigest(), license="MIT", open_source=True,
    )  # fmt: skip


IMAGE = bytes(random.Random(9).randrange(256) for _ in range(70000))
V15 = identity.Identity("FM-1_015", 15)


def test_flasher_gates() -> None:
    now = [0.0]
    # A bad hash: never offered.
    flasher = Flasher(FakeLink(), emulated=True, clock=lambda: now[0])
    bad = package_for(b"other")
    assert not flasher.verify(bad, IMAGE)
    assert flasher.state is State.REFUSED
    # A closed package is a link card.
    closed = FirmwarePackage(name="FM-1+VA", version="2", source="hub", page="https://x")
    assert not closed.installable
    assert not Flasher(FakeLink(), emulated=True).verify(closed, IMAGE)
    # No typed confirmation: no enter-upgrade.
    link = FakeLink()
    flasher = Flasher(link, emulated=True, clock=lambda: now[0])
    assert flasher.verify(package_for(IMAGE), IMAGE)
    assert "Skiff" in flasher.warning()
    assert not flasher.confirm("skiff")
    assert not flasher.start("FM-1 Midi 1", V15)
    assert link.sent == []
    # A real device is never written while the protocol is unverified.
    real = Flasher(FakeLink(), emulated=False, clock=lambda: now[0])
    assert real.verify(package_for(IMAGE), IMAGE)
    assert real.confirm("Skiff")
    assert not ota.PROTOCOL_VERIFIED
    assert not real.start("FM-1 Midi 1", V15)
    assert real.link.sent == []  # type: ignore[attr-defined]
    assert "real FM-1" in real.message
    # Typed exactly: enter-upgrade goes out.
    assert flasher.verify(package_for(IMAGE), IMAGE)
    assert flasher.confirm("Skiff")
    assert flasher.start("FM-1 Midi 1", V15)
    assert link.sent == [("FM-1 Midi 1", ota.ENTER_UPGRADE)]


def test_flasher_full_and_interrupted() -> None:
    now = [0.0]
    link = FakeLink()
    flasher = Flasher(link, emulated=True, clock=lambda: now[0])
    assert flasher.verify(package_for(IMAGE), IMAGE)
    assert flasher.confirm("Skiff")
    assert flasher.start("FM-1 Midi 1", V15)
    # The device re-enumerates and pulls the image.
    link.names = ["ota-FM-1 1"]
    flasher.poll()
    assert "ota-FM-1 1" in link.opened
    for address in range(0, len(IMAGE), 256):
        now[0] += 0.01
        flasher.on_sysex(
            "ota-FM-1 1",
            ota.encode_request(
                ota.ReadRequest(1, address, 256)
                if address + 256 <= len(IMAGE)
                else ota.ReadRequest(1, address, len(IMAGE) - address)
            ),
        )
    assert flasher.state is State.SERVING
    flasher.on_sysex("ota-FM-1 1", ota.encode_request(ota.ReadRequest(1, ota.COMPLETE_ADDRESS, 0)))
    assert flasher.state is State.REBOOTING
    served = b"".join(
        data
        for port, m in link.sent
        if (parsed := ota.parse_reply(m))
        for *_r, data, _ok in [parsed]
    )
    assert served == IMAGE
    link.names = ["FM-1 Midi 1"]
    flasher.poll()
    assert link.sent[-1] == ("FM-1 Midi 1", identity.IDENTITY_QUERY)
    flasher.on_sysex("FM-1 Midi 1", identity.reply_for("FM-1_016"))
    assert flasher.state is State.DONE
    assert "FM-1_016" in flasher.message

    # Interrupted: the device vanishes mid-transfer. Stop, no retry, the guide.
    link = FakeLink()
    flasher = Flasher(link, emulated=True, clock=lambda: now[0])
    assert flasher.verify(package_for(IMAGE), IMAGE)
    assert flasher.confirm("Skiff")
    assert flasher.start("FM-1 Midi 1", V15)
    link.names = ["ota-FM-1 1"]
    flasher.poll()
    flasher.on_sysex("ota-FM-1 1", ota.encode_request(ota.ReadRequest(1, 0, 256)))
    sent = len(link.sent)
    link.names = []
    flasher.poll()
    assert flasher.state is State.INTERRUPTED
    assert flasher.recovery_shown
    assert "Nothing more will be sent" in flasher.message
    for _ in range(5):
        now[0] += 10
        flasher.poll()
        flasher.on_sysex("ota-FM-1 1", ota.encode_request(ota.ReadRequest(1, 256, 256)))
    assert len(link.sent) == sent
    # The device stops asking (a failed checksum on its side): the same.
    link = FakeLink()
    flasher = Flasher(link, emulated=True, clock=lambda: now[0])
    assert flasher.verify(package_for(IMAGE), IMAGE)
    assert flasher.confirm("Skiff")
    assert flasher.start("FM-1 Midi 1", V15)
    link.names = ["ota-FM-1 1"]
    flasher.poll()
    flasher.on_sysex("ota-FM-1 1", ota.encode_request(ota.ReadRequest(1, 0, 256)))
    now[0] += 6
    flasher.poll()
    assert flasher.state is State.INTERRUPTED


def test_bootloader_detect() -> None:
    assert classify([], [identity.USB_UBOOT], False) is DeviceState.UBOOT
    assert classify(["ota-FM-1 1"], [], False) is DeviceState.OTA_STRANDED
    assert classify(["ota-FM-1 1"], [], True) is DeviceState.OTA
    assert classify(["FM-1 Midi 2"], [], False) is DeviceState.NORMAL
    assert classify([], [], False) is DeviceState.ABSENT
    # Both route to guidance, never to the flasher: and the guide never offers a write.
    for state in (DeviceState.UBOOT, DeviceState.OTA_STRANDED):
        text = guide(state)
        assert text
        assert "adX does not" in text
