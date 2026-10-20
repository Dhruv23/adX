"""The browser and drag-and-drop (phase_6.md 4.6, 5: ``browser_drag_creates_command``)."""

from __future__ import annotations

import pathlib
from typing import Any

import pytest
from PySide6.QtCore import QPointF, Qt
from PySide6.QtGui import QDropEvent

from adx import dnd
from adx.engine_bridge import PPQ
from adx.mainwindow import MainWindow
from adx.panels.browser.model import Entry
from ui_input import write_wav

PACKS = pathlib.Path(__file__).resolve().parents[3] / "engine" / "preset" / "packs" / "c418"
INSTRUMENT = str(PACKS / "kalimba.adxpreset")
EFFECT = str(PACKS / "long-hall.adxpreset")


def channel(studio: MainWindow, name: str) -> int:
    return next(int(c["id"]) for c in studio.project.channels_info() if c["name"] == name)


def table(studio: MainWindow, wav: str) -> list[tuple[dnd.Payload, Any, str]]:
    """Every (payload, target, what it does) the plan's table lists."""
    project = studio.project
    master = int(project.mixer_info()["master"])
    studio.session.build_and_execute("AddSlot", master, "Delay")
    slot = int(project.mixer_info()["inserts"][0]["slots"][-1]["id"])
    studio.session.build_and_execute("AddChannelNew", "Smp", "sampler", 0, "")
    pattern = int(project.patterns_info()[0]["id"])
    lead = channel(studio, "Lead")
    return [
        (dnd.SampleFile(wav), dnd.PlaylistTarget(1, 8 * PPQ * 4), "add_audio_item"),
        (dnd.SampleFile(wav), dnd.RackTarget(), "add_sampler_channel"),
        (dnd.SampleFile(wav), dnd.ZoneTarget(channel(studio, "Smp")), "add_zone"),
        (dnd.Preset(INSTRUMENT, "instrument"), dnd.ChannelTarget(lead), "apply_preset"),
        (dnd.Preset(INSTRUMENT, "instrument"), dnd.RackTarget(), "apply_preset"),
        (dnd.Preset(EFFECT, "effect"), dnd.SlotTarget(master, slot), "apply_effect_preset"),
        (dnd.PatternRef(pattern), dnd.PlaylistTarget(1, 16 * PPQ * 4), "AddPatternItem"),
        (dnd.EffectType("Reverb"), dnd.SlotTarget(master, 0), "AddSlot"),
        (dnd.EffectType("Chorus"), dnd.SlotTarget(master, slot), "set_slot_effect"),
    ]


def test_browser_drag_creates_command(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    wav = write_wav(tmp_path / "hit.wav", 0.25)
    for payload, target, expected in table(studio, wav):
        # Through the MIME round trip a real drag makes.
        decoded = dnd.decode(dnd.encode(payload))
        assert decoded == payload
        before = studio.project.undo_depth()
        text = studio.project.dumps()
        assert dnd.perform(studio.session, decoded, target) == expected
        assert studio.project.undo_depth() == before + 1, expected
        assert studio.project.dumps() != text, expected
        # One undo takes the whole drop back.
        studio.session.undo()
        assert studio.project.dumps() == text, expected
        studio.session.redo()


@pytest.mark.parametrize(
    ("payload", "target"),
    [
        (dnd.PatternRef(1), dnd.RackTarget()),
        (dnd.EffectType("Reverb"), dnd.PlaylistTarget(1, 0)),
        (dnd.Preset(EFFECT, "effect"), dnd.ChannelTarget(1)),
        (dnd.Preset(INSTRUMENT, "instrument"), dnd.SlotTarget(1, 0)),
    ],
)
def test_drop_on_the_wrong_target_does_nothing(
    studio: MainWindow, payload: dnd.Payload, target: dnd.Target
) -> None:
    before = studio.project.undo_depth()
    assert not dnd.accepts(payload, target)
    assert dnd.perform(studio.session, payload, target) == ""
    assert studio.project.undo_depth() == before


def test_a_real_drop_on_the_rack(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    wav = write_wav(tmp_path / "Kick 01.wav", 0.25)
    body = studio.channel_rack.content
    data = dnd.encode(dnd.SampleFile(wav))
    event = QDropEvent(
        QPointF(10, 10),
        Qt.DropAction.CopyAction,
        data,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
    )
    body.dropEvent(event)
    names = [str(c["name"]) for c in studio.project.channels_info()]
    assert names == ["Lead", "Kick_01"]
    assert str(studio.project.channels_info()[-1]["instrument"]) == "sampler"


def leaves(entry: Entry) -> list[Any]:
    return [entry] if not entry.children else [x for c in entry.children for x in leaves(c)]


def test_browser_lists_presets_and_filters_by_tag(studio: MainWindow) -> None:
    browser = studio.browser
    browser.refresh()
    presets = {entry.label: entry for entry in browser.sections()}["Presets"]
    everything = leaves(presets)
    assert len(everything) >= 7
    assert all(isinstance(e.payload, dnd.Preset) for e in everything)
    # The tag filter keeps only presets carrying the tag.
    index = browser.tag.findText("reverb")
    assert index >= 0
    browser.tag.setCurrentIndex(index)
    browser.refresh()
    filtered = leaves({entry.label: entry for entry in browser.sections()}["Presets"])
    assert 0 < len(filtered) < len(everything)
    assert all("reverb" in e.tags for e in filtered)


def test_a_sampler_preset_dropped_on_a_channel_plays(
    studio: MainWindow, tmp_path: pathlib.Path
) -> None:
    """P4-2: a preset carries its samples - dropped on a channel, it plays."""
    import numpy as np

    from adx import engine_bridge
    from adx.engine_bridge import PPQ
    from ui_input import read_float_wav

    kit = tmp_path / "kit"
    kit.mkdir()
    write_wav(kit / "hit.wav", 0.5, decay=4.0)
    preset = tmp_path / "hit.adxpreset"
    preset.write_text(
        '[PRESET]\nNAME="Hit"\nTYPE=sampler\n\n[ZONES]\nZONE "kit/hit.wav" key=60 root=60\n',
        encoding="utf-8",
    )
    session, project = studio.session, studio.project
    session.build_and_execute("AddChannelNew", "Drum", "additive", 0, "")
    drum = int(project.channels_info()[-1]["id"])
    notes = engine_bridge.empty_notes(1)
    notes["start"], notes["length"], notes["pitch"], notes["velocity"] = 0, PPQ, 60, 120
    session.execute(
        engine_bridge.commands.edit_notes(
            "P1", "Drum", np.zeros(0, np.uint32), engine_bridge.empty_notes(), notes, "hit"
        )
    )
    depth = project.undo_depth()
    assert dnd.perform(session, dnd.Preset(str(preset), "instrument"), dnd.ChannelTarget(drum))
    assert project.undo_depth() == depth + 1
    info = next(c for c in project.channels_info() if int(c["id"]) == drum)
    assert str(info["instrument"]) == "sampler"
    assert int(info["zones"]) == 1
    wav = tmp_path / "out.wav"
    studio.export_wav(str(wav))
    assert float(np.abs(read_float_wav(wav)[: 48000 // 4]).max()) > 0.01
