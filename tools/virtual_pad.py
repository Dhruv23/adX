"""The emulated pad controller (phase_11.md 5, virtual_pad_drives_a_set): a loopMIDI port
speaking a shipped profile, so a set can be performed - and its lights checked -
without a physical controller.

    py -3.12 tools/virtual_pad.py --headless --script set.txt [--port loopMIDI]
                                  [--profile app/adx/pads/profiles/adx_virtual_pad.adxpad]
                                  [--report out.json]

It presses as the profile's pads (notes on its channel) into the port the app opens as
an input, and reads the lights the app sends back on the same port (on the profile's
feedback channel, so the app never takes its own lights for presses). A set script is
one action per line, its time in seconds from the start:

    0.0   launch 0 1 100     # the pad of track 0, scene 1, at velocity 100
    2.0   scene 2
    4.0   stop 0
    6.0   stopall
    6.5   tap
    8.0   expect 0 1 playing # the light of that pad, now

``expect`` lines are checked against the lights received; the report lists every
mismatch. Exit status 0 when there were none.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))

from adx import engine_bridge  # noqa: E402
from adx.testing.midi import MidiIn, MidiOut  # noqa: E402

DEFAULT_PROFILE = ROOT / "app" / "adx" / "pads" / "profiles" / "adx_virtual_pad.adxpad"


@dataclass
class Action:
    at: float
    verb: str
    args: list[str]
    line: int


def parse_set(text: str) -> list[Action]:
    actions = []
    for number, raw in enumerate(text.splitlines(), 1):
        words = raw.split("#", 1)[0].split()
        if not words:
            continue
        if len(words) < 2:
            raise ValueError(f"line {number}: a time and an action")
        actions.append(Action(float(words[0]), words[1], words[2:], number))
    return sorted(actions, key=lambda a: (a.at, a.line))


@dataclass
class VirtualPad:
    """Presses and lights, as the profile says."""

    profile_text: str
    port: str = "loopMIDI"
    profile: dict[str, object] = field(default_factory=dict)

    def __post_init__(self) -> None:
        parsed = engine_bridge.parse_pad_profile(self.profile_text)
        if not parsed["ok"]:
            raise ValueError("not a pad profile")
        self.profile = dict(parsed["profile"])
        self.out = MidiOut(self.port)
        self.inp = MidiIn(self.port)
        channel = int(self.profile["channel"])  # type: ignore[arg-type]
        self.channel = (channel - 1) if channel > 0 else 0
        self.light_channel = int(self.profile["feedback_channel"]) - 1  # type: ignore[arg-type]
        colors: dict[str, dict[str, object]] = self.profile["colors"]  # type: ignore[assignment]
        #: A light's velocity -> what it shows (velocities are unique in the profile).
        self.looks = {int(c["velocity"]): look for look, c in colors.items()}  # type: ignore[call-overload]
        self.notes: list[list[int]] = self.profile["notes"]  # type: ignore[assignment]
        self.lights: dict[int, str] = {}
        #: When each light last arrived (perf_counter).
        self.when: dict[int, float] = {}
        self.sent = 0

    # --- presses -------------------------------------------------------------------

    def _press(self, data: bytes) -> None:
        self.out.send(data)
        self.sent += 1

    def _control(self, control: dict[str, object], index: int, velocity: int = 127) -> None:
        numbers: list[int] = control["numbers"]  # type: ignore[assignment]
        if index >= len(numbers):
            raise ValueError("the profile has no such control")
        number = numbers[index]
        if control["cc"]:
            self._press(bytes([0xB0 | self.channel, number, 127]))
            self._press(bytes([0xB0 | self.channel, number, 0]))
        else:
            self._press(bytes([0x90 | self.channel, number, velocity]))
            self._press(bytes([0x80 | self.channel, number, 0]))

    def launch(self, track: int, row: int, velocity: int = 127) -> None:
        note = self.notes[row][track]
        self._press(bytes([0x90 | self.channel, note, max(1, min(127, velocity))]))
        self._press(bytes([0x80 | self.channel, note, 0]))

    def scene(self, row: int) -> None:
        self._control(self.profile["scene_launch"], row)  # type: ignore[arg-type]

    def stop(self, track: int) -> None:
        self._control(self.profile["stop_track"], track)  # type: ignore[arg-type]

    def stop_all(self) -> None:
        self._control(self.profile["stop_all"], 0)  # type: ignore[arg-type]

    def tap(self) -> None:
        self._control(self.profile["tap_tempo"], 0)  # type: ignore[arg-type]

    # --- lights --------------------------------------------------------------------

    def read_lights(self) -> int:
        """Takes what arrived; returns how many lights changed."""
        changed = 0
        for at, data in self.inp.take():
            if len(data) == 3 and data[0] == (0x90 | self.light_channel):
                self.lights[data[1]] = self.looks.get(data[2], f"velocity {data[2]}")
                self.when[data[1]] = at
                changed += 1
        return changed

    def light(self, track: int, row: int) -> str:
        self.read_lights()
        return self.lights.get(self.notes[row][track], "none")

    def close(self) -> None:
        self.out.close()
        self.inp.close()


def perform(pad: VirtualPad, actions: list[Action]) -> dict[str, object]:
    """Plays the set in real time; returns what happened."""
    start = time.perf_counter()
    mismatches: list[str] = []
    checked = 0
    for action in actions:
        while (wait := action.at - (time.perf_counter() - start)) > 0.002:
            time.sleep(min(wait - 0.0015, 0.05))
            pad.read_lights()
        a = [int(x) if x.lstrip("-").isdigit() else x for x in action.args]
        if action.verb == "launch":
            pad.launch(int(a[0]), int(a[1]), int(a[2]) if len(a) > 2 else 127)
        elif action.verb == "scene":
            pad.scene(int(a[0]))
        elif action.verb == "stop":
            pad.stop(int(a[0]))
        elif action.verb == "stopall":
            pad.stop_all()
        elif action.verb == "tap":
            pad.tap()
        elif action.verb == "expect":
            checked += 1
            shown = pad.light(int(a[0]), int(a[1]))
            if shown != str(a[2]):
                mismatches.append(
                    f"line {action.line}: pad {a[0]},{a[1]} shows {shown}, expected {a[2]}"
                )
        else:
            raise ValueError(f"line {action.line}: unknown action {action.verb!r}")
    pad.read_lights()
    return {
        "presses": pad.sent,
        "checked": checked,
        "mismatches": mismatches,
        "lights": {str(k): v for k, v in sorted(pad.lights.items())},
        "seconds": time.perf_counter() - start,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", default="loopMIDI")
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    parser.add_argument("--script", type=Path, required=True)
    parser.add_argument("--headless", action="store_true", help="no window: the script only")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)
    pad = VirtualPad(args.profile.read_text(encoding="utf-8"), args.port)
    try:
        report = perform(pad, parse_set(args.script.read_text(encoding="utf-8")))
    finally:
        pad.close()
    text = json.dumps(report, indent=2)
    if args.report:
        args.report.write_text(text, encoding="utf-8")
    print(text)
    return 0 if not report["mismatches"] else 1


if __name__ == "__main__":
    sys.exit(main())
