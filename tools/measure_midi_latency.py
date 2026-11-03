"""MIDI note-on to sound, without a hardware keyboard: the software half of P9-1.

Needs loopMIDI (https://www.tobias-erichsen.de/software/loopmidi.html) running with a
port (the default name is "loopMIDI Port"). Then, with nothing else playing:

    py -3.12 tools/measure_midi_latency.py

For each buffer size it starts adX's engine on the default output with a testtone
channel, opens the loopMIDI port as a MIDI input, and runs tools/midi_latency_probe
(built on first use with MSVC), which sends notes into the port and watches the
Windows output mix in WASAPI loopback. Both ends are on the QPC clock.

What it measures: MIDI driver + adX's input thread + the audio callback + adX's output
buffer, up to the Windows mix. What it cannot: a hardware keyboard's own USB MIDI
link (about 1 ms on most keyboards), and the device's buffer and DAC after the mix
(the probe prints the endpoint's reported latency; the "est. audible" column adds it).
So this is evidence for P9-1, not its close.

With --exclusive the engine plays the default output in WASAPI exclusive mode
(STATE.md P9-9), which loopback cannot hear. Then this script sends the notes itself
(winmm, QPC-stamped) and the backend reports when the first loud frame plays by the
device's own clock (IAudioClock), so the figure runs to the device, not to a mix.
"""

from __future__ import annotations

import argparse
import ctypes
import statistics
import subprocess
import sys
import time
from pathlib import Path

from adx.engine_bridge import Engine, Project

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "tools" / "midi_latency_probe.cpp"
OUT = ROOT / "build" / "midi_probe"
PROBE = OUT / "midi_latency_probe.exe"
VCVARS = Path(
    r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
)
RATE = 48000
BLOCKS = (64, 128, 256)
GATE_MS = 10.0
TEXT = (
    "[PROJECT]\nADX_VERSION=2\n"
    "\n[CHANNEL Probe]\nINSTRUMENT=testtone\n"
    '\n[MIXER]\nINSERT 1 name="Master"\n'
)


def _build() -> None:
    if PROBE.exists() and PROBE.stat().st_mtime >= SOURCE.stat().st_mtime:
        return
    OUT.mkdir(parents=True, exist_ok=True)
    # cmd's quoting rules defeat an inline command line; a script file sidesteps them.
    script = OUT / "build_probe.cmd"
    script.write_text(
        f'@call "{VCVARS}" >nul\r\n'
        f'cl /nologo /O2 /EHsc /std:c++20 /W3 "{SOURCE}" /Fe:"{PROBE}"\r\n',
        encoding="utf-8",
    )
    result = subprocess.run(
        ["cmd", "/c", str(script)], cwd=OUT, capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        sys.exit(f"building the probe failed:\n{result.stdout}{result.stderr}")


def _run(block: int, port: str, notes: int, threshold: float) -> tuple[list[float], float, int]:
    project, _ = Project.loads(TEXT)
    engine = Engine(sample_rate=RATE, block_frames=block)
    engine.set_project(project)
    engine.start()
    try:
        names = engine.midi_inputs()
        match = next((n for n in names if port in n), None)
        if match is None:
            sys.exit(f'no MIDI input like "{port}" (have: {names}); is loopMIDI running?')
        engine.open_midi(match)
        engine.set_midi_target(int(project.channels_info()[0]["id"]))
        result = subprocess.run(
            [str(PROBE), port, str(notes), str(threshold)],
            capture_output=True,
            text=True,
            check=False,
            timeout=notes * 3 + 30,
        )
    finally:
        engine.stop()
    if result.returncode != 0 and "endpoint" not in result.stdout:
        sys.exit(f"probe failed:\n{result.stdout}{result.stderr}")
    times: list[float] = []
    endpoint = 0.0
    misses = 0
    for line in result.stdout.splitlines():
        parts = line.split()
        if parts[0] == "note":
            try:
                times.append(float(parts[2]))
            except ValueError:
                misses += 1
        elif parts[0] == "endpoint":
            endpoint = float(parts[1])
    return times, endpoint, misses


class _MidiOut:
    """A winmm MIDI output, by name, and the QPC clock in WASAPI's 100 ns units."""

    def __init__(self, port: str) -> None:
        self._winmm = ctypes.WinDLL("winmm")
        self._kernel = ctypes.WinDLL("kernel32")
        frequency = ctypes.c_int64()
        self._kernel.QueryPerformanceFrequency(ctypes.byref(frequency))
        self._frequency = frequency.value
        caps = ctypes.create_string_buffer(84)  # MIDIOUTCAPSA
        index = -1
        for i in range(self._winmm.midiOutGetNumDevs()):
            self._winmm.midiOutGetDevCapsA(i, caps, len(caps))
            if port in caps.raw[8:40].split(b"\0")[0].decode("mbcs", "replace"):
                index = i
                break
        if index < 0:
            sys.exit(f'no MIDI output like "{port}"; is loopMIDI running?')
        self._handle = ctypes.c_void_p()
        if self._winmm.midiOutOpen(ctypes.byref(self._handle), index, None, None, 0) != 0:
            sys.exit(f"could not open MIDI output {index}")

    def now(self) -> int:
        counter = ctypes.c_int64()
        self._kernel.QueryPerformanceCounter(ctypes.byref(counter))
        return int(counter.value * 10_000_000 // self._frequency)

    def send(self, status: int, data1: int, data2: int) -> int:
        """Send one message; returns the QPC time just before it went."""
        at = self.now()
        self._winmm.midiOutShortMsg(self._handle, status | (data1 << 8) | (data2 << 16))
        return at

    def close(self) -> None:
        self._winmm.midiOutReset(self._handle)
        self._winmm.midiOutClose(self._handle)


def _run_exclusive(
    block: int, port: str, notes: int, threshold: float
) -> tuple[list[float], float, int, str]:
    project, _ = Project.loads(TEXT)
    engine = Engine(sample_rate=RATE, block_frames=block, exclusive=True)
    engine.set_project(project)
    engine.start()
    out = _MidiOut(port)
    times: list[float] = []
    misses = 0
    try:
        names = engine.midi_inputs()
        match = next((n for n in names if port in n), None)
        if match is None:
            sys.exit(f'no MIDI input like "{port}" (have: {names}); is loopMIDI running?')
        engine.open_midi(match)
        engine.set_midi_target(int(project.channels_info()[0]["id"]))
        time.sleep(0.5)
        for i in range(notes):
            pitch = 60 + i % 12
            engine.arm_output_onset(threshold)
            # Spread the sends over the device period.
            time.sleep(0.001 * (i * 7 % 11))
            sent = out.send(0x90, pitch, 110)
            deadline = time.monotonic() + 0.5
            onset = 0
            while onset == 0 and time.monotonic() < deadline:
                time.sleep(0.002)
                onset = engine.output_onset_time()
            out.send(0x80, pitch, 0)
            if onset == 0:
                misses += 1
            else:
                times.append((onset - sent) / 10_000.0)
            time.sleep(0.35)  # the release dies away before the next arm
        stream = engine.stream
    finally:
        out.close()
        engine.stop()
    label = (
        f"exclusive {stream['format']}, device block {stream['block_frames']}, "
        f"reported {stream['output_latency_ms']:.2f} ms, {stream['xruns']} xruns"
    )
    return times, 0.0, misses, label


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", default="loopMIDI Port", help="the loopMIDI port's name")
    parser.add_argument("--notes", type=int, default=30, help="notes per buffer size")
    parser.add_argument("--threshold", type=float, default=0.01, help="onset level, linear")
    parser.add_argument("--blocks", type=int, nargs="*", default=list(BLOCKS))
    parser.add_argument(
        "--exclusive", action="store_true", help="WASAPI exclusive mode (STATE.md P9-9)"
    )
    args = parser.parse_args()
    if not args.exclusive:
        _build()
    passed = True
    for block in args.blocks:
        if args.exclusive:
            times, endpoint, misses, label = _run_exclusive(
                block, args.port, args.notes, args.threshold
            )
            where = "to the device"
        else:
            times, endpoint, misses = _run(block, args.port, args.notes, args.threshold)
            label, where = "shared mode (loopback)", "to the mix"
        if not times:
            print(f"block {block:5d}: no notes came back ({misses} missed)")
            passed = False
            continue
        median = statistics.median(times)
        worst = max(times)
        p95 = sorted(times)[max(0, round(0.95 * len(times)) - 1)]
        audible = median + endpoint
        print(
            f"block {block:5d}: {where} median {median:6.2f} ms, p95 {p95:6.2f}, "
            f"max {worst:6.2f} ({len(times)} notes, {misses} missed); endpoint "
            f"{endpoint:5.2f} ms; est. audible {audible:6.2f} ms [{label}]"
        )
        # Exit status judges what was measured; the endpoint part is the device's own
        # report (shared mode's period) and is printed, not gated.
        passed = passed and misses == 0 and median < GATE_MS
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
