"""scenario_external_edit_while_playing (phase_10.5.md 4.5, 5): P7-1's stand-in.

adX plays a project. This process, standing in for an editor, saves the file 50 times:
musical edits (transpose a bar, move a phrase, rewrite a pattern's figure, change the
tempo), alternating an atomic rename (VS Code's way) with an in-place truncate and write.
After each save it waits for the reload through ``wait_for``. Then: no xruns, no detector
findings in the capture, the GUI's text equal to the file after every save, and one undo
step per reload.

Not covered: the editor's own file locking (the writes are this process's); the reload
is held to the text, not to how it sounds at the moment of the edit (the capture is
checked for glitches, not against a render, because the music changes under it).
"""

from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import sys

import numpy as np
import pytest
from wannacry import MATERIAL, VIA, project_for

from adx.engine_bridge import PPQ, Project, commands
from adx.testing.driver import Driver
from adx.testing.glitch import find_glitches
from adx.testing.wav import read_wav

pytestmark = pytest.mark.skipif(sys.platform != "win32", reason="the Windows launcher and pipe")

EDITS = 50


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _bar_ticks(project: Project) -> int:
    numerator, denominator = project.meter_at(0)
    return PPQ * 4 * numerator // denominator


def _targets(project: Project) -> list[tuple[str, str]]:
    """(pattern, channel) pairs that have notes."""
    out = []
    for pattern in project.patterns():
        for channel in project.pattern_channels(pattern):
            if len(project.notes(pattern, channel)):
                out.append((pattern, channel))
    return out


def edit(project: Project, step: int) -> str:
    """The step's musical edit, applied to ``project``; returns what it was."""
    targets = _targets(project)
    pattern, channel = targets[step % len(targets)]
    notes = project.notes(pattern, channel).copy()
    bar = _bar_ticks(project)
    bars = max(1, int(notes["start"].max()) // bar + 1)
    which = (step // 4) % bars
    in_bar = (notes["start"] >= which * bar) & (notes["start"] < (which + 1) * bar)
    if not in_bar.any():
        in_bar = notes["start"] == notes["start"].min()
    kind = step % 4
    if kind == 0:
        direction = 1 if (step // 4) % 2 == 0 else -1
        notes["pitch"][in_bar] = np.clip(notes["pitch"][in_bar].astype(int) + direction, 0, 127)
        label = f"transpose bar {which + 1} of {pattern}/{channel} by {direction:+d}"
    elif kind == 1:
        shift = PPQ // 4 if (step // 4) % 2 == 0 else -(PPQ // 4)
        moved = np.clip(notes["start"][in_bar] + shift, 0, project.pattern_length(pattern) - 1)
        notes["start"][in_bar] = moved
        label = f"move bar {which + 1} of {pattern}/{channel} by {shift:+d} ticks"
    elif kind == 2:
        notes["pitch"][in_bar] = notes["pitch"][in_bar][::-1]
        label = f"rewrite bar {which + 1} of {pattern}/{channel}: the figure reversed"
    else:
        bpm = project.bpm_at(0) + (2.0 if (step // 4) % 2 == 0 else -2.0)
        project.execute(commands.set_tempo(bpm, 0))
        return f"tempo to {bpm:g}"
    empty = np.zeros(0, dtype=notes.dtype)
    project.execute(
        commands.edit_notes(pattern, channel, np.zeros(0, np.uint32), notes[in_bar], empty, label)
    )
    return label


def save(path: pathlib.Path, text: str, step: int) -> None:
    if step % 2 == 0:
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_text(text, encoding="utf-8", newline="")
        os.replace(temporary, path)
    else:
        with path.open("w", encoding="utf-8", newline="") as handle:
            handle.write(text)


@pytest.mark.parametrize("material", MATERIAL)
def test_external_edit_while_playing(
    material: str, tmp_path: pathlib.Path, evidence: pathlib.Path
) -> None:
    song = tmp_path / "song.adx"
    shutil.copy(project_for(material), song)
    # The file in canonical form first, so the app's text and ours start equal.
    model, diagnostics = Project.load(str(song))
    assert not [d for d in diagnostics if d["severity"] == "error"]
    song.write_text(model.dumps(), encoding="utf-8", newline="")

    with Driver.launch(song, via=VIA, settings_dir=evidence / "settings") as app:
        app.wait_for("project.open", timeout=30)
        start = app.state()
        assert start["project"]["text_sha256"] == _sha(song.read_text(encoding="utf-8"))
        depth = int(start["project"]["undo_depth"])
        xruns = int(start["audio"]["xruns"])
        app.trigger("transport.play")
        app.wait_for("transport.playing", timeout=10)
        app.capture_start()
        done = []
        for step in range(EDITS):
            before = model.dumps()
            done.append(edit(model, step))
            if model.dumps() == before:  # a bar of one pitch reversed is itself: transpose it
                done.append(edit(model, step - step % 4))
            text = model.dumps()
            save(song, text, step)
            app.wait_for("project.text_sha256", predicate="equals", value=_sha(text), timeout=10)
            state = app.state()
            assert state["project"]["undo_depth"] == depth + step + 1, done[-1]
            assert state["transport"]["playing"], f"playback stopped at edit {step}: {done[-1]}"
        take = app.capture_stop(evidence / "take.wav")
        end = app.state()
        assert int(end["audio"]["xruns"]) == xruns
        assert not end["project"]["dirty"]
    audio, rate = read_wav(take["path"])
    assert take["lost"] == 0
    assert float(abs(audio).max()) > 0.01, "the capture is silent"
    findings = find_glitches(audio, rate)
    assert findings == [], [str(f) for f in findings[:5]]
