"""Phase 10's analyses in the app (phase_10.md 4.1, STATE.md P10-2).

The engine runs each analysis as an :class:`~adx.engine_bridge.AnalysisJob`; this module
gathers its inputs from the project and applies a finished result. Every apply is one
undo group, so a melody extraction or a cleanup is undone in one step, and an analysis
that only reports (key, chords, tempo) writes nothing at all.

Notes go to the engine in one of two shapes: seconds on the arrangement's timeline
(key, chords, timbre - they meet audio there) or ticks within a pattern (form).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import numpy as np

from adx import engine_bridge
from adx.engine_bridge import PPQ, Project
from adx.patterns import pattern_id
from adx.session import Session

#: Instruments whose notes are not pitches: a drum machine's notes pick sounds.
UNPITCHED: frozenset[str] = frozenset({"drumsynth", "slicer", "pool"})


@dataclass(frozen=True)
class SecondsNote:
    start: float
    seconds: float
    pitch: int
    velocity: int

    def row(self) -> tuple[float, float, int, int]:
        return (self.start, self.seconds, self.pitch, self.velocity)


def _channel_ids(project: Project) -> dict[str, dict[str, Any]]:
    return {str(c["name"]): c for c in project.channels_info()}


def arrangement_notes(project: Project, channels: list[str] | None = None) -> list[SecondsNote]:
    """Every note the arrangement plays, in seconds: each pattern at each placement.

    ``channels`` limits it to those names; by default every pitched channel, which
    leaves drum machines out of a key estimate."""
    info = _channel_ids(project)
    wanted = (
        set(channels)
        if channels is not None
        else {name for name, c in info.items() if str(c["instrument"]) not in UNPITCHED}
    )
    out: list[SecondsNote] = []
    for pattern in project.patterns():
        placements = project.pattern_placements(pattern)
        if len(placements) == 0:
            continue
        for channel in project.pattern_channels(pattern):
            if channel not in wanted:
                continue
            notes = project.notes(pattern, channel)
            for start, length, offset in placements:
                for n in notes:
                    if bool(n["muted"]):
                        continue
                    local = int(n["start"]) - int(offset)
                    if local < 0 or local >= int(length):
                        continue
                    on = int(start) + local
                    off = on + int(n["length"])
                    begin = project.seconds_at(on)
                    out.append(
                        SecondsNote(
                            begin,
                            project.seconds_at(off) - begin,
                            int(n["pitch"]),
                            int(n["velocity"]),
                        )
                    )
    out.sort(key=lambda n: (n.start, n.pitch))
    return out


def pattern_tick_notes(
    project: Project, pattern: str, channel: str
) -> list[tuple[int, int, int, int]]:
    """One channel's notes in one pattern, as (start, length, pitch, velocity) ticks."""
    return [
        (int(n["start"]), int(n["length"]), int(n["pitch"]), int(n["velocity"]))
        for n in project.notes(pattern, channel)
    ]


# --- starting jobs ---------------------------------------------------------------------


def key_job(
    *, path: str | None = None, notes: list[SecondsNote] | None = None
) -> engine_bridge.AnalysisJob:
    """The key of an audio file or of notes."""
    return engine_bridge.AnalysisJob("key", _source(path, notes))


def chords_job(
    *, path: str | None = None, notes: list[SecondsNote] | None = None, frame_seconds: float = 0.5
) -> engine_bridge.AnalysisJob:
    """Chord segments (with the key and what might come next) of a file or of notes."""
    params = _source(path, notes)
    params["frame_seconds"] = frame_seconds
    return engine_bridge.AnalysisJob("chords", params)


def bpm_job(path: str) -> engine_bridge.AnalysisJob:
    """An audio file's tempo."""
    return engine_bridge.AnalysisJob("bpm", {"path": path})


def timbre_job(
    path: str, notes: list[SecondsNote], name: str = "Analyzed"
) -> engine_bridge.AnalysisJob:
    """An additive patch from a mixdown, with the notes that sound in it as ground truth."""
    return engine_bridge.AnalysisJob(
        "timbre", {"path": path, "notes": [n.row() for n in notes], "name": name}
    )


def melody_job(path: str) -> engine_bridge.AnalysisJob:
    """Notes from audio (ONNX builds only: see engine_bridge.melody_extractor_available)."""
    return engine_bridge.AnalysisJob("melody", {"path": path})


def form_job(
    project: Project, pattern: str, channel: str, *, phrase_bars: int = 4, grid: int = PPQ // 4
) -> engine_bridge.AnalysisJob:
    """Form detection and the cleaned rebuild of one channel's notes in a pattern."""
    numerator, denominator = project.meter_at(0)
    bar = PPQ * 4 * numerator // denominator
    return engine_bridge.AnalysisJob(
        "form",
        {
            "notes": pattern_tick_notes(project, pattern, channel),
            "phrase_ticks": bar * phrase_bars,
            "grid": grid,
        },
    )


def _source(path: str | None, notes: list[SecondsNote] | None) -> dict[str, Any]:
    if path:
        return {"path": path}
    if notes:
        return {"notes": [n.row() for n in notes]}
    raise ValueError("an analysis needs a file or some notes")


# --- applying results: one undo group each ---------------------------------------------


def _unique(name: str, taken: list[str]) -> str:
    if name not in taken:
        return name
    n = 2
    while f"{name} {n}" in taken:
        n += 1
    return f"{name} {n}"


def apply_timbre(session: Session, result: dict[str, Any], name: str = "Analyzed") -> str:
    """A new additive channel playing the recovered patch. Returns its name."""
    if not result.get("ok"):
        raise ValueError(result.get("error") or "the analysis did not finish")
    project = session.require()
    channel = _unique(name, project.channels())
    with session.group(f"Analyze timbre: {channel}"):
        session.build_and_execute("AddChannelNew", channel, "additive", 0, "")
        cid = int(_channel_ids(session.require())[channel]["id"])
        for param, value in result["params"]:
            session.build_and_execute("SetChannelParam", cid, str(param), float(value))
    return channel


def apply_melody(session: Session, result: dict[str, Any], name: str = "Melody") -> tuple[str, str]:
    """The extracted notes as a new channel and pattern, placed at the start on a new
    playlist track. Returns (channel, pattern)."""
    if not result.get("ok"):
        raise ValueError(result.get("error") or "the analysis did not finish")
    project = session.require()
    rows = [
        (
            project.ticks_at_seconds(float(start)),
            project.ticks_at_seconds(float(start) + float(seconds)),
            int(pitch),
            int(velocity),
        )
        for start, seconds, pitch, velocity in result["notes"]
    ]
    channel = _unique(name, project.channels())
    pattern = _unique(name, project.patterns())
    end = max((off for _on, off, _p, _v in rows), default=PPQ * 4)
    bar = PPQ * 4
    length = max(bar, -(-end // bar) * bar)
    with session.group(f"Extract melody: {pattern}"):
        session.build_and_execute("AddChannelNew", channel, "additive", 0, "")
        session.build_and_execute("AddPatternNew", pattern, length, "")
        _add_notes(
            session, pattern, channel, [(on, max(1, off - on), p, v) for on, off, p, v in rows]
        )
        _place(session, pattern)
    return channel, pattern


def apply_form(
    session: Session,
    pattern: str,
    channel: str,
    result: dict[str, Any],
    *,
    rebuild: bool = True,
    markers: bool = True,
) -> None:
    """Replace the channel's notes with the cleaned rebuild and/or put a marker at each
    section of the observed form ("A", "B"...), at the pattern's first placement."""
    if not result.get("ok"):
        raise ValueError(result.get("error") or "the analysis did not finish")
    project = session.require()
    with session.group(f"Clean up {pattern} ({result['name']})"):
        if rebuild:
            current = project.notes(pattern, channel)
            add = engine_bridge.empty_notes(len(result["notes"]))
            for i, (start, length, pitch, velocity) in enumerate(result["notes"]):
                add[i]["start"] = start
                add[i]["length"] = length
                add[i]["pitch"] = pitch
                add[i]["velocity"] = velocity
            session.execute(
                engine_bridge.commands.edit_notes(
                    pattern,
                    channel,
                    current["id"].astype(np.uint32),
                    engine_bridge.empty_notes(0),
                    add,
                    "Clean up",
                )
            )
            needed = max((int(s) + int(n) for s, n, _p, _v in result["notes"]), default=0)
            if needed > project.pattern_length(pattern):
                pid = pattern_id(session.require(), pattern)
                session.build_and_execute("SetPatternLength", pid, needed)
        if markers:
            placements = session.require().pattern_placements(pattern)
            origin = int(placements[0][0]) if len(placements) else 0
            for tick, label in result["markers"]:
                session.execute(engine_bridge.commands.add_marker(origin + int(tick), f"{label}"))


def apply_chord(
    session: Session,
    pattern: str,
    channel: str,
    tones: list[int],
    at: int,
    length: int,
    octave: int = 4,
) -> None:
    """One suggested chord written into a pattern: its pitch classes stacked upward from
    the root in ``octave``. One undo step."""
    base = 12 * (octave + 1)
    pitches: list[int] = []
    for pc in tones:
        pitch = base + int(pc)
        while pitches and pitch <= pitches[-1]:
            pitch += 12
        pitches.append(pitch)
    with session.group("Add chord"):
        _add_notes(session, pattern, channel, [(at, length, p, 100) for p in pitches])


def _add_notes(
    session: Session, pattern: str, channel: str, rows: list[tuple[int, int, int, int]]
) -> None:
    if not rows:
        return
    add = engine_bridge.empty_notes(len(rows))
    for i, (start, length, pitch, velocity) in enumerate(rows):
        add[i]["start"] = start
        add[i]["length"] = length
        add[i]["pitch"] = pitch
        add[i]["velocity"] = velocity
    session.execute(
        engine_bridge.commands.edit_notes(
            pattern,
            channel,
            np.zeros(0, dtype=np.uint32),
            engine_bridge.empty_notes(0),
            add,
            "Add notes",
        )
    )


def _place(session: Session, pattern: str) -> None:
    """On a new playlist track, at the start."""
    before = {int(t["id"]) for t in session.require().playlist_info()["tracks"]}
    session.build_and_execute("AddPlaylistTrackNew", "")
    after = {int(t["id"]) for t in session.require().playlist_info()["tracks"]}
    new = sorted(after - before)
    track = new[0] if new else max(after)
    session.build_and_execute("AddPatternItem", track, pattern_id(session.require(), pattern), 0, 0)
