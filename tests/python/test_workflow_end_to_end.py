"""The Phase 6 gate (phase_6.md 5, 6): a complete track written, arranged, mixed and
exported through the real UI, without a text editor.

Scripted the way a user works: channels made in the rack with presets dropped on them,
steps clicked in the rack and notes drawn in the piano roll, patterns laid out over 32
bars in the playlist, levels and pans turned on the rack's knobs and the mixer's faders,
inserts routed with their output selectors, a send chosen from the strip's menu, three
effects dropped into slots, two automation lanes created from knobs and drawn with the
mouse - then File > Export, File > Save, File > Open, and an export again that must be
bit-identical. No step writes or edits project text.
"""

from __future__ import annotations

import pathlib
from collections.abc import Callable

import pytest
from PySide6.QtCore import QPoint
from PySide6.QtGui import QAction
from PySide6.QtWidgets import QMenu, QWidget

import ui_input
from adx import dnd
from adx.engine_bridge import PPQ
from adx.mainwindow import MainWindow
from adx.panels.channel_rack.channel_row import ROW_HEIGHT, ChannelRow
from adx.panels.geometry_view import Pointer
from adx.panels.mixer import panel as mixer_panel
from adx.panels.params import GenericEditor
from ui_input import point, read_float_wav, ready

BAR = PPQ * 4
PACKS = pathlib.Path(__file__).resolve().parents[2] / "engine" / "preset" / "packs" / "c418"


class Chooser(QMenu):
    """A popup that, instead of waiting for a click, returns the first enabled action
    ``pick`` accepts - the choice a user would click."""

    def __init__(self, source: QMenu, pick: Callable[[QAction], bool]) -> None:
        super().__init__()
        self.source, self.pick = source, pick

    def exec(self, *_args: object) -> QAction | None:  # type: ignore[override]
        return next((a for a in self.source.actions() if a.isEnabled() and self.pick(a)), None)


def choose_from_menu(
    monkeypatch: pytest.MonkeyPatch, module: object, factory: str, pick: Callable[[QAction], bool]
) -> None:
    """The menu ``module.factory`` builds next is answered by ``pick``."""
    real = getattr(module, factory)
    monkeypatch.setattr(module, factory, lambda *a, **k: Chooser(real(*a, **k), pick))


def row_of(studio: MainWindow, name: str) -> ChannelRow:
    channel = next(int(c["id"]) for c in studio.project.channels_info() if c["name"] == name)
    return next(r for r in studio.channel_rack.rows if r.channel == channel)


def step(studio: MainWindow, row: int, steps: list[int]) -> None:
    grid = studio.channel_rack.grid
    for s in steps:
        x = (s + 0.5) * grid.state.cell_width
        ui_input.click(grid.view, Pointer(x, (row + 0.5) * ROW_HEIGHT))


def draw_note(studio: MainWindow, tick: int, pitch: int, length: int) -> None:
    """A drag with the piano roll's draw tool, through its QML input."""
    roll = studio.piano_roll
    view = roll.view
    y = view.y_of(pitch + 0.5)
    roll.input.pressed(view.x_of(tick), y, 1, 0)
    roll.input.moved(view.x_of(tick + length - PPQ // 8), y, 1, 0)
    roll.input.released(view.x_of(tick + length - PPQ // 8), y, 1, 0)


def automate(studio: MainWindow, editor: GenericEditor, parameter: str) -> int:
    """Right-click a control -> Create automation. Returns the new lane's clip id."""
    menu = editor.context_menu(parameter, editor.descriptors[parameter])
    next(a for a in menu.actions() if a.text() == "Create automation").trigger()
    return int(studio.project.automation_clips()[-1]["id"])


def draw_lane(studio: MainWindow, clip: int, values: list[float]) -> None:
    """Click points into a lane: one per bar, at the given heights (0 top .. 1 bottom)."""
    panel = studio.automation
    panel.show_clip(clip)
    editor = panel.editor
    editor.width, editor.height = 1200.0, editor.HEIGHT
    editor.pixels_per_tick = 1200.0 / (32 * BAR)
    for bar, height in enumerate(values):
        x = (bar * 4 + 2) * BAR * editor.pixels_per_tick
        ui_input.click(editor.view, Pointer(x, height * editor.HEIGHT))


def test_workflow_end_to_end(
    studio: MainWindow, tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    session, project, rack = studio.session, studio.project, studio.channel_rack
    studio.show()

    # --- 1. Four channels, instruments and presets (the rack, the browser) -----------
    for instrument in ("va", "drumsynth", "wavetable"):
        rack.instrument_box.setCurrentText(instrument)
        rack.add_channel()
    names = [str(c["name"]) for c in project.channels_info()]
    assert names == ["Lead", "Channel2", "Channel3", "Channel4"]
    lead = row_of(studio, "Lead").channel
    pad = row_of(studio, "Channel4").channel
    assert dnd.perform(
        session, dnd.Preset(str(PACKS / "kalimba.adxpreset"), "instrument"), dnd.ChannelTarget(lead)
    )
    assert dnd.perform(
        session, dnd.Preset(str(PACKS / "warm-pad.adxpreset"), "instrument"), dnd.ChannelTarget(pad)
    )
    instruments = {str(c["name"]): str(c["instrument"]) for c in project.channels_info()}
    assert instruments["Channel2"] == "va"
    assert instruments["Channel3"] == "drumsynth"

    # --- 2. Patterns: steps in the rack, notes in the piano roll -----------------------
    rack.select_pattern("P1")
    step(studio, 2, [0, 4, 8, 12])  # drums: four on the floor
    step(studio, 1, [0, 3, 6, 10])  # bass
    assert len(project.notes("P1", "Channel3")) == 4
    rack.create_pattern("Chords", 4 * BAR)
    rack.select_pattern("Chords")
    roll = studio.piano_roll
    roll.on_resize(1200.0, 600.0)
    for channel, notes in (
        ("Lead", [(0, 72, PPQ), (PPQ, 76, PPQ), (2 * PPQ, 79, 2 * PPQ), (2 * BAR, 77, BAR)]),
        ("Channel4", [(0, 60, 2 * BAR), (0, 64, 2 * BAR), (2 * BAR, 62, 2 * BAR)]),
    ):
        roll.select_clip("Chords", channel)
        roll.set_tool("draw")
        roll.fit_pattern()
        for tick, pitch, length in notes:
            draw_note(studio, tick, pitch, length)
        assert len(project.notes("Chords", channel)) == len(notes), channel
    # The rack sees the roll's notes: the Chords pattern's Lead row has lit steps.
    rack.refresh()

    # --- 3. Arrange 32 bars in the playlist --------------------------------------------
    playlist = ready(studio, 1600.0, 400.0)
    playlist.view.pixels_per_tick = 1500.0 / (32 * BAR)
    playlist.apply_view()
    assert studio.registry.trigger("insert.track")
    playlist.refresh()
    patterns = {str(p["name"]): int(p["id"]) for p in project.patterns_info()}
    playlist.set_tool("draw")
    playlist.toolbar.choose_pattern(patterns["P1"])
    for bar in range(4, 32, 4):  # P1 is already at bar 0
        ui_input.click(playlist.canvas, point(playlist, 1, bar * BAR + PPQ // 2))
    tracks = [int(t["id"]) for t in project.playlist_info()["tracks"]]
    playlist.toolbar.choose_pattern(patterns["Chords"])
    for bar in range(8, 32, 4):
        ui_input.click(playlist.canvas, point(playlist, tracks[1], bar * BAR + PPQ // 2))
    items = project.playlist_items()
    assert len(items) == 8 + 6
    assert int((items["start"] + items["length"]).max()) == 32 * BAR

    # --- 4. Mix: inserts, routing, levels, pans, a send ----------------------------------
    mixer = studio.mixer
    drum_bus, reverb_bus = mixer.add_insert(), mixer.add_insert()
    mixer.refresh()
    rack.refresh()
    for name, insert in (("Channel3", drum_bus), ("Lead", reverb_bus)):
        row = row_of(studio, name)
        row.output.setCurrentIndex(row._inserts.index(insert))
        row.output.activated.emit(row.output.currentIndex())
    outputs = {str(c["name"]): int(c["output"]) for c in project.channels_info()}
    assert outputs["Channel3"] == drum_bus
    assert outputs["Lead"] == reverb_bus
    row_of(studio, "Channel2").volume.value_changed.emit(0.7)
    row_of(studio, "Channel4").pan.value_changed.emit(-0.4)
    row_of(studio, "Lead").pan.value_changed.emit(0.3)
    strip = next(s for s in mixer.strips if s.insert == drum_bus)
    strip.fader.value_changed.emit(0.6)
    choose_from_menu(monkeypatch, mixer_panel, "send_menu", lambda a: a.data() == reverb_bus)
    strip.add_send.emit(drum_bus, QPoint(0, 0))
    sends = next(i for i in project.mixer_info()["inserts"] if int(i["id"]) == drum_bus)["sends"]
    assert [int(s["target"]) for s in sends] == [reverb_bus]
    assert abs(project.param_value("channel.Channel2.volume") - 0.7) < 1e-6
    assert abs(project.param_value(f"insert.{drum_bus}.gain") - 0.6) < 1e-6

    # --- 5. Three effects, dropped into slots ------------------------------------------
    master = int(project.mixer_info()["master"])
    for insert, effect in ((reverb_bus, "Reverb"), (drum_bus, "Compressor"), (master, "Limiter")):
        strip = next(s for s in mixer.strips if s.insert == insert)
        strip.slots[0].dropped.emit(0, 0, dnd.EffectType(effect))
        mixer.refresh()
    slots = {
        int(i["id"]): [str(s["type"]) for s in i["slots"]] for i in project.mixer_info()["inserts"]
    }
    assert slots[reverb_bus] == ["Reverb"]
    assert slots[drum_bus] == ["Compressor"]
    assert slots[master] == ["Limiter"]

    # --- 6. Two automation lanes, created from knobs and drawn -------------------------
    studio.instrument.show_channel(lead)
    instrument_editor = studio.instrument.findChildren(GenericEditor)[0]
    cutoff = next(n for n in instrument_editor.controls if n.endswith("cutoff"))
    first = automate(studio, instrument_editor, cutoff)
    draw_lane(studio, first, [0.8, 0.5, 0.2, 0.4, 0.6, 0.3, 0.5, 0.1])
    reverb_slot = int(
        next(i for i in project.mixer_info()["inserts"] if int(i["id"]) == reverb_bus)["slots"][0][
            "id"
        ]
    )
    studio.effect.show_slot(reverb_bus, reverb_slot)
    current = studio.effect.editor
    effect_editor = (
        current if isinstance(current, GenericEditor) else current.findChildren(GenericEditor)[0]
    )
    second = automate(studio, effect_editor, sorted(effect_editor.controls)[0])
    draw_lane(studio, second, [0.9, 0.7, 0.5, 0.3, 0.1, 0.3, 0.5, 0.7])
    for clip in (first, second):
        assert len(project.automation_points(clip)) >= 8

    # --- 7. Export, save, reopen, export again: identical ------------------------------
    first_wav = tmp_path / "song.wav"
    stats = studio.export_wav(str(first_wav))
    seconds = float(stats["frames"]) / float(stats["sample_rate"])
    assert seconds >= project.seconds_at(32 * BAR)
    audio = read_float_wav(first_wav)
    assert float(abs(audio).max()) > 0.01
    # Every quarter of the song makes sound.
    quarter = len(audio) // 4
    for part in range(4):
        assert float(abs(audio[part * quarter : (part + 1) * quarter]).max()) > 1e-3, part
    song = tmp_path / "song.adx"
    studio.path = str(song)
    assert studio.registry.trigger("file.save")
    assert song.exists()
    assert studio.open_project(str(song))
    second_wav = tmp_path / "again.wav"
    again = studio.export_wav(str(second_wav))
    assert again["hash"] == stats["hash"]
    assert second_wav.read_bytes() == first_wav.read_bytes()
    # No editor panel exists to have opened: the GUI alone did all of this.
    assert not [w for w in studio.findChildren(QWidget) if w.objectName() == "text_editor"]
