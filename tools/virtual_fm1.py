"""The emulated M-Vave FM-1 (phase_11.5.md 4.12): a virtual MIDI port named "FM-1 Midi",
so adX finds it exactly as it finds the device.

    py -3.12 tools/virtual_fm1.py --model FM-1_016 [--answers-dump] [--seconds 60]

It runs on teVirtualMIDI, the driver loopMIDI installs (teVirtualMIDI64.dll): the port
is a device's, not a loopback, so what adX sends is not echoed back to it, and the port
can disappear and come back under another name the way the FM-1 re-enumerates for an
update. Tests import :class:`VirtualFm1` and drive it in-process.

What it does:

- answers the vendor identity query with its model (14, 15, 16, or unknown);
- answers DX7 dump requests (only with ``answers_dump``, so both probe outcomes are
  tested) and sends a Bank Export on command;
- applies single voices, banks and parameter changes, keeping its edit buffer and bank
  so a test can read them back; records CCs, program changes, bend range (RPN 0), LED
  notes and notes;
- plays a step pattern on channel 1 when it is sent clock and start (6 clocks a step);
- runs the OTA device side: after enter-upgrade it closes "FM-1 Midi", opens
  "ota-FM-1", pulls the image in 256-byte reads, checks every checksum, and comes back
  as "FM-1 Midi" with a new model; or it disconnects at a chosen address, or stops at
  one as if a checksum failed.

Not covered: USB itself (re-enumeration here is a port closing and another opening),
audio, Bluetooth, and a real flash.
"""

from __future__ import annotations

import argparse
import ctypes
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))

from adx import engine_bridge  # noqa: E402
from adx.hardware.fm1 import identity, ota  # noqa: E402

FIXTURES = ROOT / "tests" / "fixtures" / "dx7"
_FLAGS = 0x1 | 0xC  # parse what is received; both directions

_CALLBACK = (
    ctypes.WINFUNCTYPE(
        None, ctypes.c_void_p, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_uint32, ctypes.c_size_t
    )
    if sys.platform == "win32"
    else None
)


class VirtualMidiError(RuntimeError):
    pass


def _driver() -> ctypes.WinDLL:
    if sys.platform != "win32":
        raise VirtualMidiError("the virtual FM-1 needs Windows and teVirtualMIDI")
    try:
        dll = ctypes.WinDLL("teVirtualMIDI64.dll")
    except OSError as error:
        raise VirtualMidiError("teVirtualMIDI is not installed (install loopMIDI)") from error
    dll.virtualMIDICreatePortEx2.restype = ctypes.c_void_p
    dll.virtualMIDICreatePortEx2.argtypes = [
        ctypes.c_wchar_p,
        _CALLBACK,
        ctypes.c_size_t,
        ctypes.c_uint32,
        ctypes.c_uint32,
    ]
    dll.virtualMIDISendData.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_ubyte),
        ctypes.c_uint32,
    ]
    dll.virtualMIDISendData.restype = ctypes.c_bool
    dll.virtualMIDIClosePort.argtypes = [ctypes.c_void_p]
    return dll


def available() -> bool:
    try:
        _driver()
    except VirtualMidiError:
        return False
    return True


class _Port:
    def __init__(self, name: str, on_data: object) -> None:
        self.dll = _driver()
        self.name = name
        self._cb = _CALLBACK(on_data)  # kept: the driver calls it from its own thread
        self.handle = self.dll.virtualMIDICreatePortEx2(name, self._cb, 0, 65535, _FLAGS)
        if not self.handle:
            raise VirtualMidiError(f"could not create the virtual port {name!r}")

    def send(self, data: bytes) -> None:
        buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        if not self.dll.virtualMIDISendData(self.handle, buf, len(data)):
            raise VirtualMidiError(f"sending to {self.name!r} failed")

    def close(self) -> None:
        if self.handle:
            self.dll.virtualMIDIClosePort(self.handle)
            self.handle = None


@dataclass
class VirtualFm1:
    model: str = "FM-1_016"
    answers_dump: bool = False
    #: The step pattern played on clock: (step, note, velocity), 16 steps a bar.
    pattern: list[tuple[int, int, int]] = field(default_factory=list)
    steps: int = 64
    #: OTA: after the transfer the device reports this model.
    model_after: str = ""
    disconnect_at: int | None = None
    bad_checksum_at: int | None = None
    ota_block: int = 256

    def __post_init__(self) -> None:
        self.lock = threading.Lock()
        self.bank: list[list[int]] = engine_bridge.dx7_read((FIXTURES / "random.syx").read_bytes())[
            "voices"
        ]
        self.edit = list(self.bank[0])
        self.received: list[tuple[float, bytes]] = []
        self.ccs: list[tuple[int, int, int]] = []
        self.programs: list[tuple[int, int]] = []
        self.notes: list[tuple[int, int, int]] = []
        self.leds: dict[int, bool] = {}
        self.bend_ranges: dict[int, int] = {}
        self.param_changes = 0
        self._rpn: dict[int, tuple[int, int]] = {}
        self._clock = 0
        self._running = False
        self.image = bytearray()
        self.ota_state = "idle"
        self._ota_next = 0
        self._ota_size = 0
        self._port: _Port | None = None
        self.open("FM-1 Midi")

    # --- ports ----------------------------------------------------------------------

    def open(self, name: str) -> None:
        self._port = _Port(name, self._on_data)

    def close(self) -> None:
        if self._port is not None:
            self._port.close()
            self._port = None

    @property
    def port_name(self) -> str:
        return self._port.name if self._port else ""

    def send(self, data: bytes) -> None:
        if self._port is not None:
            self._port.send(data)

    # --- what the device sends of its own ------------------------------------------

    def bank_export(self) -> None:
        """The V16 Bank Export button: the bank as a DX7 32-voice dump."""
        self.send(engine_bridge.dx7_bank_sysex(self.bank))

    def play(self, notes: list[tuple[int, int]], gap: float = 0.05) -> None:
        """Keys pressed: (note, velocity) on channel 1, ``gap`` seconds apart."""
        for note, velocity in notes:
            self.send(bytes([0x90, note, velocity]))
            time.sleep(gap)
            self.send(bytes([0x80, note, 0]))

    # --- what it receives ------------------------------------------------------------

    def _on_data(self, _port: object, data: object, length: int, _instance: int) -> None:
        if not data or not length:
            return
        message = bytes(data[:length])  # type: ignore[index]
        with self.lock:
            self.received.append((time.perf_counter(), message))
        try:
            self._handle(message)
        except Exception as error:  # a test reads the log; never kill the driver thread
            with self.lock:
                self.received.append((time.perf_counter(), f"error: {error}".encode()))

    def _handle(self, m: bytes) -> None:
        status = m[0]
        if status == 0xF0:
            self._sysex(m)
        elif status == 0xF8:
            self._tick()
        elif status in (0xFA, 0xFB):
            self._running, self._clock = True, (0 if status == 0xFA else self._clock)
        elif status == 0xFC:
            self._running = False
        elif status & 0xF0 == 0xB0:
            self._cc(status & 0x0F, m[1], m[2])
        elif status & 0xF0 == 0xC0:
            with self.lock:
                self.programs.append((status & 0x0F, m[1]))
        elif status & 0xF0 in (0x80, 0x90):
            on = status & 0xF0 == 0x90 and m[2] > 0
            with self.lock:
                if (status & 0x0F) == 1 and m[1] <= 26:  # the effect channel: LEDs
                    self.leds[m[1]] = on
                else:
                    self.notes.append((status & 0x0F, m[1], m[2] if on else 0))

    def _cc(self, channel: int, cc: int, value: int) -> None:
        with self.lock:
            self.ccs.append((channel, cc, value))
            if cc in (101, 100):
                msb, lsb = self._rpn.get(channel, (127, 127))
                self._rpn[channel] = (value, lsb) if cc == 101 else (msb, value)
            elif cc == 6 and self._rpn.get(channel) == (0, 0):
                self.bend_ranges[channel] = value

    def _sysex(self, m: bytes) -> None:
        if identity.is_query(m):
            self.send(identity.reply_for(self.model, emulated=True))
            return
        if m == ota.ENTER_UPGRADE:
            threading.Thread(target=self._ota, daemon=True).start()
            return
        reply = ota.parse_reply(m)
        if reply is not None:
            self._ota_reply(*reply)
            return
        change = engine_bridge.dx7_decode_param_change(m)
        if change is not None:
            param, value, _ch = change
            with self.lock:
                if param < 156:
                    self.edit[param] = value
                self.param_changes += 1
            return
        if len(m) == 5 and m[1] == 0x43 and (m[2] & 0xF0) == 0x20:
            if self.answers_dump:
                self.send(
                    engine_bridge.dx7_bank_sysex(self.bank)
                    if m[3] == 0x09
                    else engine_bridge.dx7_single_sysex(self.edit)
                )
            return
        read = engine_bridge.dx7_read(m)
        if read["messages"]:
            with self.lock:
                if len(read["voices"]) == 32:
                    self.bank = read["voices"]
                else:
                    self.edit = read["voices"][0]

    def _tick(self) -> None:
        if not self._running:
            return
        if self._clock % 6 == 0:
            step = (self._clock // 6) % self.steps
            for at, note, velocity in self.pattern:
                if at == step:
                    self.send(bytes([0x90, note, velocity]))
                if (at + 1) % self.steps == step:
                    self.send(bytes([0x80, note, 0]))
        self._clock += 1

    # --- OTA ---------------------------------------------------------------------------

    def _ota(self) -> None:
        """Re-enumerate as ota-FM-1, pull the image, come back as FM-1 Midi."""
        self.ota_state = "entering"
        time.sleep(0.3)
        self.close()
        time.sleep(0.5)
        self.open("ota-FM-1")
        self.ota_state = "serving"
        self.image = bytearray()
        self._ota_next = 0
        # The first read, again each second until adX (which opens the new port on its
        # once-a-second scan) answers it.
        for _ in range(15):
            time.sleep(1.0)
            if self.image or self.ota_state != "serving":
                return
            self._request(0)

    def _request(self, address: int) -> None:
        self._ota_next = address
        length = self.ota_block
        if self._ota_size:
            length = max(1, min(length, self._ota_size - address))
        self.send(ota.encode_request(ota.ReadRequest(1, address, length)))

    def _ota_reply(self, request: ota.ReadRequest, data: bytes, ok: bool) -> None:
        if self.ota_state != "serving" or request.address != self._ota_next:
            return
        if not ok or (self.bad_checksum_at is not None and request.address >= self.bad_checksum_at):
            self.ota_state = "stranded"  # a block failed its check: the device stops
            return
        self.image += data
        nxt = request.address + len(data)
        if self.disconnect_at is not None and nxt >= self.disconnect_at:
            self.ota_state = "disconnected"
            self.close()
            return
        if nxt >= self._ota_size_hint() or len(data) < request.length:
            self.send(ota.encode_request(ota.ReadRequest(1, ota.COMPLETE_ADDRESS, 0)))
            self.ota_state = "rebooting"
            threading.Thread(target=self._reboot, daemon=True).start()
            return
        self._request(nxt)

    def _ota_size_hint(self) -> int:
        return self._ota_size or (1 << 30)

    def expect_image(self, size: int) -> None:
        """The image's size, as the device learns it from the image header."""
        self._ota_size = size

    def _reboot(self) -> None:
        time.sleep(0.3)
        self.close()
        time.sleep(0.5)
        if self.model_after:
            self.model = self.model_after
        self.open("FM-1 Midi")
        self.ota_state = "done"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--model", default="FM-1_016")
    parser.add_argument("--answers-dump", action="store_true")
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--bank-export-after", type=float, default=-1.0)
    args = parser.parse_args()
    device = VirtualFm1(model=args.model, answers_dump=args.answers_dump)
    print(f"virtual FM-1 ({args.model}) on {device.port_name}", flush=True)
    start = time.monotonic()
    exported = False
    while time.monotonic() - start < args.seconds:
        if not exported and 0 <= args.bank_export_after <= time.monotonic() - start:
            device.bank_export()
            exported = True
        time.sleep(0.05)
    device.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
