"""Phase 10's analyzers: spectrum, spectrogram, oscilloscope, vectorscope, loudness and
particles, all built in the frame clock's one engine call (phase_10.md 4.4)."""

from __future__ import annotations

from PySide6.QtWidgets import QWidget

from adx.panels.analyzers.base import AnalyzerPanel
from adx.panels.analyzers.host import AnalyzerHost
from adx.panels.analyzers.loudness import LoudnessPanel
from adx.panels.analyzers.particles import ParticlesPanel
from adx.panels.analyzers.scope import ScopePanel
from adx.panels.analyzers.spectrogram import SpectrogramPanel
from adx.panels.analyzers.spectrum import SpectrumPanel
from adx.panels.analyzers.vectorscope import VectorscopePanel

__all__ = ["AnalyzerHost", "AnalyzerPanel", "SpectrogramPanel", "make_panels"]


def make_panels(host: AnalyzerHost, parent: QWidget) -> list[AnalyzerPanel | SpectrogramPanel]:
    """The six, in the order their tabs appear."""
    return [
        SpectrumPanel(host, parent),
        SpectrogramPanel(host, parent),
        ScopePanel(host, parent),
        VectorscopePanel(host, parent),
        LoudnessPanel(host, parent),
        ParticlesPanel(host, parent),
    ]
