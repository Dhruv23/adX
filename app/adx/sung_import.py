"""Singing-voice project import (phase_9.md 4.1, STATE.md P9-6): UTAU's UST, OpenUtau's
USTX and Vocaloid's VSQX become ordinary notes with lyrics, for the Voice instrument.

UST and USTX carry pitch curves too (UTAU's Mode2 points, OpenUtau's pitch data): each
becomes the note's pitch curve, in cents from its own pitch. VSQX is notes and lyrics
only - adX cannot sing a Vocaloid voice, so its channel is given a Voice or plugin
instrument by the user.

The files are read here rather than in the engine because UST is usually Shift-JIS,
which Python decodes and portable C++ does not; the notes then go through the same
import the MIDI files use (``import_song``), as one undo step.
"""

from __future__ import annotations

import configparser
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from typing import Any

from adx import engine_bridge

#: adX's ticks per quarter note.
PPQ = engine_bridge.PPQ
#: What UST and most USTX files count in.
UTAU_RESOLUTION = 480

Song = dict[str, Any]


def _decode(raw: bytes) -> str:
    for encoding in ("utf-8-sig", "cp932"):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            continue
    return raw.decode("latin-1")


def _ticks(value: float, resolution: int) -> int:
    return round(value * PPQ / max(1, resolution))


def _ms_to_ticks(ms: float, bpm: float) -> int:
    return round(ms / 1000.0 * bpm / 60.0 * PPQ)


def _curve(points: list[tuple[float, float]], bpm: float, length: int) -> list[tuple[int, int]]:
    """(ms from the note's start, cents) points as note-relative (tick, cents), clipped
    to the note: a point before its start becomes the value the line has at 0."""
    if not points:
        return []
    ticked = sorted((_ms_to_ticks(ms, bpm), cents) for ms, cents in points)
    out: list[tuple[int, int]] = []
    for i, (tick, cents) in enumerate(ticked):
        if tick < 0:
            nxt = ticked[i + 1] if i + 1 < len(ticked) else None
            if nxt is not None and nxt[0] > 0:
                t = -tick / (nxt[0] - tick)
                out.append((0, round(cents + (nxt[1] - cents) * t)))
            continue
        if tick >= length:
            break
        if out and out[-1][0] == tick:
            out[-1] = (tick, round(cents))
        else:
            out.append((tick, round(cents)))
    if all(c == 0 for _, c in out):
        return []
    return out


# --- UST --------------------------------------------------------------------------------


def read_ust(path: str | Path) -> Song:
    """A UTAU sequence: [#SETTING] Tempo, then one [#NNNN] section per note (R is a
    rest), Length in 480ths, NoteNum, Lyric, a Tempo where it changes, and Mode2 pitch
    points (PBS start;y, PBW widths, PBY heights, all y in tenths of a semitone)."""
    text = _decode(Path(path).read_bytes())
    # "[#VERSION]" holds a bare line; configparser wants keys, so give it none.
    parser = configparser.ConfigParser(interpolation=None, strict=False, allow_no_value=True)
    parser.optionxform = str  # type: ignore[assignment, method-assign]
    parser.read_string(text)
    bpm = float(parser.get("#SETTING", "Tempo", fallback="120").replace(",", ".") or 120)
    tempos: list[tuple[int, float]] = [(0, bpm)]
    notes: list[dict[str, Any]] = []
    at = 0
    for section in parser.sections():
        if not (section.startswith("#") and section[1:].isdigit()):
            continue
        fields = parser[section]
        length = _ticks(float(fields.get("Length", "480")), UTAU_RESOLUTION)
        if "Tempo" in fields and fields["Tempo"].strip():
            changed = float(fields["Tempo"].replace(",", "."))
            if changed != bpm:
                bpm = changed
                tempos.append((at, bpm))
        lyric = fields.get("Lyric", "").strip()
        if lyric not in ("R", "r", ""):
            note: dict[str, Any] = {
                "start": at,
                "length": length,
                "pitch": int(fields.get("NoteNum", "60")),
                "velocity": 100,
                "lyric": lyric,
            }
            points = _mode2_points(fields)
            bend = _curve(points, bpm, length)
            if bend:
                note["bend"] = bend
            notes.append(note)
        at += length
    name = parser.get("#SETTING", "ProjectName", fallback="") or Path(path).stem
    return {"tempos": tempos, "tracks": [{"name": name, "notes": notes}]}


def _numbers(value: str) -> list[float]:
    out: list[float] = []
    for part in value.split(","):
        part = part.strip()
        try:
            out.append(float(part) if part else 0.0)
        except ValueError:
            out.append(0.0)
    return out


def _mode2_points(fields: configparser.SectionProxy) -> list[tuple[float, float]]:
    """UTAU's Mode2 pitch: the first point at PBS (ms;y), each next one PBW ms further,
    at the PBY heights in order and back to 0 at the last. y is in 10-cent steps."""
    if "PBS" not in fields or "PBW" not in fields:
        return []
    start = fields["PBS"].replace(";", ",").split(",")
    x = float(start[0] or 0)
    first = float(start[1]) if len(start) > 1 and start[1].strip() else 0.0
    widths = _numbers(fields["PBW"])
    heights = _numbers(fields.get("PBY", ""))
    points = [(x, first * 10.0)]
    for i, width in enumerate(widths):
        x += width
        y = heights[i] if i < len(heights) else 0.0
        points.append((x, y * 10.0))
    return points


# --- USTX -------------------------------------------------------------------------------


def read_ustx(path: str | Path) -> Song:
    """An OpenUtau project: tempos (or the older single bpm), voice parts at a position,
    notes at a position relative to their part with tone, lyric and pitch data (x in ms
    from the note's start, y in tenths of a semitone). One track per track number."""
    import yaml  # type: ignore[import-untyped]  # PyYAML ships no stubs; it is read, not typed

    data = yaml.safe_load(_decode(Path(path).read_bytes())) or {}
    resolution = int(data.get("resolution", UTAU_RESOLUTION))
    tempos_raw = data.get("tempos") or [{"position": 0, "bpm": data.get("bpm", 120)}]
    tempos = [
        (_ticks(float(t.get("position", 0)), resolution), float(t.get("bpm", 120)))
        for t in tempos_raw
    ]
    tempos.sort()

    def bpm_at(tick: int) -> float:
        current = tempos[0][1] if tempos else 120.0
        for at, bpm in tempos:
            if at > tick:
                break
            current = bpm
        return current

    names = [str(t.get("track_name") or t.get("singer") or "") for t in data.get("tracks") or []]
    by_track: dict[int, list[dict[str, Any]]] = {}
    for part in data.get("voice_parts") or []:
        track_no = int(part.get("track_no", 0))
        offset = float(part.get("position", 0))
        for raw in part.get("notes") or []:
            start = _ticks(offset + float(raw.get("position", 0)), resolution)
            length = max(1, _ticks(float(raw.get("duration", resolution)), resolution))
            note: dict[str, Any] = {
                "start": start,
                "length": length,
                "pitch": int(raw.get("tone", 60)),
                "velocity": 100,
                "lyric": str(raw.get("lyric", "")),
            }
            pitch = raw.get("pitch") or {}
            points = [
                (float(p.get("x", 0)), float(p.get("y", 0)) * 10.0) for p in pitch.get("data") or []
            ]
            bend = _curve(points, bpm_at(start), length)
            if bend:
                note["bend"] = bend
            by_track.setdefault(track_no, []).append(note)
    tracks: list[dict[str, Any]] = []
    for track_no in sorted(by_track):
        name = (
            names[track_no]
            if track_no < len(names) and names[track_no]
            else f"Voice {track_no + 1}"
        )
        tracks.append({"name": name, "notes": sorted(by_track[track_no], key=lambda n: n["start"])})
    return {"tempos": tempos or [(0, 120.0)], "tracks": tracks}


# --- VSQX -------------------------------------------------------------------------------


def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _child(element: ElementTree.Element, *names: str) -> ElementTree.Element | None:
    for child in element:
        if _local(child.tag) in names:
            return child
    return None


def _text(element: ElementTree.Element, *names: str, default: str = "") -> str:
    found = _child(element, *names)
    return (found.text or default).strip() if found is not None and found.text else default


def read_vsqx(path: str | Path) -> Song:
    """A Vocaloid 3 or 4 project: notes and lyrics only. VSQ4 spells fields t, dur, n,
    v, y; VSQ3 posTick, durTick, noteNum, velocity, lyric. Tempo is bpm times 100."""
    root = ElementTree.fromstring(Path(path).read_bytes())
    master = _child(root, "masterTrack")
    resolution = int(_text(master, "resolution", default="480")) if master is not None else 480
    tempos: list[tuple[int, float]] = []
    if master is not None:
        for tempo in master:
            if _local(tempo.tag) != "tempo":
                continue
            at = int(_text(tempo, "t", "posTick", default="0"))
            value = float(_text(tempo, "v", "bpm", default="12000"))
            tempos.append((_ticks(at, resolution), value / 100.0))
    tracks: list[dict[str, Any]] = []
    for track in root:
        if _local(track.tag) != "vsTrack":
            continue
        name = _text(track, "name", "trackName") or f"Voice {len(tracks) + 1}"
        notes: list[dict[str, Any]] = []
        for part in track:
            if _local(part.tag) not in ("vsPart", "musicalPart"):
                continue
            offset = int(_text(part, "t", "posTick", default="0"))
            for note in part:
                if _local(note.tag) != "note":
                    continue
                notes.append(
                    {
                        "start": _ticks(
                            offset + int(_text(note, "t", "posTick", default="0")), resolution
                        ),
                        "length": max(
                            1, _ticks(int(_text(note, "dur", "durTick", default="480")), resolution)
                        ),
                        "pitch": int(_text(note, "n", "noteNum", default="60")),
                        "velocity": max(
                            1, min(127, int(_text(note, "v", "velocity", default="64")))
                        ),
                        "lyric": _text(note, "y", "lyric"),
                    }
                )
        tracks.append({"name": name, "notes": sorted(notes, key=lambda n: n["start"])})
    return {"tempos": sorted(tempos) or [(0, 120.0)], "tracks": tracks}


READERS = {".ust": read_ust, ".ustx": read_ustx, ".vsqx": read_vsqx}


def read_song(path: str | Path) -> Song:
    """Whichever of the three ``path`` is, by its extension."""
    reader = READERS.get(Path(path).suffix.lower())
    if reader is None:
        raise ValueError(f"not a UST, USTX or VSQX file: {path}")
    return reader(path)


def import_sung(project: engine_bridge.Project, path: str | Path) -> dict[str, Any]:
    """``path`` into ``project`` as one undo step: a channel per track, one pattern of
    their notes (lyrics and pitch curves kept) placed at the start, and the tempo."""
    song = read_song(path)
    return engine_bridge.import_song(project, song, Path(path).stem)
