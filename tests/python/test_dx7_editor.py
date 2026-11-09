"""The dx7 instrument in the app (phase_11.5.md 5, 6): test_dx7_editor.py - every
parameter has a control that moves it and back, automatable and learnable; the curves
redraw from the values; the name is ten characters; a voice saves and reloads in the
project and as an `.adxpreset`, and converts to FM with a report."""

from __future__ import annotations

import pathlib

import pytest

from adx import engine_bridge
from adx.hardware import dx7
from adx.mainwindow import MainWindow
from adx.panels.instrument.dx7.editor import Dx7Overview, dx7_editor, eg_points
from adx.panels.instrument.generic_editor import channel_target

FIXTURES = pathlib.Path(__file__).resolve().parents[1] / "fixtures" / "dx7"


def open_random(studio: MainWindow, index: int = 3) -> tuple[int, list[int]]:
    voice = dx7.read_file(FIXTURES / "random.syx")["voices"][index]
    name = dx7.open_as_channel(studio.session, voice)
    cid = next(int(c["id"]) for c in studio.project.channels_info() if c["name"] == name)
    return cid, voice


def test_open_as_channel_is_the_voice(studio: MainWindow) -> None:
    history = len(studio.project.history())
    cid, voice = open_random(studio)
    assert len(studio.project.history()) == history + 1  # one undo step
    assert dx7.channel_voice(studio.session, cid) == voice
    info = next(c for c in studio.project.channels_info() if int(c["id"]) == cid)
    assert info["instrument"] == "dx7"


def test_every_parameter_moves_and_back(studio: MainWindow) -> None:
    cid, _voice = open_random(studio)
    editor = dx7_editor(studio.session, cid)
    generic = editor.widget(1)
    names = dx7.param_names()
    target = channel_target(studio.session, cid)
    assert target.automate is not None
    assert target.learn is not None
    # Each voice byte: a control in the generic editor, which turns the parameter.
    controls = getattr(generic, "controls", None)
    assert controls is not None
    shown = set(controls)
    missing = [n for n in names if n not in shown]
    assert not missing, missing[:10]
    for name in ("op1.level", "algorithm", "op3.kls.rcurve", "lfo.wave", "transpose"):
        before = target.read_values()[name]
        target.turn(name, before + 1 if name != "transpose" else before - 1)
        assert target.read_values()[name] != before
        target.turn(name, before)
        assert target.read_values()[name] == before


def test_curves_redraw_from_values(studio: MainWindow) -> None:
    cid, _voice = open_random(studio)
    overview = Dx7Overview(studio.session, cid)
    target = channel_target(studio.session, cid)
    target.turn("op1.eg.l2", 10.0)
    overview.op_box.setCurrentIndex(0)
    overview.refresh()
    assert overview.curves.eg[2][1] == pytest.approx(10.0 / 99.0)
    target.turn("op1.kls.rdepth", 99.0)
    target.turn("op1.kls.rcurve", 3.0)
    overview.refresh()
    assert max(v for _k, v in overview.curves.scaling) > 0.0
    target.turn("op1.kls.rcurve", 0.0)
    overview.refresh()
    assert min(v for _k, v in overview.curves.scaling) < 0.0
    assert overview.algorithm.algorithm == int(target.read_values()["algorithm"])
    points = eg_points([99, 99, 99, 99], [99, 99, 99, 0])
    assert points[0] == (0.0, 0.0)
    assert points[-1][1] == 0.0


def test_name_is_ten_characters(studio: MainWindow) -> None:
    cid, _voice = open_random(studio)
    overview = Dx7Overview(studio.session, cid)
    assert overview.name.maxLength() == 10
    overview.name.setText("BRASS LEAD AND MORE")
    assert overview.name.text() == "BRASS LEAD"
    history = len(studio.project.history())
    overview.commit_name()
    assert dx7.name_of(dx7.channel_voice(studio.session, cid)) == "BRASS LEAD"
    assert len(studio.project.history()) == history + 1


def test_saves_and_reloads(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    cid, voice = open_random(studio)
    # In the project.
    path = tmp_path / "dx7.adx"
    studio.project.save(str(path))
    again, diagnostics = engine_bridge.Project.load(str(path))
    assert not [d for d in diagnostics if d["severity"] == "error"]
    info = next(c for c in again.channels_info() if c["instrument"] == "dx7")
    held = {str(p[0]): float(p[1]) for p in info["params"]}
    names = dx7.param_names()[:156]
    defaults = engine_bridge.dx7_voice_to_params(dx7.init_voice())
    values = [held.get(n, defaults[i]) for i, n in enumerate(names)]
    assert engine_bridge.dx7_params_to_voice(values) == voice
    # As a .syx and back.
    syx = tmp_path / "one.syx"
    Dx7Overview(studio.session, cid).export(str(syx))
    assert dx7.read_file(syx)["voices"] == [voice]


def test_preset_round_trip(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    cid, voice = open_random(studio, 7)
    preset = tmp_path / "voice.adxpreset"
    project = studio.project
    Dx7Overview(studio.session, cid).save_preset(str(preset), "dx7 test")
    assert "dx7" in preset.read_text(encoding="utf-8")
    other = dx7.open_as_channel(studio.session, dx7.init_voice(), "Target")
    tid = next(int(c["id"]) for c in project.channels_info() if c["name"] == other)
    studio.session.call(project.apply_preset, tid, str(preset))
    assert dx7.channel_voice(studio.session, tid) == voice


def test_convert_to_fm_reports(studio: MainWindow) -> None:
    params, report = dx7.to_fm(dx7.init_voice())
    assert report == []  # INIT VOICE uses nothing FM lacks
    assert params["algorithm"] == 1.0
    assert params["op1.level"] == pytest.approx(1.0)
    rich = dx7.read_file(FIXTURES / "extremes.syx")["voices"][1]
    _params, report = dx7.to_fm(rich)
    assert any("LFO" in line for line in report)
    assert any("keyboard level scaling" in line for line in report)
    assert any("pitch EG" in line for line in report)
    cid, _voice = open_random(studio)
    before = len(studio.project.channels_info())
    lines = Dx7Overview(studio.session, cid).convert()
    channels = studio.project.channels_info()
    assert len(channels) == before + 1
    assert channels[-1]["instrument"] == "fm"
    assert isinstance(lines, list)
