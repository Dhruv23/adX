"""scenario_build_track_through_gui (phase_10.5.md 4.8, 5): P6-1's stand-in.

An empty project, written into through the GUI by synthesized input alone:
- the user's 4-bar melody, note by note, drawn in the piano roll;
- an 8-bar section of the transcription, both hands on two rack channels, the same way;
- the rest of the transcription by Edit > Paste of its .adx text, put on the system
  clipboard the way another program would;
- the left hand's velocity typed into the inspector for every note at once;
- the parts arranged in the playlist by a drag from the browser, a channel's volume
  turned, and the song exported through File > Export.

Then ``adx diff`` against the sources shows no note differences, and after a save and a
reopen through adX.cmd the same export is bit-identical.

On the stand-in (CI), the same route writes a Phase 10 genre-suite track's parts.

Not covered: the operating system's save dialog (``quit(save=True, path=...)`` does
what it does once it has a name) and real mouse pixels (the input is Qt's own events at
widget coordinates).
"""

from __future__ import annotations

import math
import pathlib
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Any

import numpy as np
import pytest
from wannacry import MATERIAL, STAND_IN, VIA, require

from adx.engine_bridge import PPQ, Project
from adx.export_settings import FORMATS
from adx.panels.piano_roll.quantize import GRIDS
from adx.testing.driver import Driver
from adx.testing.wav import read_wav

ROOT = pathlib.Path(__file__).resolve().parents[2]
BAR = 4 * PPQ

pytestmark = pytest.mark.skipif(
    sys.platform != "win32", reason="the Windows launcher and clipboard"
)


@dataclass(frozen=True)
class Part:
    """Notes of one source (pattern, channel), written into the same names here: the
    notes before ``drawn_bars`` drawn (all of them when None), the rest pasted."""

    source: pathlib.Path
    pattern: str
    channel: str
    drawn_bars: int | None = None


def parts_for(material: str) -> list[Part]:
    if material == "wannacry":
        corpus = require("human", "piano")
        return [
            Part(corpus.human, "P1", "Lead"),
            Part(corpus.piano, "Melody", "RightHand", drawn_bars=8),
            Part(corpus.piano, "Melody", "LeftHand", drawn_bars=8),
        ]
    return [
        Part(STAND_IN, "Intro", "Pad"),
        Part(STAND_IN, "Development", "Piano"),
        Part(STAND_IN, "Development", "Bass", drawn_bars=2),
    ]


def notes_of(part: Part) -> np.ndarray:
    project, _ = Project.load(str(part.source))
    return project.notes(part.pattern, part.channel)


def grid_for(notes: np.ndarray) -> str:
    """The coarsest snap grid every start and length sits on."""
    step = 0
    for value in [*notes["start"].tolist(), *notes["length"].tolist()]:
        step = math.gcd(step, int(value))
    fitting = [(label, ticks) for label, ticks in GRIDS if step % ticks == 0]
    assert fitting, f"no grid divides {step} ticks"
    return max(fitting, key=lambda g: g[1])[0]


def fragment(notes: np.ndarray, pattern: str, channel: str) -> str:
    """.adx text of some notes, as a copy in another program would put it."""
    lines = [
        f"  {int(n['pitch'])} {_position(int(n['start']))} {_position(int(n['length']))} {int(n['velocity'])}"
        for n in notes
    ]
    return (
        "[PROJECT]\nADX_VERSION=2\n\n"
        f"[CHANNEL {channel}]\nINSTRUMENT=additive\nOUTPUT=insert.1\n\n"
        f"[PATTERN {pattern}]\nLENGTH={_position(-(-int((notes['start'] + notes['length']).max()) // BAR) * BAR)}\n"
        f"NOTES {channel}\n" + "\n".join(lines) + "\n"
    )


def _position(ticks: int) -> str:
    bar, rest = divmod(ticks, BAR)
    beat, tick = divmod(rest, PPQ)
    return f"{bar}:{beat}:{tick}"


def set_clipboard(text: str, folder: pathlib.Path) -> None:
    source = folder / "clipboard.adx"
    source.write_text(text, encoding="utf-8")
    run = subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-Command",
            f"Set-Clipboard -Value ([IO.File]::ReadAllText('{source}'))",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert run.returncode == 0, run.stderr


class Roll:
    """The piano roll, worked by synthesized input; its viewport read back."""

    def __init__(self, app: Driver) -> None:
        self.app = app

    def view(self) -> dict[str, float]:
        return dict(
            self.app.script(
                "v = window.piano_roll.view\n"
                "result = dict(start=v.tick_start, end=v.tick_end, top=v.pitch_top, bottom=v.pitch_bottom,"
                " ppt=v.pixels_per_tick, pps=v.pixels_per_semitone)"
            )
        )

    def zoom_to(self, grid_ticks: int, pixels: float = 10.0) -> None:
        for _ in range(40):
            if self.view()["ppt"] * grid_ticks >= pixels:
                return
            self.app.scroll("piano_roll.roll", dy=120, x=10, y=10, modifiers=["ctrl"])
        raise AssertionError("the piano roll would not zoom in")

    def show(self, start: int, end: int, pitch: int) -> dict[str, float]:
        for _ in range(400):
            v = self.view()
            if start < v["start"]:
                self.app.scroll("piano_roll.roll", dy=120, modifiers=["shift"])
            elif end > v["end"] - 20 / v["ppt"]:
                self.app.scroll("piano_roll.roll", dy=-120, modifiers=["shift"])
            elif pitch + 1 > v["top"] - 0.5:
                self.app.scroll("piano_roll.roll", dy=120)
            elif pitch < v["bottom"] + 0.5:
                self.app.scroll("piano_roll.roll", dy=-120)
            else:
                return v
        raise AssertionError(f"could not bring tick {start}, pitch {pitch} into view")

    def draw(self, start: int, length: int, pitch: int, grid_ticks: int) -> None:
        v = self.show(start, start + length, pitch)
        x0 = (start - v["start"]) * v["ppt"] + min(3.0, grid_ticks * v["ppt"] / 3)
        x1 = (start + length - v["start"]) * v["ppt"]
        y = (v["top"] - pitch - 0.5) * v["pps"]
        self.app.drag("piano_roll.roll", (x0, y), (x1, y), steps=3)

    def cursor_at(self, tick: int) -> None:
        v = self.show(tick, tick + 1, int(self.view()["bottom"] + 1))
        self.app.click("piano_roll.ruler", x=(tick - v["start"]) * v["ppt"] + 1, y=5)


def set_velocity(app: Driver, velocity: int) -> None:
    """Type a velocity into the inspector, for whatever notes are selected."""
    app.key("Ctrl+A", target="inspector.velocity")
    app.type("inspector.velocity", str(velocity))
    app.key("Return", target="inspector.velocity")


def name_dialog(app: Driver, name: str) -> None:
    """The open name dialog (new pattern, rename): its text replaced, then OK."""
    app.wait_for("modal", timeout=10)
    app.key("Ctrl+A")
    app.type(None, name)
    app.answer("OK")
    app.wait_for("modal", predicate="falsy", timeout=10)


def ensure_channel(app: Driver, name: str) -> int:
    channels = app.state()["project"]["channels"]
    if name not in channels:
        app.trigger("view.channel_rack")
        app.click("channel_rack.add_channel")
        app.wait_for(lambda s: len(s["project"]["channels"]) == len(channels) + 1, timeout=10)
        new = len(channels) + 1
        app.double_click(f"channel_rack.row:{new}.name", wait=False)
        name_dialog(app, name)
    ids = app.script("result = {c['name']: int(c['id']) for c in window.project.channels_info()}")
    return int(ids[name])


def ensure_pattern(app: Driver, name: str) -> None:
    if name in app.state()["project"]["patterns"]:
        return
    app.trigger("view.channel_rack")
    app.click("channel_rack.add_pattern", wait=False)
    name_dialog(app, name)
    app.wait_for("project.patterns", predicate="contains", value=name, timeout=10)


def open_in_roll(app: Driver, pattern: str, channel: str, grid: str) -> None:
    app.trigger("view.piano_roll")
    app.choose("piano_roll.pattern", pattern)
    app.choose("piano_roll.channel", channel)
    app.choose("piano_roll.grid", grid)


def export(app: Driver, out: pathlib.Path) -> pathlib.Path:
    app.trigger("file.export", wait=False)
    app.wait_for("modal", timeout=10)
    app.key("Ctrl+A", target="export.output")
    app.type("export.output", str(out))
    app.choose("export.format", index=FORMATS.index("wav32f"))
    app.answer("Export")
    app.wait_for("status", predicate="contains", value="Exported", timeout=600)
    deadline = time.monotonic() + 30
    while not out.exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    return out


def place(app: Driver, pattern: str, track: int, tick: int) -> None:
    """Drag the pattern from the browser onto the playlist."""
    app.trigger("view.browser")
    app.trigger("view.playlist")
    spot: dict[str, Any] = dict(
        app.script(
            "p = window.playlist\n"
            f"rows = [r for r in p.track_rows() if r[0] == {track}]\n"
            f"result = dict(x=p.view.x_of({tick}) + 2, y=rows[0][1] + rows[0][2] / 2 if rows else None)"
        )
    )
    assert spot["y"] is not None, f"no playlist track {track}"
    app.drop("browser.tree", "playlist.canvas", item=pattern, x=spot["x"], y=spot["y"])


@pytest.mark.parametrize("material", MATERIAL)
def test_build_track_through_gui(
    material: str, tmp_path: pathlib.Path, evidence: pathlib.Path
) -> None:
    parts = parts_for(material)
    song = tmp_path / "song.adx"
    first_export = tmp_path / "first.wav"
    with Driver.launch(via=VIA, settings_dir=evidence / "settings") as app:
        app.wait_for("project.open", timeout=30)
        roll = Roll(app)
        for part in parts:
            notes = notes_of(part)
            ensure_pattern(app, part.pattern)
            ensure_channel(app, part.channel)
            grid = grid_for(notes)
            grid_ticks = dict(GRIDS)[grid]
            open_in_roll(app, part.pattern, part.channel, grid)
            roll.zoom_to(grid_ticks)
            limit = None if part.drawn_bars is None else part.drawn_bars * BAR
            drawn = notes if limit is None else notes[notes["start"] < limit]
            pasted = notes[len(drawn) :] if limit is not None else notes[:0]
            velocities = set(notes["velocity"].tolist())
            one_velocity = len(velocities) == 1
            if not one_velocity:
                app.trigger("view.inspector")
            # Latest first: a press never lands on a note drawn before it.
            for note in sorted(drawn, key=lambda n: (-int(n["start"]), int(n["pitch"]))):
                roll.draw(int(note["start"]), int(note["length"]), int(note["pitch"]), grid_ticks)
                if not one_velocity and int(note["velocity"]) != 100:
                    # The note just drawn is selected: its velocity typed in the inspector.
                    set_velocity(app, int(note["velocity"]))
            if len(pasted):
                set_clipboard(fragment(pasted, part.pattern, part.channel), tmp_path)
                roll.cursor_at(int(pasted["start"].min()))
                app.trigger("edit.paste")
            app.wait_for(
                lambda s, p=part, n=len(notes): (
                    app.script(f"result = len(window.project.notes({p.pattern!r}, {p.channel!r}))")
                    == n
                ),
                timeout=30,
            )
            if one_velocity and velocities != {100}:
                # One velocity: every note selected, and it typed once.
                app.key("Ctrl+A", target="piano_roll.roll")
                app.trigger("view.inspector")
                set_velocity(app, velocities.pop())

        # Arrange: every written pattern on the playlist, after what is there.
        at = int(
            app.script(
                "i = window.project.playlist_items()\n"
                "result = int((i['start'] + i['length']).max()) if len(i) else 0"
            )
        )
        for pattern in dict.fromkeys(p.pattern for p in parts):
            if app.script(f"result = len(window.project.pattern_placements({pattern!r}))"):
                continue
            place(app, pattern, 1, at)
            app.wait_for(
                lambda _s, pat=pattern: (
                    app.script(f"result = len(window.project.pattern_placements({pat!r}))") > 0
                ),
                timeout=10,
            )
            at += int(app.script(f"result = window.project.pattern_length({pattern!r})"))
        # Mix: a channel's volume turned.
        channel_id = ensure_channel(app, parts[-1].channel)
        app.trigger("view.channel_rack")
        for _ in range(3):
            app.scroll(f"channel_rack.row:{channel_id}.volume", dy=-120)
        export(app, first_export)
        app.quit(save=True, path=song)

    for part in parts:
        diff = subprocess.run(
            [sys.executable, "-m", "adx", "diff", str(part.source), str(song)],
            capture_output=True,
            text=True,
            check=False,
            cwd=ROOT,
        )
        assert f"pattern {part.pattern}: {part.channel}:" not in diff.stdout, diff.stdout
        ours = Project.load(str(song))[0].notes(part.pattern, part.channel)
        theirs = notes_of(part)
        for field in ("start", "length", "pitch", "velocity"):
            assert sorted(ours[field].tolist()) == sorted(theirs[field].tolist()), (
                f"{part.channel}: {field}"
            )

    second_export = tmp_path / "second.wav"
    with Driver.launch(song, via=VIA, settings_dir=evidence / "settings2") as app:
        app.wait_for("project.open", timeout=30)
        export(app, second_export)
    a, _ = read_wav(first_export)
    b, _ = read_wav(second_export)
    assert float(abs(a).max()) > 0.01, "the export is silent"
    assert a.shape == b.shape and (a == b).all(), "the reopened project exports differently"
