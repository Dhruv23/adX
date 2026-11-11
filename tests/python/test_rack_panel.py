"""The rack view, the browser's Sounds, and dropping a rack (phase_12.md 4.1, 4.11, 5:
test_rack_panel, browser_audition_click)."""

from __future__ import annotations

import wave
from collections.abc import Iterator
from pathlib import Path

import pytest
from pytestqt.qtbot import QtBot

from adx import dnd, engine_bridge
from adx.engine_bridge import Engine
from adx.panels.browser import model
from adx.panels.browser.panel import BrowserPanel
from adx.panels.rack.panel import RackPanel, group_of
from adx.session import Session

REPO = Path(__file__).resolve().parents[2]
DEMO = REPO / "docs" / "examples" / "phase12"
BASE = '[PROJECT]\nADX_VERSION=2\n\n[MIXER]\nINSERT 1 name="Master"\n'


@pytest.fixture
def session() -> Iterator[Session]:
    engine = Engine(null_backend=True)
    s = Session(engine)
    project, _ = engine_bridge.Project.loads(BASE)
    s.open(project)
    try:
        yield s
    finally:
        engine.stop()


def test_group_of() -> None:
    assert group_of("Euphoric Kick/Decay") == ("Euphoric Kick", "Decay")
    assert group_of("Mine") == ("Macros", "Mine")


def test_dropping_a_rack_inserts_it_and_its_macros_turn(qtbot: QtBot, session: Session) -> None:
    panel = RackPanel(session)
    qtbot.addWidget(panel)
    assert panel.empty.isVisibleTo(panel)
    done = dnd.perform(session, dnd.RackFile(str(DEMO / "euphoric-kick.adxrack")), dnd.RackTarget())
    assert done == "insert_rack"
    project = session.require()
    names = [str(c["name"]) for c in project.channels_info()]
    assert names == ["Euphoric Kick/punch", "Euphoric Kick/tail", "Euphoric Kick/click"]
    panel.refresh()
    assert len(panel.knobs) == 2
    decay = next(m for m in project.macros() if m["name"] == "Euphoric Kick/Decay")
    knob = panel.knobs[int(decay["id"])]
    assert knob.value() == 500
    knob.setValue(900)
    assert next(m for m in project.macros() if m["id"] == decay["id"])["value"] == pytest.approx(
        0.9
    )
    assert "layer" not in project.dumps()
    # One undo step undoes the whole knob turn, the next the rack.
    project.undo()
    assert next(m for m in project.macros() if m["id"] == decay["id"])["value"] == pytest.approx(
        0.5
    )
    project.undo()
    assert project.macros() == []


def test_rack_payload_round_trips() -> None:
    payload = dnd.RackFile("C:/x/kick.adxrack")
    assert dnd.decode(dnd.encode(payload)) == payload
    assert dnd.accepts(payload, dnd.RackTarget())
    assert not dnd.accepts(payload, dnd.SlotTarget(1, 0))


def test_sounds_group_by_category() -> None:
    presets = [
        {"name": "Hoover", "kind": "instrument", "category": "Lead", "pack": "demo", "path": "h"},
        {"name": "Verb", "kind": "effect", "category": "Chain/Space", "pack": "demo", "path": "v"},
    ]
    racks = [
        {
            "name": "Kick",
            "category": "Drums/Kick",
            "pack": "demo",
            "path": "k",
            "needs_library": True,
        },
    ]
    sounds = model.sound_entries(presets, racks)
    assert [s.label for s in sounds] == ["Drums/Kick", "Lead"]
    assert sounds[0].children[0].kind == "rack"
    assert "needs library" in sounds[0].children[0].label
    assert isinstance(sounds[0].children[0].payload, dnd.RackFile)


def test_browser_audition_click(
    qtbot: QtBot, tmp_path: Path, monkeypatch: pytest.MonkeyPatch, session: Session
) -> None:
    # A click plays the cached preview and changes nothing in the project or the graph.
    cache = tmp_path / "audition"
    (cache / "demo").mkdir(parents=True)
    with wave.open(str(cache / "demo" / "Hoover.wav"), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(48000)
        out.writeframes(b"\0\0" * 480)
    monkeypatch.setattr(model, "audition_cache", lambda: cache)
    browser = BrowserPanel(str(tmp_path), session=session)
    browser.preset_dirs = [str(DEMO)]
    qtbot.addWidget(browser)
    browser.refresh()
    before = session.require().dumps()
    played: list[str] = []
    monkeypatch.setattr(browser.preview, "play", lambda path: played.append(path) or True)
    assert browser.audition(str(DEMO / "hoover.adxpreset")).endswith("Hoover.wav")
    assert played == [str(cache / "demo" / "Hoover.wav")]
    assert session.require().dumps() == before
    # A sound never auditioned plays nothing, and says so by returning "".
    assert browser.audition(str(DEMO / "marimba.adxpreset")) == ""
