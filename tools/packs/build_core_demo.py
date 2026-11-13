"""Writes docs/examples/core_demo.adx (phase_13.md 6): 32 bars made only from the `core`
pack - its 808 Kit, 808 Distorted, the transitions - and its pattern templates.

    python tools/packs/build_core_demo.py

Everything goes through the same calls the app makes: racks are inserted (so the file
holds ordinary channels, no rack indirection), templates are pasted as notes, and the
patterns are placed on the playlist. Only synthesized sounds are used, so the demo plays
the same on a machine without the sample libraries.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "app"))

from adx import engine_bridge  # noqa: E402
from adx.engine_bridge import Project  # noqa: E402

CORE = REPO / "engine" / "preset" / "packs" / "core"
PACKS = [str(REPO / "engine" / "preset" / "packs")]
OUT = REPO / "docs" / "examples" / "core_demo.adx"
BAR = 3840 * 4
BLANK = '[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 128\n\n[MIXER]\nINSERT 1 name="Master"\n'

#: Section -> (first bar, bars, drum template, bass on?, extra racks playing).
SECTIONS = [
    ("Intro", 0, 8, "house_offbeat_hats", False),
    ("Build", 8, 8, "four_on_floor", True),
    ("Drop", 16, 12, "four_on_floor", True),
    ("Outro", 28, 4, "half_time", False),
]

#: The bass line, a bar of (step of 16, semitones from C1, 16ths long).
BASS = [(0, 0, 3), (4, 0, 2), (7, 3, 1), (8, 0, 3), (12, 7, 2), (14, 5, 2)]


def template_notes(name: str) -> np.ndarray:
    source, _ = Project.load(str(CORE / "patterns" / f"{name}.adx"))
    pattern = source.patterns()[0]
    return source.notes(pattern, source.pattern_channels(pattern)[0])


def add_notes(project: Project, pattern: str, channels: list[str], notes: np.ndarray) -> None:
    for channel in channels:
        rows = notes.copy()
        rows["id"] = 0
        project.apply(
            engine_bridge.commands.edit_notes(
                pattern,
                channel,
                np.zeros(0, dtype=np.uint32),
                engine_bridge.empty_notes(),
                rows,
                "Notes",
            )
        )


def tiled(notes: np.ndarray, bars: int, template_bars: int) -> np.ndarray:
    parts = []
    for repeat in range(bars // template_bars):
        part = notes.copy()
        part["start"] = part["start"] + repeat * template_bars * BAR
        parts.append(part)
    return np.concatenate(parts)


def bass_notes(bars: int) -> np.ndarray:
    rows = []
    for bar in range(bars):
        for step, semis, length in BASS:
            rows.append((bar * BAR + step * 960, length * 960, 24 + semis))
    notes = np.zeros(len(rows), dtype=engine_bridge.empty_notes().dtype)
    for i, (start, length, pitch) in enumerate(rows):
        notes[i]["start"] = start
        notes[i]["length"] = length
        notes[i]["pitch"] = pitch
        notes[i]["velocity"] = 105
        notes[i]["release"] = 64
    return notes


def single(pitch: int, length: int) -> np.ndarray:
    notes = np.zeros(1, dtype=engine_bridge.empty_notes().dtype)
    notes[0]["start"] = 0
    notes[0]["length"] = length
    notes[0]["pitch"] = pitch
    notes[0]["velocity"] = 110
    notes[0]["release"] = 64
    return notes


def place(project: Project, pattern: str, bar: int) -> None:
    before = {int(t["id"]) for t in project.playlist_info()["tracks"]}
    project.apply(engine_bridge.commands.build("AddPlaylistTrackNew", pattern))
    after = {int(t["id"]) for t in project.playlist_info()["tracks"]}
    track = sorted(after - before)[0]
    pid = next(int(p["id"]) for p in project.patterns_info() if p["name"] == pattern)
    project.apply(engine_bridge.commands.build("AddPatternItem", track, pid, bar * BAR, 0))


def rack(project: Project, name: str) -> list[str]:
    before = set(project.channels())
    project.insert_rack(str(next(CORE.rglob(f"{name}.adxrack"))), PACKS)
    return [c for c in project.channels() if c not in before]


def main() -> int:
    project, _ = Project.loads(BLANK)
    kit = rack(project, "808-kit")
    bass = rack(project, "808-distorted")
    riser = rack(project, "noise-riser-8")
    impact = rack(project, "impact")
    for section, first, bars, template, with_bass in SECTIONS:
        drums = f"{section} Drums"
        project.apply(engine_bridge.commands.build("AddPatternNew", drums, bars * BAR, ""))
        source = template_notes(template)
        template_bars = max(1, int(np.ceil((source["start"].max() + 1) / BAR)))
        add_notes(project, drums, kit, tiled(source, bars, template_bars))
        place(project, drums, first)
        if with_bass:
            line = f"{section} Bass"
            project.apply(engine_bridge.commands.build("AddPatternNew", line, bars * BAR, ""))
            add_notes(project, line, bass, bass_notes(bars))
            place(project, line, first)
    project.apply(engine_bridge.commands.build("AddPatternNew", "Rise", 8 * BAR, ""))
    add_notes(project, "Rise", riser, single(60, 8 * BAR))
    place(project, "Rise", 8)
    project.apply(engine_bridge.commands.build("AddPatternNew", "Hit", 2 * BAR, ""))
    add_notes(project, "Hit", impact, single(36, BAR))
    place(project, "Hit", 16)
    text = (
        "# phase_13.md 6: 32 bars made only from the core pack (tools/packs/build_core_demo.py).\n"
    )
    OUT.write_text(text + project.dumps(), encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(REPO)}: {len(project.channels())} channels")
    return 0


if __name__ == "__main__":
    sys.exit(main())
