"""UST, USTX and VSQX import (STATE.md P9-6, phase_9.md 4.1): notes with lyrics, and
UST's and USTX's pitch points as each note's pitch curve."""

from __future__ import annotations

import pathlib

import pytest

from adx import sung_import
from adx.engine_bridge import PPQ, Project

UST = """[#VERSION]
UST Version1.2
[#SETTING]
Tempo=120.00
Tracks=1
ProjectName=Teto test
Mode2=True
[#0000]
Length=480
Lyric=R
NoteNum=60
[#0001]
Length=480
Lyric=て
NoteNum=69
PBS=-50;-20
PBW=100,150
PBY=10
[#0002]
Length=960
Lyric=と
NoteNum=71
Tempo=60.00
[#TRACKEND]
"""

USTX = """name: OpenUtau test
ustx_version: 0.6
resolution: 480
tempos:
- position: 0
  bpm: 120
tracks:
- singer: Teto
  track_name: Lead
- singer: Teto
  track_name: Harmony
voice_parts:
- name: Part 1
  track_no: 0
  position: 1920
  notes:
  - position: 0
    duration: 480
    tone: 64
    lyric: la
    pitch:
      data:
      - {x: 0, y: 0, shape: io}
      - {x: 250, y: 20, shape: io}
      snap_first: true
  - position: 480
    duration: 480
    tone: 65
    lyric: li
- name: Part 2
  track_no: 1
  position: 0
  notes:
  - position: 0
    duration: 960
    tone: 60
    lyric: lu
"""

VSQ4 = """<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<vsq4 xmlns="http://www.yamaha.co.jp/vocaloid/schema/vsq4/">
  <masterTrack><resolution>480</resolution>
    <tempo><t>0</t><v>14000</v></tempo>
  </masterTrack>
  <vsTrack><tNo>0</tNo><name>Miku</name>
    <vsPart><t>480</t>
      <note><t>0</t><dur>240</dur><n>62</n><v>64</v><y>a</y></note>
      <note><t>240</t><dur>240</dur><n>64</n><v>90</v><y>i</y></note>
    </vsPart>
  </vsTrack>
</vsq4>
"""

VSQ3 = """<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<vsq3 xmlns="http://www.yamaha.co.jp/vocaloid/schema/vsq3/">
  <masterTrack><resolution>480</resolution>
    <tempo><posTick>0</posTick><bpm>9000</bpm></tempo>
  </masterTrack>
  <vsTrack><vsTrackNo>0</vsTrackNo><trackName>Luka</trackName>
    <musicalPart><posTick>0</posTick>
      <note><posTick>960</posTick><durTick>480</durTick><noteNum>57</noteNum><velocity>70</velocity><lyric>u</lyric></note>
    </musicalPart>
  </vsTrack>
</vsq3>
"""


def _empty() -> Project:
    project, _ = Project.loads('[PROJECT]\nADX_VERSION=2\n\n[MIXER]\nINSERT 1 name="Master"\n')
    return project


def _clip(project: Project, pattern: str, channel: str) -> list[tuple[int, int, int, dict]]:
    notes = project.notes(pattern, channel)
    return [
        (
            int(n["start"]),
            int(n["length"]),
            int(n["pitch"]),
            project.note_extras(pattern, channel, int(n["id"])),
        )
        for n in sorted(notes, key=lambda n: int(n["start"]))
    ]


def test_ust_notes_lyrics_tempo_and_pitch(tmp_path: pathlib.Path) -> None:
    path = tmp_path / "teto.ust"
    path.write_bytes(UST.encode("cp932"))  # UTAU's own encoding
    project = _empty()
    result = sung_import.import_sung(project, path)
    pattern = str(result["pattern"])
    notes = _clip(project, pattern, "Teto test")
    assert [(s, ln, p) for s, ln, p, _ in notes] == [(PPQ, PPQ, 69), (2 * PPQ, 2 * PPQ, 71)]
    assert notes[0][3]["lyric"] == "て"
    assert notes[1][3]["lyric"] == "と"
    # Mode2: -200 cents 50 ms before the note, 100 cents at 50 ms, back to 0 at 200 ms.
    # At 120 bpm 50 ms is 384 ticks: clipped at 0 to the line's value there, -50 cents.
    bend = [(at, cents) for at, cents, _kind in notes[0][3]["bend"]]
    assert bend[0] == (0, -50)
    assert (384, 100) in bend
    assert bend[-1] == (1536, 0)
    assert notes[1][3]["bend"] == []
    # The tempo change on the third note is in the map.
    assert "60" in project.dumps()


def test_ustx_parts_tracks_and_pitch(tmp_path: pathlib.Path) -> None:
    path = tmp_path / "song.ustx"
    path.write_text(USTX, encoding="utf-8")
    project = _empty()
    pattern = str(sung_import.import_sung(project, path)["pattern"])
    lead = _clip(project, pattern, "Lead")
    assert [(s, p) for s, _ln, p, _ in lead] == [(4 * PPQ, 64), (5 * PPQ, 65)]
    assert [e["lyric"] for *_rest, e in lead] == ["la", "li"]
    # 250 ms at 120 bpm is half a beat: +200 cents there.
    assert [(at, cents) for at, cents, _ in lead[0][3]["bend"]] == [(0, 0), (PPQ // 2, 200)]
    harmony = _clip(project, pattern, "Harmony")
    assert [(s, ln, p) for s, ln, p, _ in harmony] == [(0, 2 * PPQ, 60)]


@pytest.mark.parametrize(
    ("text", "name", "expected", "bpm"),
    [
        (VSQ4, "Miku", [(PPQ, PPQ // 2, 62, "a"), (PPQ + PPQ // 2, PPQ // 2, 64, "i")], "140"),
        (VSQ3, "Luka", [(2 * PPQ, PPQ, 57, "u")], "90"),
    ],
)
def test_vsqx_notes_and_lyrics(
    tmp_path: pathlib.Path,
    text: str,
    name: str,
    expected: list[tuple[int, int, int, str]],
    bpm: str,
) -> None:
    path = tmp_path / "song.vsqx"
    path.write_text(text, encoding="utf-8")
    project = _empty()
    pattern = str(sung_import.import_sung(project, path)["pattern"])
    got = [(s, ln, p, e["lyric"]) for s, ln, p, e in _clip(project, pattern, name)]
    assert got == expected
    assert bpm in project.dumps()


def test_import_is_one_undo_step(tmp_path: pathlib.Path) -> None:
    path = tmp_path / "song.ustx"
    path.write_text(USTX, encoding="utf-8")
    project = _empty()
    before = project.dumps()
    sung_import.import_sung(project, path)
    assert project.undo()
    assert project.dumps() == before


def test_unknown_extension_is_refused(tmp_path: pathlib.Path) -> None:
    with pytest.raises(ValueError, match="not a UST"):
        sung_import.read_song(tmp_path / "song.txt")


def test_lyrics_round_trip_through_midi(tmp_path: pathlib.Path) -> None:
    """Exported as SMF lyric events, and read back onto the same notes."""
    from adx import engine_bridge

    path = tmp_path / "song.ustx"
    path.write_text(USTX, encoding="utf-8")
    project = _empty()
    sung_import.import_sung(project, path)
    midi = tmp_path / "song.mid"
    engine_bridge.export_midi(project, str(midi))
    again = _empty()
    pattern = str(engine_bridge.import_midi(again, str(midi))["pattern"])
    lead = _clip(again, pattern, "Lead")
    assert [e["lyric"] for *_rest, e in lead] == ["la", "li"]
