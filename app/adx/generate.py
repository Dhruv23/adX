"""The genre suites from Python (phase_10.md 4.6): a style's template or a whole generated
track, as .adx text. Headless - ``adx generate`` and the scenarios use it without Qt."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from adx import engine_bridge

#: The shipped preset packs, beside the engine's sources.
PACKS = Path(__file__).resolve().parents[2] / "engine" / "preset" / "packs"


@dataclass(frozen=True)
class Style:
    id: str
    name: str
    description: str
    pack: str


@dataclass(frozen=True)
class Generated:
    text: str
    bpm: float
    key: str
    notes: list[str]


def styles() -> list[Style]:
    return [
        Style(str(s["id"]), str(s["name"]), str(s["description"]), str(s["pack"]))
        for s in engine_bridge.genre_styles()
    ]


def _generated(result: dict[str, Any]) -> Generated:
    return Generated(
        str(result["text"]),
        float(result["bpm"]),
        str(result["key"]),
        [str(n) for n in result["notes"]],
    )


def generate(
    style: str,
    *,
    seed: int = 1,
    bars: int = 32,
    bpm: float | None = None,
    key: str | None = None,
    packs: Path = PACKS,
) -> Generated:
    """A whole track: the same arguments give the same text, byte for byte."""
    return _generated(engine_bridge.generate_track(style, str(packs), seed, bars, bpm, key))


def template(
    style: str,
    *,
    seed: int = 1,
    bars: int = 32,
    bpm: float | None = None,
    key: str | None = None,
    packs: Path = PACKS,
) -> Generated:
    """The style's channels, mixer, playlist skeleton and markers, with no notes."""
    return _generated(engine_bridge.style_template(style, str(packs), seed, bars, bpm, key))
