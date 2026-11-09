"""`.adxdevice` profiles (phase_11.5.md 4.9): one per firmware generation.

    [DEVICE_PROFILE]
    NAME="M-Vave FM-1 (V16)"
    MATCH_DEVICE="FM-1 Midi"     ; the USB MIDI port name
    MATCH_MODEL="FM-1_016"       ; the identity reply's model string
    PARTS=8                      ; channels 1..8
    EFFECT_CHANNEL=2
    BEND_RANGE_MAX=24
    LED_NOTES=0-26               ; or none
    FEEDBACK=onoff               ; or none
    SENDS_VELOCITY=probe         ; yes, no or probe
    [PART_CC]
    MOD=1  ...
    [FX_CC]
    FILTER_TYPE=70  ...

A new firmware with a new CC map is a new file, not code.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

SHIPPED = Path(__file__).with_name("profiles")


class ProfileError(ValueError):
    pass


@dataclass
class DeviceProfile:
    name: str
    match_device: str
    match_model: str
    parts: int = 1
    effect_channel: int = 2
    bend_range_max: int = 12
    led_notes: range | None = None
    feedback: str = "none"
    sends_velocity: str = "probe"
    part_cc: dict[str, int] = field(default_factory=dict)
    fx_cc: dict[str, int] = field(default_factory=dict)
    path: Path | None = None

    @property
    def part_channels(self) -> list[int]:
        """The parts' MIDI channels, 1-based."""
        return list(range(1, self.parts + 1))

    def fx_names(self) -> list[str]:
        """The effects parameters, in CC order."""
        return sorted(self.fx_cc, key=lambda n: self.fx_cc[n])


def _value(text: str) -> str:
    text = text.split(";", 1)[0].strip()
    if len(text) >= 2 and text[0] == text[-1] == '"':
        return text[1:-1]
    return text


def parse(text: str, path: Path | None = None) -> DeviceProfile:
    """A profile from its text; ProfileError with a line number on anything wrong."""
    section = ""
    head: dict[str, str] = {}
    part_cc: dict[str, int] = {}
    fx_cc: dict[str, int] = {}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split(";", 1)[0].strip() if not raw.lstrip().startswith(";") else ""
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().upper()
            if section not in ("DEVICE_PROFILE", "PART_CC", "FX_CC"):
                raise ProfileError(f"line {number}: unknown section [{section}]")
            continue
        # A line may hold several KEY=VALUE pairs separated by spaces (as the plan's
        # sample does); quoted values hold no '='.
        pairs = [line] if '"' in line else line.split()
        for pair in pairs:
            if "=" not in pair:
                raise ProfileError(f"line {number}: expected KEY=VALUE, got {pair!r}")
            key, value = pair.split("=", 1)
            key, value = key.strip().upper(), _value(value)
            if section == "DEVICE_PROFILE":
                head[key] = value
            elif section in ("PART_CC", "FX_CC"):
                try:
                    cc = int(value)
                except ValueError:
                    raise ProfileError(f"line {number}: {key} is not a CC number") from None
                if not 0 <= cc <= 127:
                    raise ProfileError(f"line {number}: CC {cc} is outside 0-127")
                (part_cc if section == "PART_CC" else fx_cc)[key] = cc
            else:
                raise ProfileError(f"line {number}: {key} outside a section")
    for required in ("NAME", "MATCH_DEVICE", "MATCH_MODEL"):
        if required not in head:
            raise ProfileError(f"[DEVICE_PROFILE] has no {required}")
    if len(set(fx_cc.values())) != len(fx_cc):
        raise ProfileError("[FX_CC] uses one CC twice")
    leds: range | None = None
    if head.get("LED_NOTES", "none").lower() != "none":
        lo, _, hi = head["LED_NOTES"].partition("-")
        leds = range(int(lo), int(hi or lo) + 1)
    return DeviceProfile(
        name=head["NAME"],
        match_device=head["MATCH_DEVICE"],
        match_model=head["MATCH_MODEL"],
        parts=int(head.get("PARTS", "1")),
        effect_channel=int(head.get("EFFECT_CHANNEL", "2")),
        bend_range_max=int(head.get("BEND_RANGE_MAX", "12")),
        led_notes=leds,
        feedback=head.get("FEEDBACK", "none").lower(),
        sends_velocity=head.get("SENDS_VELOCITY", "probe").lower(),
        part_cc=part_cc,
        fx_cc=fx_cc,
        path=path,
    )


def shipped() -> list[DeviceProfile]:
    """Every profile adX ships, by file name."""
    return [parse(p.read_text(encoding="utf-8"), p) for p in sorted(SHIPPED.glob("*.adxdevice"))]


def for_model(model: str | None) -> DeviceProfile | None:
    """The profile whose MATCH_MODEL is ``model``; None for an unknown model."""
    if not model:
        return None
    for profile in shipped():
        if profile.match_model == model:
            return profile
    return None
