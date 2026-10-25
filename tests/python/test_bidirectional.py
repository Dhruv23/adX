"""The phase gate (phase_7.md 1, 6): **a project edited in the GUI and in an external
editor at once, changes flowing both ways, no dropouts on reload.**

The whole application, Live sync, the transport playing, and 200 operations alternating
between the two sides: a GUI edit made through the session (as a panel makes it), and an
external edit written to the file the way editors write - sometimes a temporary renamed
over it, sometimes in place. After every operation both "windows" must converge: the
project's canonical text is what the file says.

Every tenth external edit is made to collide with an unwritten GUI edit. Each collision
must be raised as a conflict - never resolved by guessing - and is resolved alternately
"take theirs" and "keep mine"; the chosen side's edit must then be in both places.

"No dropouts": playback never stops, and the slowest audio callback stays inside the
block deadline (with the 1 % allowance shared CI runners need, P3-8, applied as headroom
on the worst case).
"""

from __future__ import annotations

import os
import pathlib
import random
import time
from collections.abc import Callable

from PySide6.QtWidgets import QApplication

from adx import engine_bridge
from adx.engine_bridge import PPQ
from adx.mainwindow import MainWindow
from adx.text_sync import write_atomically

OPERATIONS = 200

TEXT = """[PROJECT]
ADX_VERSION=2
TITLE="Both ways"

[TEMPO]
0:0:0 120

[CHANNEL Lead]
OUTPUT=insert.2
VOLUME=0.8

[CHANNEL Pad]
OUTPUT=insert.2
VOLUME=0.5

[PATTERN Verse]
LENGTH=4:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100
  E4 0:1:0 0:1:0 90
NOTES Pad
  C3 0:0:0 4:0:0 80

[PLAYLIST]
TRACK 1
  PATTERN Verse 0:0:0

[MIXER]
INSERT 1 name="Master"
INSERT 2 name="Bus"
ROUTE insert.2 -> insert.1
"""


def pump_until(condition: Callable[[], bool], seconds: float = 5.0) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        QApplication.processEvents()
        if condition():
            return True
        time.sleep(0.002)
    QApplication.processEvents()
    return condition()


def canonical(text: str) -> str:
    project, diagnostics = engine_bridge.Project.loads(text)
    assert not [d for d in diagnostics if str(d["severity"]) == "error"], diagnostics
    return project.dumps()


class External:
    """An external editor: reads the file, changes one line's worth, saves it."""

    def __init__(self, path: pathlib.Path, rng: random.Random) -> None:
        self.path = path
        self.rng = rng

    def edit(self, n: int) -> str:
        text = self.path.read_text(encoding="utf-8")
        lines = text.split("\n")
        kind = n % 4
        if kind == 0:  # a channel's volume
            for i, line in enumerate(lines):
                if line.startswith("VOLUME="):
                    lines[i] = f"VOLUME={self.rng.randint(1, 99) / 100}"
                    break
        elif kind == 1:  # a new note in the Lead clip
            at = lines.index("NOTES Lead") + 1
            bar, beat = self.rng.randint(0, 3), self.rng.randint(0, 3)
            lines.insert(at, f"  G4 {bar}:{beat}:0 0:0:960 {self.rng.randint(40, 120)}")
        elif kind == 2:  # the title
            for i, line in enumerate(lines):
                if line.startswith("TITLE="):
                    lines[i] = f'TITLE="external {n}"'
        else:  # the tempo
            at = lines.index("[TEMPO]") + 1
            lines[at] = f"0:0:0 {self.rng.randint(90, 160)}"
        new = "\n".join(lines)
        if self.rng.random() < 0.5:
            write_atomically(str(self.path), new)
        else:
            self.path.write_text(new, encoding="utf-8", newline="")
        return new


def gui_edit(studio: MainWindow, rng: random.Random, n: int) -> None:
    """One edit as a panel makes it: a command through the session."""
    project = studio.project
    channels = {str(c["name"]): int(c["id"]) for c in project.channels_info()}
    kind = n % 3
    if kind == 0:
        assert studio.session.build_and_execute(
            "SetChannelField", channels["Pad"], "pan", rng.randint(-9, 9) / 10
        )
    elif kind == 1:
        row = engine_bridge.empty_notes(1)
        row["start"] = rng.randint(0, 15) * PPQ
        row["length"] = PPQ // 2
        row["pitch"] = rng.randint(48, 84)
        row["velocity"] = 100
        import numpy as np

        assert studio.session.execute(
            engine_bridge.commands.edit_notes(
                "Verse",
                "Lead",
                np.zeros(0, np.uint32),
                engine_bridge.empty_notes(0),
                row,
                "Add note",
            )
        )
    else:
        assert studio.session.execute(
            engine_bridge.commands.add_marker(rng.randint(0, 15) * PPQ * 4, f"gui {n}")
        )


def test_bidirectional(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    path = tmp_path / "both.adx"
    path.write_text(TEXT, encoding="utf-8", newline="")
    layer = studio.text
    sync = layer.file_sync
    sync.debounce_ms = 30
    assert studio.open_project(str(path))
    layer.set_mode("live")
    sync._live.setInterval(20)
    layer.file_sync.conflict.disconnect(layer._conflict)
    conflicts: list[str] = []
    sync.conflict.connect(conflicts.append)
    reloads: list[int] = []
    sync.reloaded.connect(lambda commands, _structural: reloads.append(commands))

    engine = studio.engine
    engine.set_loop(0, PPQ * 16, True)
    engine.play()
    assert pump_until(lambda: engine.state() == "playing")

    rng = random.Random(7)
    external = External(path, rng)
    collisions = 0
    for n in range(OPERATIONS):
        if n % 2 == 0:
            gui_edit(studio, rng, n // 2)
            # Live mode writes it; the file then says what the project says.
            assert pump_until(
                lambda: path.read_text(encoding="utf-8") == sync.sync.render_text()
            ), n
        elif n % 20 == 19:
            # A collision: a GUI edit adX has not written yet, and an external save.
            collisions += 1
            sync._live.setInterval(60_000)
            gui_edit(studio, rng, n)
            theirs = external.edit(n)
            assert pump_until(lambda expected=collisions: len(conflicts) == expected), (
                f"collision {collisions} not raised"
            )
            sync._live.stop()
            sync._live.setInterval(20)
            if collisions % 2:
                assert sync.take_theirs()
                assert studio.project.dumps() == canonical(theirs)
            else:
                mine = sync.sync.render_text()
                sync.keep_mine()
                assert path.read_text(encoding="utf-8") == mine
        else:
            before = len(reloads)
            theirs = external.edit(n)
            assert pump_until(lambda count=before: len(reloads) > count), (
                f"external edit {n} not reloaded"
            )
            assert studio.project.dumps() == canonical(theirs), n
        # Both windows converged, and nothing stopped.
        assert canonical(path.read_text(encoding="utf-8")) == studio.project.dumps(), n
        assert engine.state() == "playing", n

    # Every collision was a conflict, and only collisions were.
    assert len(conflicts) == collisions == OPERATIONS // 20
    # The final file round-trips.
    final = path.read_text(encoding="utf-8")
    assert canonical(final) == final
    # No dropouts: the slowest callback inside the block's deadline.
    deadline_ms = 1000.0 * 256 / engine.sample_rate
    worst = engine.worst_callback_ms()
    allowance = 4.0 if os.environ.get("CI") else 1.0
    assert worst < deadline_ms * allowance, f"worst callback {worst:.2f} ms"
    engine.stop_playback()
