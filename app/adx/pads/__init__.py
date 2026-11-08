"""Pad controllers (phase_11.md 4.7): the shipped profiles, the user's own, and the one
object that turns an open MIDI input into a clip launcher and lights its pads.

A profile is a ``.adxpad`` text file. Shipped ones live in ``profiles/`` beside this
module; learned or edited ones in the settings directory's ``pads/``, where a profile
of the same name overrides the shipped one. ``PadControllers.attach`` matches an input
port to a profile by its MATCH_DEVICE (or takes the one named), hands the compiled map
to the audio thread and starts the lights on the matching output.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from adx import engine_bridge
from adx.engine_bridge import Engine

SHIPPED = Path(__file__).resolve().parent / "profiles"


@dataclass
class Profile:
    """A profile file and what it says."""

    path: Path
    text: str
    name: str
    match_device: str
    columns: int
    rows: int
    warnings: list[str] = field(default_factory=list)
    shipped: bool = True

    @staticmethod
    def read(path: Path, shipped: bool = True) -> Profile | None:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            return None
        parsed = engine_bridge.parse_pad_profile(text)
        if not parsed["ok"]:
            return None
        info = parsed["profile"]
        return Profile(
            path=path,
            text=text,
            name=str(info["name"]) or path.stem,
            match_device=str(info["match_device"]),
            columns=int(info["columns"]),
            rows=int(info["rows"]),
            warnings=[str(w) for w in parsed["warnings"]],
            shipped=shipped,
        )

    def matches(self, port: str) -> bool:
        return bool(self.match_device) and self.match_device in port


def user_dir(settings_dir: Path | None) -> Path | None:
    return settings_dir / "pads" if settings_dir is not None else None


def profiles(settings_dir: Path | None = None) -> list[Profile]:
    """Every profile, shipped first; a user profile replaces a shipped one of its name."""
    found: dict[str, Profile] = {}
    for path in sorted(SHIPPED.glob("*.adxpad")):
        profile = Profile.read(path)
        if profile is not None:
            found[profile.name] = profile
    mine = user_dir(settings_dir)
    if mine is not None and mine.is_dir():
        for path in sorted(mine.glob("*.adxpad")):
            profile = Profile.read(path, shipped=False)
            if profile is not None:
                found[profile.name] = profile
    return list(found.values())


def match(port: str, settings_dir: Path | None = None) -> Profile | None:
    """The profile whose device this port is; the longest match wins."""
    candidates = [p for p in profiles(settings_dir) if p.matches(port)]
    return max(candidates, key=lambda p: len(p.match_device), default=None)


def save_learned(text: str, settings_dir: Path) -> Path:
    """Writes a learned profile into the user's pads directory."""
    parsed = engine_bridge.parse_pad_profile(text)
    name = str(parsed["profile"]["name"]) or "Learned"
    folder = settings_dir / "pads"
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / (re.sub(r"[^A-Za-z0-9_-]+", "_", name).strip("_").lower() + ".adxpad")
    path.write_text(text, encoding="utf-8")
    return path


class Learn:
    """Learn mode: each press from the device is the next step (PadLearn)."""

    def __init__(self, name: str, columns: int, rows: int, match_device: str = "") -> None:
        self.native = engine_bridge.PadLearn(name, columns, rows)
        self.match_device = match_device

    @property
    def prompt(self) -> str:
        return str(self.native.prompt)

    @property
    def done(self) -> bool:
        return bool(self.native.done)

    def press(self, control: dict[str, Any]) -> bool:
        """A control message as Engine.midi_last_control reports it."""
        kind = str(control.get("kind", ""))
        kind = "cc" if kind in ("cc", "control_change") else kind
        value = round(float(control.get("value", 0.0)) * 127)
        if kind == "note" and not control.get("pressed", value > 0):
            value = 0
        return bool(
            self.native.press(
                kind, int(control.get("channel", 1)) - 1, int(control.get("number", 0)), value
            )
        )

    def skip(self) -> None:
        self.native.skip()

    def result(self) -> str:
        text = str(self.native.result())
        if self.match_device:
            text = text.replace('MATCH_DEVICE=""', f'MATCH_DEVICE="{self.match_device}"')
        return text


def _base(port: str) -> str:
    """RtMidi appends the port's index, which differs between its two directions."""
    head, _, tail = port.rpartition(" ")
    return head if tail.isdigit() else port


class PadControllers:
    """The open inputs speaking a pad profile, and the lights of the one being lit."""

    def __init__(self, engine: Engine, settings_dir: Path | None = None) -> None:
        self.engine = engine
        self.settings_dir = settings_dir
        self.attached: dict[str, Profile] = {}
        self.lit: str | None = None

    def attach(self, input_name: str, profile: Profile | None = None) -> Profile | None:
        """Makes an open input a pad controller: the named profile, or the one that
        matches its port. Lights the output of the same device when one is open (or can
        be opened). Returns the profile, or None when nothing matches."""
        profile = profile or match(input_name, self.settings_dir)
        if profile is None:
            return None
        self.engine.set_pad_profile(input_name, profile.text)
        self.attached[input_name] = profile
        output = self._output_for(input_name)
        if output is not None:
            self.engine.start_pad_feedback(output, profile.text)
            self.lit = output
        return profile

    def detach(self, input_name: str) -> None:
        self.engine.clear_pad_profile(input_name)
        self.attached.pop(input_name, None)
        if self.lit is not None and (not self.attached or _base(self.lit) == _base(input_name)):
            self.engine.stop_pad_feedback()
            self.lit = None

    def _output_for(self, input_name: str) -> str | None:
        base = _base(input_name)
        for name in self.engine.midi_outputs():
            if _base(name) == base:
                if name not in self.engine.midi_outputs_opened:
                    try:
                        self.engine.open_midi_output(name)
                    except (RuntimeError, ValueError):
                        return None
                return name
        return None

    def set_cells(self, info: dict[str, Any]) -> None:
        """The session's filled cells, so an empty one is lit as empty."""
        cells = [
            (t, r)
            for t, track in enumerate(info.get("tracks", []))
            for r, slot in enumerate(track.get("slots", []))
            if slot is not None
        ]
        self.engine.set_pad_cells(cells)

    def stop(self) -> None:
        for name in list(self.attached):
            self.detach(name)
