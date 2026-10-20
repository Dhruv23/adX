"""Automation editing (phase_6.md 4.5, 5)."""

from __future__ import annotations

import numpy as np
from PySide6.QtWidgets import QWidget

from adx import engine_bridge
from adx.engine_bridge import PPQ
from adx.mainwindow import MainWindow
from adx.panels.automation.lane_editor import LaneEditor
from adx.panels.geometry_view import SHIFT, Pointer
from adx.panels.params import GenericEditor
from ui_input import click, drag


def automate_from_control(studio: MainWindow, parameter: str) -> str:
    """Right-click a knob -> "Create automation": returns the lane's target path."""
    channel = int(studio.project.channels_info()[0]["id"])
    studio.instrument.show_channel(channel)
    editor = studio.instrument.findChildren(GenericEditor)[0]
    menu = editor.context_menu(parameter, editor.descriptors[parameter])
    entry = next(a for a in menu.actions() if a.text() == "Create automation")
    assert entry.isEnabled()
    entry.trigger()
    clips = studio.project.automation_clips()
    assert clips
    return str(clips[-1]["target"])


def lane(studio: MainWindow) -> LaneEditor:
    panel = studio.automation
    panel.refresh()
    panel.lanes.setCurrentRow(0)
    editor = panel.editor
    editor.width, editor.height = 800.0, editor.HEIGHT
    return editor


def x_of(editor: LaneEditor, tick: float) -> float:
    return (tick - editor.tick_start) * editor.pixels_per_tick


def test_create_automation_from_a_knob(studio: MainWindow) -> None:
    depth = studio.project.undo_depth()
    target = automate_from_control(studio, "filter.cutoff")
    assert target.endswith(".filter.cutoff")
    # One step, starting with a point at the parameter's current value.
    assert studio.project.undo_depth() == depth + 1
    points = studio.project.automation_points(int(studio.project.automation_clips()[-1]["id"]))
    assert len(points) >= 1
    value = studio.project.param_value(target)
    assert value is not None
    assert abs(float(points[0][1]) - value) < 1e-5
    # The knob now shows that it is automated.
    editor = studio.instrument.findChildren(GenericEditor)[0]
    editor.refresh()
    assert editor.controls["filter.cutoff"].automation_color


def test_automation_drag_is_one_command(studio: MainWindow) -> None:
    automate_from_control(studio, "filter.cutoff")
    editor = lane(studio)
    view = editor.view
    y = editor.HEIGHT / 2
    # Three quick clicks: three points, three steps (each press is a new gesture).
    depth = studio.project.undo_depth()
    for beat in (1, 2, 3):
        click(view, Pointer(x_of(editor, beat * PPQ), y))
    assert studio.project.undo_depth() == depth + 3
    points = editor.source.read().copy()
    assert len(points) >= 4
    # Shift-drag a band around the three, then drag them: one history entry.
    drag(
        view,
        Pointer(x_of(editor, PPQ * 0.5), 1, modifiers=SHIFT),
        Pointer(x_of(editor, PPQ * 3.5), editor.HEIGHT - 1, modifiers=SHIFT),
    )
    assert len(editor.selected) == 3
    depth = studio.project.undo_depth()
    start = Pointer(x_of(editor, 2 * PPQ), y)
    drag(view, start, *[Pointer(start.x + i * 4, y - i * 3) for i in range(1, 11)])
    assert studio.project.undo_depth() == depth + 1
    moved = editor.source.read().copy()
    assert not np.array_equal(moved, points)
    # A second drag straight after is its own step, not merged into the first.
    drag(view, start, Pointer(start.x, y - 10))
    assert studio.project.undo_depth() == depth + 2
    studio.session.undo()
    assert np.array_equal(editor.source.read(), moved)
    studio.session.undo()
    assert np.array_equal(editor.source.read(), points)


def test_automation_shape_library(studio: MainWindow) -> None:
    automate_from_control(studio, "filter.cutoff")
    lane(studio)
    panel = studio.automation
    assert set(panel.shape_actions) >= {"ramp", "pulse", "sine", "random"}
    for shape in ("ramp", "pulse", "sine", "random"):
        depth = studio.project.undo_depth()
        panel.shape_actions[shape].trigger()
        assert studio.project.undo_depth() == depth + 1
        points = panel.editor.source.read()
        assert len(points) >= 2
        assert np.all(np.diff(points[:, 0]) >= 0)


def test_automation_uses_engine_curve(qapp: object) -> None:
    # The lane is tessellated by the engine's AutomationGeometry, which evaluates
    # core::Curve; the Python side only asks. One exponential segment 0..1 over a beat:
    points = np.array([[0.0, 0.0, 1, 0.5, 0.33, 0.0, 0.67, 1.0], [PPQ, 1.0, 0, 0, 0, 0, 0, 0]])
    geometry = engine_bridge.AutomationGeometry()
    geometry.build(
        points, (0.0, 1.0, False), (0.0, float(PPQ), 1000.0 / PPQ, 1.0), np.zeros(2, np.uint8)
    )
    with geometry.view(0) as lease:
        vertices = np.array(lease.array, copy=True).reshape(-1, 3)
    xs, ys = vertices[:, 0], vertices[:, 1]
    inside = (xs > 0.01) & (xs < 0.99)
    assert inside.sum() > 10
    t = xs[inside]
    expected = np.array(
        [engine_bridge.curve_evaluate(1, 0.5, 0.33, 0.0, 0.67, 1.0, float(v)) for v in t]
    )
    assert np.allclose(1.0 - ys[inside], expected, atol=1e-5) or np.allclose(
        ys[inside], expected, atol=1e-5
    )


def test_controller_lfo_editor(studio: MainWindow) -> None:
    controllers = studio.automation.controllers
    depth = studio.project.undo_depth()
    assert controllers.add("lfo", "channel.Lead.volume")
    assert studio.project.undo_depth() == depth + 1
    info = studio.project.controllers_info()
    assert len(info) == 1
    assert str(info[0]["kind"]) == "lfo"
    assert controllers.findChildren(QWidget)
