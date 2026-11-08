"""Performing from Python (phase_11.md 4.6-4.8): nudge and tap move the master rate,
and a performance recorded on an offline engine commits as one undo step of ordinary
playlist items."""

from __future__ import annotations

from adx.engine_bridge import Engine, Project

SET = """[PROJECT]
ADX_VERSION=2

[TEMPO]
0:0:0 120

[CHANNEL Lead]
INSTRUMENT=testtone
OUTPUT=insert.1

[PATTERN A]
LENGTH=1:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100

[PATTERN B]
LENGTH=1:0:0
NOTES Lead
  E4 0:0:0 0:1:0 100

[PLAYLIST]
TRACK 1 name="Lead"

[MIXER]
INSERT 1 name="Master"

[SESSION]
QUANTIZE=bar
SCENE "One"
SCENE "Two" tempo=60
TRACK Lead linked=1
  1 A
  2 B
"""


def _engine() -> tuple[Engine, Project]:
    project, _diagnostics = Project.loads(SET)
    engine = Engine(loopback=0)
    engine.set_project(project)
    engine.start()
    return engine, project


def test_rate_and_tap() -> None:
    engine, _project = _engine()
    try:
        engine.render_offline(256)
        engine.set_session_rate(1.04)
        engine.render_offline(256)
        assert abs(engine.session_rate - 1.04) < 1e-9
        engine.set_session_rate(1.0)
        engine.render_offline(256)
        assert engine.session_rate == 1.0
        for i in range(4):
            engine.tap_tempo(5.0 + i * 0.4)  # 150 bpm against the map's 120
            engine.render_offline(256)
        assert abs(engine.tapped_bpm - 150.0) < 1e-6
        assert abs(engine.session_rate - 1.25) < 1e-6
    finally:
        engine.stop()


def test_record_and_commit() -> None:
    engine, project = _engine()
    try:
        engine.record_performance(True)
        engine.play()
        engine.launch_clip(0, 0)
        engine.render_offline(96000 * 2)
        engine.fire_scene(1)  # B, and 60 bpm from the next bar
        engine.render_offline(96000 * 3)
        assert engine.performance_recording
        engine.record_performance(False)
        engine.render_offline(256)
        engine.drain_performance()
        kinds = {e["kind"] for e in engine.performance_events}
        assert {"pass", "stop", "rate"} <= kinds
        depth = project.undo_depth()
        summary = engine.commit_performance(project, engine.position_ticks())
        assert summary["items"] >= 3
        assert summary["tempo_events"] >= 1
        assert summary["patterns_made"] == 0
        assert project.undo_depth() == depth + 1
        text = project.dumps()
        assert "PATTERN A 0:0:0" in text or "PATTERN A 1:0:0" in text
        assert "PATTERN B" in text
        assert project.undo()
        assert "PATTERN B 3:0:0" not in project.dumps()
    finally:
        engine.stop()
