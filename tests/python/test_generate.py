"""The genre suites from the app (phase_10.md 4.6, 6): deterministic text, a track that
loads and plays, and the panel's one click."""

from __future__ import annotations

import pathlib

import pytest

from adx import engine_bridge, generate
from adx.engine_bridge import Project
from adx.mainwindow import MainWindow


def test_styles_listed() -> None:
    ids = [s.id for s in generate.styles()]
    assert ids == ["c418", "stakillaz"]


@pytest.mark.parametrize("style", ["c418", "stakillaz"])
def test_genre_suite_deterministic(style: str) -> None:
    a = generate.generate(style, seed=7, bars=16)
    b = generate.generate(style, seed=7, bars=16)
    assert a.text == b.text
    assert generate.generate(style, seed=8, bars=16).text != a.text
    project, diagnostics = Project.loads(a.text)
    assert not [d for d in diagnostics if d["severity"] == "error"]
    assert project.dumps() == a.text
    assert len(project.channels()) >= 4
    assert len(project.markers()) >= 4
    assert not any("not found" in n for n in a.notes)


def test_tempo_and_key_are_honoured() -> None:
    track = generate.generate("c418", seed=3, bpm=101.0, key="F major")
    assert track.bpm == 101.0
    assert track.key == "F major"
    project, _ = Project.loads(track.text)
    assert project.bpm_at(0) == pytest.approx(101.0)
    # The key it was asked for is the key its notes are in.
    from adx import analysis

    result = engine_bridge.analyze(
        "key", {"notes": [n.row() for n in analysis.arrangement_notes(project)]}
    )
    assert result["key"]["name"] in ("F major", "D minor")
    with pytest.raises(ValueError, match="no genre suite"):
        generate.generate("polka")
    with pytest.raises(ValueError, match="unknown key"):
        generate.generate("c418", key="H major")


def test_template_has_no_notes() -> None:
    bare = generate.template("stakillaz", seed=2)
    project, _ = Project.loads(bare.text)
    assert project.patterns() == []
    assert len(project.channels()) == 4
    assert len(project.markers()) >= 4


def test_generated_track_renders(tmp_path: pathlib.Path) -> None:
    track = generate.generate("stakillaz", seed=1, bars=8)
    project, _ = Project.loads(track.text)
    result = engine_bridge.export_project(
        project, {"output": str(tmp_path / "t.wav"), "format": "wav24"}
    )
    assert result["ok"], result
    stats = result["files"][0]
    assert 0.05 < stats["peak"] <= 1.0


def test_generate_panel_opens_a_track(studio: MainWindow) -> None:
    panel = studio.generate
    panel.style_box.setCurrentIndex(1)
    panel.seed.setValue(5)
    panel.bars.setValue(16)
    result = panel.run(notes=True)
    assert result is not None
    project = studio.session.require()
    assert project.dumps() == result.text
    assert "STAKILLAZ" in panel.status.text()
    assert studio.path is None  # a new, unsaved project
    template = panel.run(notes=False)
    assert template is not None
    assert studio.session.require().patterns() == []
