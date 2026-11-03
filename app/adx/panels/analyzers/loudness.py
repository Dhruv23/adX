"""The loudness meter (phase_10.md 4.4): the master's momentary, short-term and integrated
LUFS (BS.1770, measured on the audio thread), and a history of the short-term."""

from __future__ import annotations

from PySide6.QtWidgets import QWidget

from adx.engine_bridge import AnalyzerFrame
from adx.panels.analyzers.base import AnalyzerPanel
from adx.panels.analyzers.host import AnalyzerHost
from adx.panels.geometry_view import WORLD_LINES, WORLD_TRIANGLES


def _lufs(value: float) -> str:
    return "-inf" if value <= -199.0 else f"{value:.1f}"


class LoudnessPanel(AnalyzerPanel):
    panel_id = "loudness"
    title = "Loudness"
    kind = "loudness"
    layers = (("loudness.bars", WORLD_TRIANGLES), ("loudness.history", WORLD_LINES))

    def __init__(self, host: AnalyzerHost, parent: QWidget | None = None) -> None:
        super().__init__(host, parent)
        self.values = (-200.0, -200.0, -200.0)

    def show_readouts(self, frame: AnalyzerFrame) -> None:
        _correlation, momentary, short, integrated = frame.readouts
        self.values = (momentary, short, integrated)
        self.readout.setText(f"M {_lufs(momentary)}  S {_lufs(short)}  I {_lufs(integrated)} LUFS")
