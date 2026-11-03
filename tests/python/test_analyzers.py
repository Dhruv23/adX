"""The six analyzer panels (phase_10.md 4.4, 6): all open, still one engine call a frame;
each shows the master while a project plays; closing them takes the hub off the clock."""

from __future__ import annotations

import time

from PySide6.QtCore import QCoreApplication

from adx import engine_bridge
from adx.engine_bridge import Project
from adx.mainwindow import MainWindow
from adx.panels.analyzers import AnalyzerPanel, SpectrogramPanel

_SONG = """[PROJECT]
ADX_VERSION=2
TUNING=440

[TEMPO]
0:0:0 120

[METER]
0:0:0 4/4

[CHANNEL Keys]
INSTRUMENT=additive
OUTPUT=insert.1
PARAM harmonic.2=0.5
PARAM env.sustain=1

[PATTERN P1]
LENGTH=8:0:0
NOTES Keys
  60 0:0:0 2:0:0 110
  64 0:0:0 2:0:0 110
  67 2:0:0 2:0:0 110
  57 4:0:0 4:0:0 110

[PLAYLIST]
TRACK 1
  PATTERN P1 0:0:0

[MIXER]
INSERT 1 name="Master"
"""


def _open_all(studio: MainWindow) -> None:
    studio.show()
    for panel in studio.analyzers:
        panel.setFloating(True)
        panel.show()
        panel.raise_()
    QCoreApplication.processEvents()


def _play(studio: MainWindow) -> None:
    project, _ = Project.loads(_SONG)
    studio.session.open(project)
    studio.engine.play()


def _spin(studio: MainWindow, seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        studio.clock.tick()
        QCoreApplication.processEvents()
        time.sleep(0.016)


def test_visualizer_one_ffi_call(studio: MainWindow) -> None:
    _open_all(studio)
    assert studio.analyzer_host.open == {
        "spectrum",
        "spectrogram",
        "scope",
        "vector",
        "loudness",
        "particles",
    }
    assert studio.clock.analyzers is not None
    studio.clock.meters = studio.mixer.meters
    before = engine_bridge.CALLS.count
    for _ in range(60):
        studio.clock.tick()
    assert engine_bridge.CALLS.count - before == 60
    # The uploads were Qt calls on the frames' results, not engine calls.
    before = engine_bridge.CALLS.count
    for panel in studio.analyzers:
        panel.frame(studio.clock.analyzer_frame)
    studio.mixer.frame(studio.clock)
    assert engine_bridge.CALLS.count == before


def test_analyzers_show_the_master(studio: MainWindow) -> None:
    _open_all(studio)
    _play(studio)
    _spin(studio, 2.5)
    frame = studio.clock.analyzer_frame
    assert frame is not None
    assert frame.layers["spectrum.lines"][1] > 0
    assert frame.layers["scope"][1] > 0
    assert frame.layers["vector.trace"][1] > 0
    assert frame.layers["loudness.bars"][1] > 0
    _correlation, momentary, _short, _integrated = frame.readouts
    assert momentary > -70.0  # something plays
    spectrogram = next(p for p in studio.analyzers if isinstance(p, SpectrogramPanel))
    assert spectrogram.waterfall.pushed > 10
    assert spectrogram.waterfall.image is not None
    vectorscope = next(p for p in studio.analyzers if p.panel_id == "vectorscope")
    assert isinstance(vectorscope, AnalyzerPanel)
    assert vectorscope.uploads > 0
    # The additive is mono at the centre: correlation near +1.
    assert vectorscope.correlation > 0.9  # type: ignore[attr-defined]
    assert studio.analyzer_host.analyzers is not None
    assert studio.analyzer_host.analyzers.transforms > 10


def test_closing_analyzers_takes_the_hub_off(studio: MainWindow) -> None:
    _open_all(studio)
    assert studio.clock.analyzers is not None
    for panel in studio.analyzers:
        panel.hide()
    QCoreApplication.processEvents()
    assert studio.analyzer_host.open == set()
    assert studio.clock.analyzers is None
    before = engine_bridge.CALLS.count
    studio.clock.tick()
    assert engine_bridge.CALLS.count - before == 1


def test_analyzer_options_reach_the_hub(studio: MainWindow) -> None:
    _open_all(studio)
    spectrum = next(p for p in studio.analyzers if p.panel_id == "spectrum")
    spectrum.fft_size.setCurrentText("32768")  # type: ignore[attr-defined]
    spectrum.window_box.setCurrentText("blackman-harris")  # type: ignore[attr-defined]
    host = studio.analyzer_host
    assert host.analyzers is not None
    settings = host.analyzers.settings()
    assert settings["fft_size"] == 32768
    assert settings["window"] == "blackman-harris"
    scope = next(p for p in studio.analyzers if p.panel_id == "scope")
    scope.hold.setChecked(True)  # type: ignore[attr-defined]
    assert host.analyzers.settings()["scope_hold"] is True
