"""One export's settings, shared by ``adx render`` and the export dialog.

Both build an :class:`ExportSettings` and hand :meth:`ExportSettings.request` to the
engine, so the CLI and the GUI cannot disagree about what a setting means
(phase_8.md 3.5, 3.6; ``render_cli``). No Qt here: the CLI imports this headless.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

FORMATS: tuple[str, ...] = ("wav16", "wav24", "wav32f", "mp3", "flac", "ogg")
RANGES: tuple[str, ...] = ("project", "loop", "markers", "selection")
STEMS: tuple[str, ...] = ("channels", "inserts", "patterns")
DITHERS: tuple[str, ...] = ("tpdf", "shaped", "off")

_SUFFIX_FORMAT = {".wav": "wav24", ".mp3": "mp3", ".flac": "flac", ".ogg": "ogg"}


def format_for(path: str) -> str:
    """The format an output name implies: .wav is 24-bit."""
    return _SUFFIX_FORMAT.get(Path(path).suffix.lower(), "wav24")


def parse_tail(text: str) -> tuple[float, bool]:
    """``4s``, ``4``, ``500ms`` or ``silence``: (seconds, until silence)."""
    spelled = text.strip().lower()
    if spelled in ("silence", "until-silence"):
        return 0.0, True
    match = re.fullmatch(r"([0-9]*\.?[0-9]+)\s*(ms|s)?", spelled)
    if match is None:
        raise ValueError(f"tail is seconds (4s, 500ms) or 'silence', not {text!r}")
    value = float(match.group(1))
    return (value / 1000.0 if match.group(2) == "ms" else value), False


def parse_normalize(text: str) -> tuple[str, float]:
    """``-14LUFS`` or ``-1dB`` (peak): (kind, target)."""
    match = re.fullmatch(r"\s*([-+]?[0-9]*\.?[0-9]+)\s*(lufs|lu|db|dbfs|dbtp)?\s*", text.lower())
    if match is None:
        raise ValueError(f"normalize is a target like -14LUFS or -1dB, not {text!r}")
    unit = match.group(2) or "lufs"
    return ("lufs" if unit.startswith("lu") else "peak"), float(match.group(1))


@dataclass
class ExportSettings:
    """Every axis of the matrix (phase_8.md 3.5), with the dialog's defaults."""

    output: str
    format: str = "wav24"
    range: str = "project"
    from_ticks: int = 0
    to_ticks: int = 0
    from_marker: str = ""
    to_marker: str = ""
    split_at_markers: bool = False
    master: bool = True
    stems: str = ""
    tail_seconds: float = 0.0
    tail_until_silence: bool = False
    silence_db: float = -80.0
    dither: str = "tpdf"
    normalize: str = "off"
    normalize_target: float = -14.0
    sample_rate: int = 0
    render_rate: int = 48000
    block: int = 256
    mp3_bitrate: int = 320
    flac_level: int = 5
    ogg_quality: float = 0.6
    midi: bool = False
    extra: dict[str, Any] = field(default_factory=dict)

    def request(self) -> dict[str, Any]:
        """The dict ``engine_bridge.export_project`` takes."""
        if self.format not in FORMATS:
            raise ValueError(f"unknown format {self.format!r}")
        request: dict[str, Any] = {
            "output": self.output,
            "format": self.format,
            "range": self.range,
            "from": self.from_ticks,
            "to": self.to_ticks,
            "from_marker": self.from_marker,
            "to_marker": self.to_marker,
            "split_at_markers": self.split_at_markers,
            "master": self.master,
            "stems": self.stems,
            "tail": self.tail_seconds,
            "tail_until_silence": self.tail_until_silence,
            "silence_db": self.silence_db,
            "dither": self.dither,
            "normalize": self.normalize,
            "normalize_target": self.normalize_target,
            "sample_rate": self.sample_rate,
            "render_rate": self.render_rate,
            "block": self.block,
            "mp3_bitrate": self.mp3_bitrate,
            "flac_level": self.flac_level,
            "ogg_quality": self.ogg_quality,
            "midi": self.midi,
        }
        request.update(self.extra)
        return request
