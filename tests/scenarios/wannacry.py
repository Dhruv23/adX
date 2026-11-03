"""The WannaCry corpus (phase_10.5.md 4.8): what every scenario that writes music uses.

Three local files, excluded from git and never committed, nor anything made from them:
the user's 4-bar melody, the agent's piano transcription, and the tutorial video the
reference audio comes from. ``ADX_WANNACRY_DIR`` moves the root.

CI has none of them, so each scenario also runs on a committed generated project (the
stand-in). A WannaCry case that skips because a file is missing counts as not run.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
#: How scenarios launch adX: the launcher, as the user does. ``ADX_SCENARIO_VIA=gui``
#: runs ``python -m adx gui`` instead, whose output a failing run then shows.
VIA = os.environ.get("ADX_SCENARIO_VIA", "adX.cmd")

#: The committed stand-in: a Phase 10 genre-suite track (tests/golden).
STAND_IN = ROOT / "tests" / "golden" / "genre_c418_seed1.adx"
VIDEO = "Ninajirachi & Porter Robinson - WannaCry (Piano Tutorial).mp4"


@dataclass(frozen=True)
class Corpus:
    root: Path

    @property
    def human(self) -> Path:
        return self.root / "wanna_cry_human.adx"

    @property
    def piano(self) -> Path:
        return self.root / "output" / "transcription" / "wannacry_piano.adx"

    @property
    def video(self) -> Path:
        return self.root / VIDEO

    @property
    def reference(self) -> Path:
        """Extracted by tools/extract_reference.py."""
        return self.root / "output" / "reference" / "wannacry.wav"

    def missing(self, *names: str) -> list[str]:
        return [n for n in names if not getattr(self, n).exists()]


def corpus() -> Corpus:
    return Corpus(Path(os.environ.get("ADX_WANNACRY_DIR", ROOT)))


def require(*names: str) -> Corpus:
    """The corpus, or a skip naming what is missing (which counts as not run)."""
    found = corpus()
    gone = found.missing(*names)
    if gone:
        pytest.skip(f"[wannacry] not run: missing {', '.join(gone)} under {found.root}")
    return found


#: ``material`` parameters: the stand-in everywhere, WannaCry where the corpus is.
MATERIAL = [
    pytest.param("standin", id="standin"),
    pytest.param("wannacry", id="wannacry", marks=pytest.mark.wannacry),
]


def project_for(material: str) -> Path:
    """The project a playing scenario plays: the transcription, or the stand-in."""
    if material == "wannacry":
        return require("piano").piano
    return STAND_IN
