"""The connected FM-1 (phase_11.5.md 4.6): parts, the effects state adX owns, the
connect snapshot, live editing, key LEDs, and what the device plays back.

It never reads state from the device, because the device reports none: no CCs, no
effects in its dumps. adX holds the FX state, sends it whole on connect (and project
load, and transport start), and says plainly that a knob turned on the device is not
seen.
"""

from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass, field

import numpy as np
import numpy.typing as npt

from adx import engine_bridge
from adx.hardware.fm1.profile import DeviceProfile

#: The DX7 parameter-change rate the device is sent at most, per parameter.
LIVE_EDIT_RATE = 50.0
#: Consecutive notes at one velocity before a take is marked "fixed velocity".
FIXED_VELOCITY_RUN = 16
#: What the device's FX start at when adX has never set them: the chain's own "nothing
#: happens" setting (the filter open, every mix 0), the same as Fm1Effects' defaults.
FX_DEFAULTS = {
    "FILTER_CUTOFF": 127, "DIST_DRIVE": 32, "DIST_TONE": 127, "CHORUS_RATE": 32,
    "CHORUS_DEPTH": 64, "PHASER_RATE": 32, "PHASER_DEPTH": 64, "PHASER_FEEDBACK": 64,
    "DELAY_TIME": 48, "DELAY_FEEDBACK": 48, "DELAY_TONE": 100, "REVERB_SIZE": 64,
    "REVERB_DAMPING": 64, "REVERB_WIDTH": 127,
}  # fmt: skip


@dataclass
class Part:
    """One of the V16's eight parts (or the only one before V16)."""

    channel: int
    program: int | None = None
    bend_range: int = 2
    #: CC 7 and CC 10 values; glide and tuning by the CCs the profile names, if any.
    level: int = 100
    pan: int = 64
    #: Marks the part whose notes the device arpeggiates and sends back (4.6).
    arp_source: bool = False


@dataclass
class Fm1Device:
    profile: DeviceProfile
    #: One short message to the device's output.
    send: Callable[[bytes], None]
    #: One SysEx message, with the gap (ms) to leave after the previous one.
    send_sysex: Callable[[bytes, int], None]
    #: Milliseconds between SysEx messages: 0 over USB, about 10 over BLE and TRS.
    sysex_gap_ms: int = 0
    fx: dict[str, int] = field(default_factory=dict)
    parts: list[Part] = field(default_factory=list)
    #: What adX last sent to each program slot: slot -> voice name.
    slot_names: dict[int, str] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if not self.parts:
            self.parts = [Part(channel=c) for c in self.profile.part_channels]
        for name in self.profile.fx_names():
            self.fx.setdefault(name, FX_DEFAULTS.get(name, 0))

    # --- the snapshot ---------------------------------------------------------------

    def snapshot(self) -> list[bytes]:
        """Everything adX owns, in a fixed order: each part's program and bend range,
        then every effect CC on the effect channel, in CC order."""
        out: list[bytes] = []
        for part in self.parts:
            ch = (part.channel - 1) & 0x0F
            if part.program is not None:
                out.append(bytes([0xC0 | ch, part.program & 0x7F]))
            out += rpn_bend_range(part.channel, part.bend_range)
            if "VOLUME" in self.profile.part_cc:
                out.append(bytes([0xB0 | ch, self.profile.part_cc["VOLUME"], part.level & 0x7F]))
        fx_ch = (self.profile.effect_channel - 1) & 0x0F
        for name in self.profile.fx_names():
            out.append(bytes([0xB0 | fx_ch, self.profile.fx_cc[name], self.fx[name] & 0x7F]))
        return out

    def send_snapshot(self) -> int:
        messages = self.snapshot()
        for message in messages:
            self.send(message)
        return len(messages)

    # --- effects and parts ----------------------------------------------------------

    def set_fx(self, name: str, value: int) -> bytes:
        """Set an effect by name: adX's state, and its CC on the effect channel."""
        cc = self.profile.fx_cc[name]
        self.fx[name] = max(0, min(127, int(value)))
        message = bytes([0xB0 | ((self.profile.effect_channel - 1) & 0x0F), cc, self.fx[name]])
        self.send(message)
        return message

    def part_cc(self, part: int, name: str, value: int) -> bytes | None:
        """A part CC by the profile's name (MOD, BREATH, FOOT, VOLUME, EXPRESSION,
        SUSTAIN); None when this generation has no such CC."""
        cc = self.profile.part_cc.get(name)
        if cc is None:
            return None
        message = bytes([0xB0 | ((self.parts[part].channel - 1) & 0x0F), cc, value & 0x7F])
        self.send(message)
        return message

    def program(self, part: int, slot: int) -> None:
        self.parts[part].program = slot
        self.send(bytes([0xC0 | ((self.parts[part].channel - 1) & 0x0F), slot & 0x7F]))

    def bend_range(self, part: int, semitones: int) -> None:
        top = self.profile.bend_range_max
        self.parts[part].bend_range = max(0, min(top, semitones))
        for message in rpn_bend_range(self.parts[part].channel, self.parts[part].bend_range):
            self.send(message)

    # --- voices ---------------------------------------------------------------------

    def send_voice(self, voice: list[int], name: str = "") -> None:
        """A single-voice dump: the device's edit buffer."""
        self.send_sysex(engine_bridge.dx7_single_sysex(voice), self.sysex_gap_ms)
        if name:
            self.slot_names[-1] = name

    def send_bank(self, voices: list[list[int]], names: list[str] | None = None) -> None:
        self.send_sysex(engine_bridge.dx7_bank_sysex(voices), self.sysex_gap_ms)
        for slot, name in enumerate(names or []):
            self.slot_names[slot] = name

    def request_dump(self, bank: bool = True) -> None:
        self.send_sysex(engine_bridge.dx7_dump_request(bank), self.sysex_gap_ms)

    # --- LEDs -----------------------------------------------------------------------

    def leds(self, lit: dict[int, bool]) -> list[bytes]:
        """Key LEDs on V16: note on (lit) or off for keys 0-26 on the effect channel."""
        if self.profile.led_notes is None:
            return []
        ch = (self.profile.effect_channel - 1) & 0x0F
        out = []
        for key, on in sorted(lit.items()):
            if key in self.profile.led_notes:
                out.append(bytes([(0x90 if on else 0x80) | ch, key, 127 if on else 0]))
        for message in out:
            self.send(message)
        return out


def rpn_bend_range(channel: int, semitones: int) -> list[bytes]:
    """RPN 0 (pitch-bend sensitivity) on a channel, then the null RPN."""
    ch = (channel - 1) & 0x0F
    b = 0xB0 | ch
    return [
        bytes([b, 101, 0]),
        bytes([b, 100, 0]),
        bytes([b, 6, max(0, min(127, semitones))]),
        bytes([b, 38, 0]),
        bytes([b, 101, 127]),
        bytes([b, 100, 127]),
    ]


@dataclass
class LiveEdit:
    """dx7 parameter edits sent to the device as DX7 parameter changes, at most
    LIVE_EDIT_RATE per second per parameter, the last value always sent (4.6)."""

    send_sysex: Callable[[bytes, int], None]
    clock: Callable[[], float] = time.monotonic
    rate: float = LIVE_EDIT_RATE
    channel: int = 0
    _last: dict[int, float] = field(default_factory=dict)
    _pending: dict[int, int] = field(default_factory=dict)
    sent: int = 0

    def change(self, param: int, value: int) -> None:
        now = self.clock()
        if now - self._last.get(param, -1e9) >= 1.0 / self.rate:
            self._send(param, value, now)
            self._pending.pop(param, None)
        else:
            self._pending[param] = value

    def flush(self, force: bool = False) -> None:
        """Send pending values whose interval has passed (all of them with ``force``)."""
        now = self.clock()
        for param, value in list(self._pending.items()):
            if force or now - self._last.get(param, -1e9) >= 1.0 / self.rate:
                self._send(param, value, now)
                del self._pending[param]

    def _send(self, param: int, value: int, now: float) -> None:
        message = engine_bridge.dx7_param_change(param, value, self.channel)
        if message is not None:
            self.send_sysex(message, 0)
            self.sent += 1
        self._last[param] = now


@dataclass
class VelocityWatch:
    """Flags a take whose notes all carry one velocity (the device may send a fixed one)."""

    run: int = 0
    last: int | None = None
    fixed: bool = False

    def note(self, velocity: int) -> bool:
        self.run = self.run + 1 if velocity == self.last else 1
        self.last = velocity
        if self.run >= FIXED_VELOCITY_RUN:
            self.fixed = True
        return self.fixed


def ping_latency(
    capture: npt.NDArray[np.floating], sent_at: int, threshold: float = 0.01
) -> int | None:
    """Samples from a ping note sent at frame ``sent_at`` to its onset in the return
    audio (mono or the first channel of interleaved stereo); None when nothing came back."""
    data = np.asarray(capture, dtype=np.float64)
    if data.ndim == 2:
        data = data[:, 0]
    after = np.abs(data[sent_at:])
    hits = np.flatnonzero(after > threshold)
    return int(hits[0]) if len(hits) else None
