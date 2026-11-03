"""The transient particle fountain (phase_10.md 4.4): a burst on every kick and transient,
from a fixed pool of 512."""

from __future__ import annotations

from PySide6.QtWidgets import QWidget

from adx.panels.analyzers.base import AnalyzerPanel
from adx.panels.analyzers.host import AnalyzerHost
from adx.panels.geometry_view import WORLD_TRIANGLES


class ParticlesPanel(AnalyzerPanel):
    panel_id = "particles"
    title = "Particles"
    kind = "particles"
    layers = (("particles", WORLD_TRIANGLES),)

    def __init__(self, host: AnalyzerHost, parent: QWidget | None = None) -> None:
        super().__init__(host, parent)
