"""The piano roll's tools and edit logic (phase_5.md 4.7, 5).

Headless: the tools act on a RollModel with a fixed view, driven by Pointer events in
the scene-graph item's pixels, exactly as the panel drives them.
"""

from __future__ import annotations

import subprocess
import sys
from collections.abc import Callable

import numpy as np
import pytest

from adx import engine_bridge
from adx.engine_bridge import PPQ, Project
from adx.panels.piano_roll import chords, quantize, scales, tools
from adx.panels.piano_roll.model import (
    RollModel,
    field_to_lane,
    lane_to_field,
    lane_values,
    set_lane_values,
)
from adx.panels.piano_roll.tools import ALT, CTRL, RIGHT, SHIFT, Pointer
from adx.panels.piano_roll.view_state import ViewState

BEAT = PPQ
SCENE = """[PROJECT]
ADX_VERSION=2

[CHANNEL Lead]
OUTPUT=insert.1

[PATTERN Roll]
LENGTH=4:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100
  C4 0:1:0 0:1:0 90
  E4 0:2:0 0:1:0 80
  G4 0:2:0 0:1:0 80
  B4 0:2:0 0:1:0 80

[PLAYLIST]
TRACK 1
  PATTERN Roll 0:0:0

[MIXER]
INSERT 1 name="Master"
"""

#: 0.05 px per tick: a beat is 192 px, a sixteenth 48 px. Rows are 10 px from pitch 84.
VIEW = {"tick_start": 0.0, "pitch_top": 84.0, "pixels_per_tick": 0.05, "pixels_per_semitone": 10.0}


def make_model() -> RollModel:
    project, diagnostics = Project.loads(SCENE)
    assert not [d for d in diagnostics if d["severity"] == "error"]
    view = ViewState(width=1000.0, height=600.0, lane_height=100.0, **VIEW)  # type: ignore[arg-type]
    return RollModel(project=project, pattern="Roll", channel="Lead", view=view)


def at(
    model: RollModel, tick: float, pitch: float, button: int = tools.LEFT, modifiers: int = 0
) -> Pointer:
    """The pixel of a tick and the middle of a pitch row."""
    return Pointer(model.view.x_of(tick), model.view.y_of(pitch + 0.5), button, modifiers)


def drag(model: RollModel, tool: tools.Tool, start: Pointer, *path: Pointer) -> None:
    tool.press(model, start)
    for point in path:
        tool.move(model, point)
    tool.release(model, path[-1] if path else start)


def ids_at(model: RollModel, pitch: int, start: int) -> list[int]:
    notes = model.notes()
    return [int(i) for i in notes["id"][(notes["pitch"] == pitch) & (notes["start"] == start)]]


def g_draw(m: RollModel) -> None:
    drag(m, tools.DrawTool(), at(m, 3 * BEAT, 72), at(m, 3.5 * BEAT, 72))


def g_paint(m: RollModel) -> None:
    drag(
        m,
        tools.PaintTool(),
        at(m, 3 * BEAT, 74),
        *[at(m, 3 * BEAT + k * PPQ // 4, 74) for k in range(1, 4)],
    )


def g_select_move(m: RollModel) -> None:
    tool = tools.SelectTool()
    drag(m, tool, at(m, 0.5 * BEAT, 60), at(m, 0.5 * BEAT, 62))


def g_slice(m: RollModel) -> None:
    drag(m, tools.SliceTool(), at(m, 0.5 * BEAT, 60))


def g_glue(m: RollModel) -> None:
    m.select(ids_at(m, 60, 0) + ids_at(m, 60, BEAT))
    drag(m, tools.GlueTool(), at(m, 0.5 * BEAT, 60))


def g_strum(m: RollModel) -> None:
    m.select(ids_at(m, 64, 2 * BEAT) + ids_at(m, 67, 2 * BEAT) + ids_at(m, 71, 2 * BEAT))
    drag(m, tools.StrumTool(), at(m, 2.1 * BEAT, 67), at(m, 2.4 * BEAT, 67))


def g_mute(m: RollModel) -> None:
    drag(m, tools.MuteTool(), at(m, 0.5 * BEAT, 60))


def g_slide(m: RollModel) -> None:
    drag(m, tools.SlideTool(), at(m, 0.5 * BEAT, 60), at(m, 0.9 * BEAT, 63))


def g_bend(m: RollModel) -> None:
    pencil = [at(m, (0.1 + 0.05 * k) * BEAT, 60 + (k % 3) * 0.3) for k in range(1, 12)]
    drag(m, tools.PitchCurveTool(), at(m, 0.05 * BEAT, 60), *pencil)


def g_chord(m: RollModel) -> None:
    drag(m, chords.ChordTool(), at(m, 3 * BEAT, 62))


def g_lane(m: RollModel) -> None:
    editor = tools.LaneEditor()
    lane_y = m.view.note_height + 10
    x = m.view.x_of(0) + 1
    assert editor.press(m, Pointer(x, lane_y))
    editor.release(m, Pointer(x, lane_y + 40))


GESTURES: list[tuple[str, Callable[[RollModel], None]]] = [
    ("draw", g_draw),
    ("paint", g_paint),
    ("select", g_select_move),
    ("slice", g_slice),
    ("glue", g_glue),
    ("strum", g_strum),
    ("mute", g_mute),
    ("slide", g_slide),
    ("pitch curve", g_bend),
    ("chord", g_chord),
    ("lane", g_lane),
]


@pytest.mark.parametrize(("name", "gesture"), GESTURES, ids=[g[0] for g in GESTURES])
def test_piano_roll_tools_emit_one_command(name: str, gesture: Callable[[RollModel], None]) -> None:
    """Every gesture, with every tool, is exactly one command and one history entry."""
    model = make_model()
    history = len(model.project.history())
    gesture(model)
    assert model.commands == 1, name
    assert len(model.project.history()) == history + 1, name


@pytest.mark.parametrize(("name", "gesture"), GESTURES, ids=[g[0] for g in GESTURES])
def test_piano_roll_undo_restores(name: str, gesture: Callable[[RollModel], None]) -> None:
    """A gesture, then undo, restores the pattern byte-identically."""
    model = make_model()
    before = model.project.dumps()
    gesture(model)
    assert model.project.dumps() != before, name
    assert model.undo()
    assert model.project.dumps() == before, name


def test_a_click_that_changes_nothing_is_no_command() -> None:
    model = make_model()
    drag(model, tools.SelectTool(), at(model, 0.5 * BEAT, 60))  # select only
    drag(model, tools.DrawTool(), at(model, 0.5 * BEAT, 60), at(model, 0.52 * BEAT, 60))  # sub-grid
    assert model.commands == 0
    assert len(model.selection) == 1


def test_tool_semantics() -> None:
    m = make_model()
    g_slice(m)
    assert sorted(m.notes()["length"][m.notes()["pitch"] == 60].tolist()) == [
        BEAT // 2,
        BEAT // 2,
        BEAT,
    ]
    m = make_model()
    g_glue(m)
    c4 = m.notes()[m.notes()["pitch"] == 60]
    assert len(c4) == 1
    assert int(c4["length"][0]) == 2 * BEAT
    m = make_model()
    g_strum(m)
    chord = m.notes()[m.notes()["pitch"] > 62]
    assert sorted(chord["start"].tolist()) == sorted(set(chord["start"].tolist()))  # spread out
    assert int(chord["start"][np.argmin(chord["pitch"])]) == 2 * BEAT  # upward from the root
    m = make_model()
    g_mute(m)
    assert int(m.notes()["muted"][m.notes()["start"] == 0][0]) == 1
    m = make_model()
    g_slide(m)
    first = ids_at(m, 60, 0)[0]
    slide = m.project.note_extras("Roll", "Lead", first)["slide"]
    assert slide is not None
    assert slide[0] == 300  # three semitones up, stored in cents


def test_right_click_deletes_and_keys_nudge() -> None:
    m = make_model()
    drag(m, tools.DrawTool(), at(m, 0.5 * BEAT, 60, RIGHT))
    assert len(m.notes()) == 4
    m.select(ids_at(m, 64, 2 * BEAT))
    assert tools.SelectTool().key(m, tools.KEY_UP, SHIFT)
    assert ids_at(m, 76, 2 * BEAT)
    assert tools.SelectTool().key(m, tools.KEY_DELETE, 0)
    assert len(m.notes()) == 3
    assert m.commands == 3


def test_selection_modes() -> None:
    m = make_model()
    tool = tools.SelectTool()
    drag(tool=tool, model=m, start=at(m, 0.5 * BEAT, 60))
    drag(m, tool, at(m, 2.5 * BEAT, 64, modifiers=SHIFT))
    assert len(m.selection) == 2
    drag(m, tool, at(m, 0.5 * BEAT, 60, modifiers=CTRL))
    assert len(m.selection) == 1
    # A rubber band over the chord, replacing the selection.
    drag(
        m,
        tool,
        Pointer(m.view.x_of(1.9 * BEAT), m.view.y_of(73)),
        Pointer(m.view.x_of(2.2 * BEAT), m.view.y_of(63)),
    )
    assert len(m.selection) == 3
    assert m.commands == 0


def test_alt_slide_targets_the_next_notes_head() -> None:
    m = make_model()
    drag(m, tools.SlideTool(), at(m, 0.5 * BEAT, 60), at(m, 2.05 * BEAT, 67, modifiers=ALT))
    slide = m.project.note_extras("Roll", "Lead", ids_at(m, 60, 0)[0])["slide"]
    assert slide is not None
    assert slide[0] == 700


def test_simplify_thins_a_drawn_curve() -> None:
    points = [(i * 10, round(50 * np.sin(i / 3))) for i in range(60)]
    assert len(tools.simplify(points, 4.0)) < len(points) // 2
    straight = [(i, 2 * i) for i in range(30)]
    assert tools.simplify(straight, 0.5) == [straight[0], straight[-1]]


def test_quantize_strength() -> None:
    """Strength 0 is a no-op; 1.0 is exact; 0.5 is exactly halfway, in ticks."""
    notes = engine_bridge.empty_notes(4)
    notes["start"] = [10, 950, 1930, 2890]
    notes["length"] = 100
    grid = 960
    zero = quantize.quantize(notes, quantize.QuantizeSettings(grid=grid, strength=0.0))
    assert zero["start"].tolist() == notes["start"].tolist()
    exact = quantize.quantize(notes, quantize.QuantizeSettings(grid=grid, strength=1.0))
    assert exact["start"].tolist() == [0, 960, 1920, 2880]
    half = quantize.quantize(notes, quantize.QuantizeSettings(grid=grid, strength=0.5))
    assert half["start"].tolist() == [5, 955, 1925, 2885]
    swung = quantize.quantize(notes, quantize.QuantizeSettings(grid=grid, strength=1.0, swing=0.5))
    assert swung["start"].tolist() == [0, 960 + 240, 1920, 2880 + 240]
    ends = quantize.quantize(notes, quantize.QuantizeSettings(grid=grid, strength=1.0, ends=True))
    # Ends at 110, 1050, 2030, 2990 snap onto their own starts: each becomes one grid step.
    assert (ends["start"] + ends["length"]).tolist() == [960, 1920, 2880, 3840]
    long = notes.copy()
    long["length"] = 900
    ends = quantize.quantize(long, quantize.QuantizeSettings(grid=grid, strength=1.0, ends=True))
    assert (ends["start"] + ends["length"]).tolist() == [960, 1920, 2880, 3840]


_HUMANIZE_SCRIPT = """
import numpy as np, hashlib
from adx import engine_bridge
from adx.panels.piano_roll.quantize import humanize, HumanizeSettings
notes = engine_bridge.empty_notes(64)
notes["id"] = np.arange(1, 65); notes["start"] = np.arange(64) * 960; notes["velocity"] = 100
out = humanize(notes, HumanizeSettings(seed=42))
print(hashlib.sha256(out["start"].tobytes() + out["velocity"].tobytes()).hexdigest())
"""


def _fields(rows: np.ndarray) -> bytes:
    """What humanize changes, without the dtype's uninitialised padding bytes."""
    return bytes(rows["start"].tobytes() + rows["velocity"].tobytes())


def test_humanize_seeded() -> None:
    """The same seed gives the same result, across runs and across processes."""
    notes = engine_bridge.empty_notes(64)
    notes["id"] = np.arange(1, 65)
    notes["start"] = np.arange(64) * 960
    notes["velocity"] = 100
    a = quantize.humanize(notes, quantize.HumanizeSettings(seed=42))
    b = quantize.humanize(notes, quantize.HumanizeSettings(seed=42))
    assert _fields(a) == _fields(b)
    assert _fields(quantize.humanize(notes, quantize.HumanizeSettings(seed=43))) != _fields(a)
    assert not np.array_equal(a["start"], notes["start"])
    import hashlib

    here = hashlib.sha256(_fields(a)).hexdigest()
    there = subprocess.run(
        [sys.executable, "-c", _HUMANIZE_SCRIPT], capture_output=True, text=True, check=True
    )
    assert there.stdout.strip() == here
    # Reordering the array does not change any note's jitter.
    shuffled = quantize.humanize(notes[::-1].copy(), quantize.HumanizeSettings(seed=42))[::-1]
    assert _fields(shuffled) == _fields(a)


def test_scale_snap() -> None:
    """Drawing outside the scale snaps to the nearest scale degree."""
    c_major = scales.mask_of("Major")
    assert scales.snap_pitch(61, 0, c_major) == 60  # C# -> C (tie resolves down)
    assert scales.snap_pitch(66, 0, c_major) == 65  # F# -> F
    assert scales.snap_pitch(64, 0, c_major) == 64
    assert scales.snap_pitch(61, 0, 0) == 61  # no scale, no snapping
    m = make_model()
    m.scale_root, m.scale_mask = 0, c_major
    drag(m, tools.DrawTool(), at(m, 3 * BEAT, 73))
    assert ids_at(m, 72, 3 * BEAT)
    assert scales.step_in_scale(60, 2, 0, c_major) == 64


def test_lane_mapping_matches_the_engine() -> None:
    """Python's lane conversions are the engine's laneValue(), inverted."""
    for lane in range(6):
        for value in (0.0, 0.5, 1.0):
            name, field = lane_to_field(lane, value)
            row = engine_bridge.empty_notes(1)[0]
            row[name] = field
            assert abs(field_to_lane(lane, row) - value) < 1.0 / 127 + 1e-6, (lane, value)
    rows = engine_bridge.empty_notes(3)
    rows["velocity"] = [127, 64, 1]
    scaled = set_lane_values(0, rows, lane_values(0, rows) * 0.5)
    assert scaled["velocity"].tolist() == [64, 32, 1]

    # The engine draws a velocity-64 bar from 1 - 64/127 down to the lane's bottom.
    m = make_model()
    viewport = engine_bridge.make_viewport(0.0, 84.0, 0.05, 10.0, 1000.0, 500.0)
    geometry = engine_bridge.PianoRollGeometry()
    geometry.build(m.project, viewport, "Roll", "Lead", np.zeros(0, dtype=np.uint32))
    with geometry.view(4) as v:
        tops = sorted({round(float(y), 4) for y in v.array[:, 1]})
    expected = sorted({round(1 - field_to_lane(0, n), 4) for n in m.notes()} | {1.0})
    assert tops == pytest.approx(expected, abs=1e-4)


def test_arpeggiate_and_riff() -> None:
    m = make_model()
    m.select(ids_at(m, 64, 2 * BEAT) + ids_at(m, 67, 2 * BEAT) + ids_at(m, 71, 2 * BEAT))
    assert chords.arpeggiate(m, "Up")
    run = m.notes()[m.notes()["start"] >= 2 * BEAT]
    assert run["pitch"].tolist() == [64, 67, 71, 64]
    assert m.project.history()[-1].startswith("Arpeggiate selection")
    m = make_model()
    settings = chords.RiffSettings(seed=7, bars=2)
    first = chords.riff_rows(m, settings)
    assert first.tobytes() == chords.riff_rows(m, settings).tobytes()
    assert chords.generate_riff(m, settings)
    assert m.commands == 1


def test_lyrics_round_trip(tmp_path: object) -> None:
    project, _d = Project.loads(SCENE)
    note = int(project.notes("Roll", "Lead")["id"][0])
    project.execute(engine_bridge.commands.set_lyric("Roll", "Lead", note, "ka"))
    ids, starts, lengths, lyrics, aliases = project.lyric_cells("Roll", "Lead", 0, 4 * BEAT)
    assert lyrics[list(ids).index(note)] == "ka"
    assert len(ids) == len(starts) == len(lengths) == len(aliases) == 5
    assert set(aliases) == {""}  # no voicebank on this channel
