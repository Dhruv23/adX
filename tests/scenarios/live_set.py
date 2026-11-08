"""A live set for the Phase 11 gates (phase_11.md 6): a session built from a project's own
patterns, and a deterministic set script to perform on it.

Used by ``test_scripted_set.py`` (scenario_scripted_set_20min: the app through adX.cmd,
the emulated controller into loopMIDI) and ``bench/bench_live_set.py``
(live_set_no_dropouts: the Phase 10 benchmark project for 30 minutes).
"""

from __future__ import annotations

import random
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from adx.engine_bridge import Project

#: The set's size: four tracks by six scenes fits the shipped 8 x 8 grids.
TRACKS = 4
SCENES = 6
FOLLOWS = ("none", "next", "none", "repeat", "none", "previous")


def build_set(source: Path, out: Path, scene_tempo: float | None = None) -> dict[str, Any]:
    """Writes ``source`` with a [SESSION] added: the channels with the most notes as
    tracks, its patterns down the scenes (each cell the pattern's notes for that
    channel), a few follow actions, and - when ``scene_tempo`` is given - scene 5 at
    that tempo. Returns the session (session_info's shape)."""
    project, diagnostics = Project.load(str(source))
    errors = [d for d in diagnostics if d.get("severity") == "error"]
    assert not errors, errors
    counts: dict[str, int] = {}
    holds: dict[tuple[str, str], bool] = {}
    for pattern in project.patterns_info():
        name = str(pattern["name"])
        for channel in project.channels_info():
            notes = project.notes(name, str(channel["name"]))
            if len(notes):
                counts[str(channel["name"])] = counts.get(str(channel["name"]), 0) + len(notes)
                holds[(name, str(channel["name"]))] = True
    channels = sorted(counts, key=lambda c: -counts[c])[:TRACKS]
    patterns = [str(p["name"]) for p in project.patterns_info()][:SCENES]
    assert channels, f"{source} has no notes to perform"
    assert patterns, f"{source} has no patterns"
    scenes = [
        {
            "name": f"S{r + 1}",
            "tempo": scene_tempo if (scene_tempo is not None and r == 4) else None,
        }
        for r in range(len(patterns))
    ]
    tracks = []
    for t, channel in enumerate(channels):
        slots: list[dict[str, Any] | None] = []
        for r, pattern in enumerate(patterns):
            if not holds.get((pattern, channel)):
                slots.append(None)
                continue
            cell: dict[str, Any] = {"pattern": pattern, "follow": FOLLOWS[(r + t) % len(FOLLOWS)]}
            if cell["follow"] == "repeat":
                cell["repeats"] = 2
            slots.append(cell)
        tracks.append({"channel": channel, "slots": slots, "stop_on_empty": True})
    info = {"quantize": "bar", "scenes": scenes, "tracks": tracks}
    project.set_session(info)
    out.write_text(_without_arrangement(project.dumps()), encoding="utf-8")
    reread, diagnostics = Project.load(str(out))
    assert not [d for d in diagnostics if d.get("severity") == "error"], diagnostics
    return dict(reread.session_info())


def _without_arrangement(text: str) -> str:
    """The set plays only what is launched: the playlist's tracks stay (the session's
    tracks link to them), its placements go."""
    out: list[str] = []
    section = ""
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped
        elif section == "[PLAYLIST]" and line.startswith(" ") and not stripped.startswith("TRACK"):
            continue
        out.append(line)
    return "\n".join(out) + "\n"


@dataclass(frozen=True)
class Step:
    at: float
    verb: str
    args: tuple[int, ...] = ()


def set_script(minutes: float, info: dict[str, Any], seed: int = 11) -> list[Step]:
    """A set: a launch, a scene, a stop or a stop-all every few seconds, three nudges
    and a run of taps a few times over - the same every run."""
    rng = random.Random(seed)
    cells = [
        (t, r)
        for t, track in enumerate(info["tracks"])
        for r, slot in enumerate(track["slots"])
        if slot is not None
    ]
    rows = len(info["scenes"])
    steps: list[Step] = []
    at = 1.0
    end = minutes * 60.0
    count = 0
    while at < end - 5.0:
        roll = rng.random()
        if roll < 0.55:
            t, r = rng.choice(cells)
            steps.append(Step(at, "launch", (t, r, rng.randint(70, 127))))
        elif roll < 0.75:
            steps.append(Step(at, "scene", (rng.randrange(rows),)))
        elif roll < 0.88:
            steps.append(Step(at, "stop", (rng.randrange(len(info["tracks"])),)))
        elif roll < 0.92:
            steps.append(Step(at, "stopall"))
        elif roll < 0.96:
            steps.append(Step(at, "nudge", (1 if rng.random() < 0.5 else -1,)))
        else:
            for i in range(4):
                steps.append(Step(at + i * 0.5, "tap"))
        count += 1
        at += rng.uniform(2.5, 4.5)
    return steps
