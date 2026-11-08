"""MIDI out of a test or a shell, for ``adx midi`` (phase_10.5.md 4.4): list the system's
ports, send one message, play a timed script into a port. loopMIDI makes a port that
the app opens as an input, so a script stands in for a pad controller.

Windows only (winmm through ctypes); no Qt and no engine.

A script is one message per line, its time in seconds from the start:

    # time  message   channel  data...
    0.000   note_on   1        60  100
    0.500   note_off  1        60
    0.500   cc        1        7   90
    1.000   pc        1        5
    1.250   raw       B0 40 7F

Channels are 1-16; ``raw`` takes hex bytes.
"""

from __future__ import annotations

import ctypes
import sys
import time
from dataclasses import dataclass
from pathlib import Path


class MidiError(RuntimeError):
    pass


def _winmm() -> ctypes.WinDLL:
    if sys.platform != "win32":
        raise MidiError("adx midi needs Windows (winmm)")
    return ctypes.WinDLL("winmm")


def _name(caps: ctypes.Array[ctypes.c_char]) -> str:
    return caps.raw[8:40].split(b"\0")[0].decode("mbcs", "replace")


def ports() -> dict[str, list[str]]:
    """The system's MIDI inputs and outputs, by name."""
    winmm = _winmm()
    caps = ctypes.create_string_buffer(84)
    inputs = []
    for i in range(winmm.midiInGetNumDevs()):
        if winmm.midiInGetDevCapsA(i, caps, len(caps)) == 0:
            inputs.append(_name(caps))
    outputs = []
    for i in range(winmm.midiOutGetNumDevs()):
        if winmm.midiOutGetDevCapsA(i, caps, len(caps)) == 0:
            outputs.append(_name(caps))
    return {"inputs": inputs, "outputs": outputs}


class MidiOut:
    """A winmm output, found by a part of its name."""

    def __init__(self, port: str) -> None:
        self._winmm = _winmm()
        outputs = ports()["outputs"]
        matches = [i for i, name in enumerate(outputs) if port.lower() in name.lower()]
        if not matches:
            raise MidiError(
                f"no MIDI output like {port!r} (outputs: {', '.join(outputs) or 'none'})"
            )
        self.name = outputs[matches[0]]
        self._handle = ctypes.c_void_p()
        if self._winmm.midiOutOpen(ctypes.byref(self._handle), matches[0], None, None, 0) != 0:
            raise MidiError(f"could not open MIDI output {self.name!r}")

    def send(self, data: bytes) -> None:
        if not 1 <= len(data) <= 3:
            raise MidiError("a short message is 1 to 3 bytes")
        word = sum(b << (8 * i) for i, b in enumerate(data))
        if self._winmm.midiOutShortMsg(self._handle, word) != 0:
            raise MidiError(f"sending to {self.name!r} failed")

    def close(self) -> None:
        self._winmm.midiOutReset(self._handle)
        self._winmm.midiOutClose(self._handle)

    def __enter__(self) -> MidiOut:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()


#: winmm's MIM_DATA: a short message arrived.
_MIM_DATA = 0x3C3
_CALLBACK_FUNCTION = 0x30000
_MidiInProc = (
    ctypes.WINFUNCTYPE(
        None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_size_t
    )
    if sys.platform == "win32"
    else None
)


class MidiIn:
    """A winmm input, found by a part of its name: every short message it receives,
    with its arrival time, in a list a reader drains (``take``). loopMIDI lets this and
    the app open the same port at once."""

    def __init__(self, port: str) -> None:
        import threading

        self._winmm = _winmm()
        inputs = ports()["inputs"]
        matches = [i for i, name in enumerate(inputs) if port.lower() in name.lower()]
        if not matches:
            raise MidiError(f"no MIDI input like {port!r} (inputs: {', '.join(inputs) or 'none'})")
        self.name = inputs[matches[0]]
        self._lock = threading.Lock()
        self._received: list[tuple[float, bytes]] = []

        def proc(_handle: object, message: int, _instance: int, param1: int, _p2: int) -> None:
            if message != _MIM_DATA:
                return
            status = param1 & 0xFF
            size = 2 if status & 0xF0 in (0xC0, 0xD0) else 3
            data = bytes((param1 >> (8 * i)) & 0xFF for i in range(size))
            with self._lock:
                self._received.append((time.perf_counter(), data))

        assert _MidiInProc is not None
        self._proc = _MidiInProc(proc)  # kept: winmm calls it from its own thread
        self._handle = ctypes.c_void_p()
        opened = self._winmm.midiInOpen(
            ctypes.byref(self._handle),
            matches[0],
            ctypes.cast(self._proc, ctypes.c_void_p),
            None,
            _CALLBACK_FUNCTION,
        )
        if opened != 0:
            raise MidiError(f"could not open MIDI input {self.name!r}")
        self._winmm.midiInStart(self._handle)

    def take(self) -> list[tuple[float, bytes]]:
        """Everything received since the last take, oldest first."""
        with self._lock:
            out, self._received = self._received, []
        return out

    def close(self) -> None:
        self._winmm.midiInStop(self._handle)
        self._winmm.midiInReset(self._handle)
        self._winmm.midiInClose(self._handle)

    def __enter__(self) -> MidiIn:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()


_STATUS = {
    "note_off": 0x80,
    "note_on": 0x90,
    "aftertouch": 0xA0,
    "cc": 0xB0,
    "pc": 0xC0,
    "pitch": 0xE0,
}


def message(kind: str, args: list[str]) -> bytes:
    """One message from its script words (``note_on 1 60 100``)."""
    if kind == "raw":
        return bytes(int(a, 16) for a in args)
    if kind not in _STATUS:
        raise MidiError(f"unknown message {kind!r}")
    if not args:
        raise MidiError(f"{kind} needs a channel")
    channel = int(args[0])
    if not 1 <= channel <= 16:
        raise MidiError(f"channel {channel} is not 1-16")
    data = [int(a) for a in args[1:]]
    if kind == "note_off" and len(data) == 1:
        data.append(0)
    if kind == "pitch" and len(data) == 1:
        value = data[0] + 8192  # -8192..8191
        data = [value & 0x7F, value >> 7]
    want = 1 if kind == "pc" else 2
    if len(data) != want or any(not 0 <= d <= 127 for d in data):
        raise MidiError(f"{kind} takes {want} data value(s) of 0-127")
    return bytes([_STATUS[kind] | (channel - 1), *data])


@dataclass(frozen=True)
class Event:
    at: float
    data: bytes
    line: int


def parse_script(text: str) -> list[Event]:
    events = []
    for number, raw in enumerate(text.splitlines(), 1):
        words = raw.split("#", 1)[0].split()
        if not words:
            continue
        if len(words) < 2:
            raise MidiError(f"line {number}: a time and a message")
        try:
            at = float(words[0])
            events.append(Event(at, message(words[1], words[2:]), number))
        except (ValueError, MidiError) as error:
            raise MidiError(f"line {number}: {error}") from None
    return sorted(events, key=lambda e: (e.at, e.line))


def play(out: MidiOut, events: list[Event]) -> float:
    """Send each event at its time; returns the worst lateness in ms."""
    start = time.perf_counter()
    worst = 0.0
    for event in events:
        while (wait := event.at - (time.perf_counter() - start)) > 0.002:
            time.sleep(wait - 0.0015)
        while time.perf_counter() - start < event.at:
            pass
        late = (time.perf_counter() - start - event.at) * 1000.0
        out.send(event.data)
        worst = max(worst, late)
    return worst


def play_file(port: str, path: str | Path) -> tuple[int, float]:
    events = parse_script(Path(path).read_text(encoding="utf-8"))
    with MidiOut(port) as out:
        return len(events), play(out, events)
