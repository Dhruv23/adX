"""The session from Python (phase_11.md 5): the [SESSION] model as dicts, launching on an
offline engine, and the session panel's grid, cells and edits."""

from __future__ import annotations

from adx.engine_bridge import Engine, Project

TEXT = """[PROJECT]
ADX_VERSION=2

[TEMPO]
0:0:0 120

[CHANNEL Lead]
INSTRUMENT=testtone
OUTPUT=insert.1

[CHANNEL Bass]
INSTRUMENT=testtone
OUTPUT=insert.1

[PATTERN A]
LENGTH=1:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100

[PATTERN B]
LENGTH=1:0:0
NOTES Bass
  C2 0:0:0 0:1:0 100

[PLAYLIST]
TRACK 1 name="Lead"
TRACK 2 name="Bass"

[MIXER]
INSERT 1 name="Master"

[SESSION]
QUANTIZE=bar
SCENE "One"
SCENE "Two"
TRACK Lead linked=1
  1 A follow=next
TRACK Bass linked=2
  2 B quantize=instant
"""


def _project() -> Project:
    project, diagnostics = Project.loads(TEXT)
    assert not [d for d in diagnostics if d.get("severity") == "error"]
    return project


def test_session_info_and_edit() -> None:
    project = _project()
    info = project.session_info()
    assert info["quantize"] == "bar"
    assert [s["name"] for s in info["scenes"]] == ["One", "Two"]
    lead, bass = info["tracks"]
    assert lead["channel"] == "Lead"
    assert lead["slots"][0]["pattern"] == "A"
    assert lead["slots"][0]["follow"] == "next"
    assert lead["slots"][1] is None
    assert bass["slots"][1]["quantize"] == "instant"
    # An edit is one undo step, and reaches the text.
    lead["slots"][1] = {"pattern": "A", "loop": False}
    project.set_session(info)
    assert project.session_info()["tracks"][0]["slots"][1]["loop"] is False
    assert "2 A loop=no" in project.dumps()
    assert project.undo()
    assert project.session_info()["tracks"][0]["slots"][1] is None


def test_launch_on_an_offline_engine() -> None:
    project = _project()
    engine = Engine(loopback=0)
    engine.set_project(project)
    engine.start()
    try:
        engine.render_offline(256)
        assert engine.clip_state(0, 0) == "stopped"
        engine.launch_clip(0, 0)
        engine.render_offline(256)
        # Nothing was playing, so the session's grid starts with the launch.
        assert engine.clip_state(0, 0) == "playing"
        assert 0.0 <= engine.clip_progress(0) <= 1.0
        engine.fire_scene(1)
        engine.render_offline(256)
        assert engine.clip_state(1, 1) == "playing"  # instant
        assert engine.clip_state(0, 0) == "queued-stop"  # Lead's row 2 is empty: stops at the bar
        engine.render_offline(48000 * 2)
        assert engine.clip_state(0, 0) == "stopped"
        assert engine.session_counts["refused"] == 0
    finally:
        engine.stop()


def test_session_panel_grid(quick: None) -> None:
    from adx.panels.session.panel import SessionPanel
    from adx.session import Session

    engine = Engine(null_backend=True)
    session = Session(engine)
    session.open(_project())
    panel = SessionPanel(session)
    try:
        assert set(panel.cells) == {(0, 0), (0, 1), (1, 0), (1, 1)}
        assert panel.cell_text(0, 0) == "A"
        assert panel.cell_text(1, 0) == ""
        assert panel.quantize.currentText() == "bar"
        panel.set_cell(1, 0, {"pattern": "B"})
        assert panel.cell_text(1, 0) == "B"
        panel.set_cell(1, 0, None)
        assert panel.cell_text(1, 0) == ""
        panel._add_scene()
        assert (0, 2) in panel.cells
        assert session.project is not None
        assert session.project.undo()
        panel.rebuild()
        assert (0, 2) not in panel.cells
        panel.refresh_states()
        assert panel.state_of(0, 0) == "stopped"
        # The grid is geometry: every cell one quad, from one build.
        assert panel.grid.view.vertex_count(0) == 4 * 6
        assert panel.grid.cell_at(100, 30) == (1, 1)
        assert panel.grid.cell_at(9999, 0) is None
        panel.set_scene_tempo(1, 140.0)
        assert session.project.session_info()["scenes"][1]["tempo"] == 140.0
    finally:
        panel.deleteLater()


def test_session_geometry_budget(quick: None) -> None:
    """session_geometry_budget (phase_11.md 5): an 8 x 64 grid is one build and two
    leases a frame, quickly, with the rings and marks of what plays and waits."""
    import time

    import numpy as np

    from adx import engine_bridge
    from adx.engine_bridge import SessionGeometry

    lines = ["[SESSION]", "QUANTIZE=instant"]
    lines += [f'SCENE "S{r}"' for r in range(64)]
    channels = []
    for t in range(8):
        channels.append(f"\n[CHANNEL C{t}]\nINSTRUMENT=testtone\nOUTPUT=insert.1\n")
        lines.append(f"TRACK C{t}")
        lines += [f"  {r + 1} P" for r in range(64)]
    text = (
        "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n"
        + "".join(channels)
        + "\n[PATTERN P]\nLENGTH=1:0:0\n"
        + "".join(f"NOTES C{t}\n  C4 0:0:0 0:1:0 100\n" for t in range(8))
        + '\n[PLAYLIST]\nTRACK 1 name="x"\n\n[MIXER]\nINSERT 1 name="Master"\n\n'
        + "\n".join(lines)
        + "\n"
    )
    project, diagnostics = Project.loads(text)
    assert not [d for d in diagnostics if d.get("severity") == "error"]
    engine = Engine(loopback=0)
    engine.set_project(project)
    engine.start()
    try:
        for t in range(8):
            engine.launch_clip(t, t)
        engine.render_offline(512)
        builder = SessionGeometry()
        filled = np.ones(8 * 64, dtype=np.uint8)
        before = engine_bridge.CALLS.count
        started = time.perf_counter()
        for _ in range(60):
            playing, queued = builder.build(engine, 8, 64, filled)
            for layer in (0, 1):
                with builder.view(layer):
                    pass
        per_frame = (time.perf_counter() - started) / 60
        assert playing == 8
        assert queued == 0
        # One build and two leases a frame.
        assert (engine_bridge.CALLS.count - before) / 60 <= 3
        assert per_frame < 0.004, per_frame
    finally:
        engine.stop()
