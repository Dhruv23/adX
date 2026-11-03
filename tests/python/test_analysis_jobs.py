"""Phase 10's analyses from the app (phase_10.md 4.1 and 6, STATE.md P10-2): every one a
job with progress and cancel, every apply one undo step, a cancelled job leaving the
project untouched."""

from __future__ import annotations

import pathlib
import time

import pytest

from adx import analysis, engine_bridge
from adx.engine_bridge import Project
from adx.mainwindow import MainWindow

#: A I-V-vi-IV in C major, a bar each, chords in the middle octave and the root below.
_PROGRESSION = [(0, (60, 64, 67)), (1, (59, 62, 67)), (2, (57, 60, 64)), (3, (57, 60, 65))]


def _progression_text(bars: int = 8, tempo: float = 120.0) -> str:
    roots = {0: 48, 1: 43, 2: 45, 3: 41}
    lines = []
    for b in range(bars):
        step, chord = _PROGRESSION[b % 4]
        for pitch in (*chord, roots[step]):
            lines.append(f"  {pitch} {b}:0:0 1:0:0 100")
    body = "\n".join(lines)
    return f"""[PROJECT]
ADX_VERSION=2
TUNING=440

[TEMPO]
0:0:0 {tempo}

[METER]
0:0:0 4/4

[CHANNEL Keys]
INSTRUMENT=additive
OUTPUT=insert.1
PARAM harmonic.2=0.4
PARAM harmonic.3=0.25
PARAM env.attack=0.01
PARAM env.sustain=0.9

[PATTERN P1]
LENGTH={bars}:0:0
NOTES Keys
{body}

[PLAYLIST]
TRACK 1
  PATTERN P1 0:0:0

[MIXER]
INSERT 1 name="Master"
"""


def _render(project: Project, path: pathlib.Path) -> str:
    result = engine_bridge.export_project(
        project, {"output": str(path), "format": "wav24", "tail": 1.0}
    )
    assert result["ok"], result
    return str(path)


def _wait(job: engine_bridge.AnalysisJob, seconds: float = 60.0) -> dict[str, object]:
    deadline = time.monotonic() + seconds
    while not job.done and time.monotonic() < deadline:
        time.sleep(0.01)
    return job.result()


def test_key_and_chords_from_notes() -> None:
    project, diagnostics = Project.loads(_progression_text())
    assert not [d for d in diagnostics if d["severity"] == "error"]
    notes = analysis.arrangement_notes(project)
    assert len(notes) == 32
    assert notes[0].start == 0.0
    assert notes[4].start == pytest.approx(2.0)  # bar 2 at 120 bpm

    key = _wait(analysis.key_job(notes=notes))
    assert key["ok"]
    assert key["key"]["name"] in ("C major", "A minor")  # type: ignore[index]
    assert len(key["key"]["ranked"]) == 24  # type: ignore[index]

    chords = _wait(analysis.chords_job(notes=notes, frame_seconds=0.5))
    assert chords["ok"]
    names = [s["chord"] for s in chords["segments"]]  # type: ignore[union-attr]
    assert names[:4] == ["C", "G", "Am", "F"]
    assert chords["segments"][0]["end"] == pytest.approx(2.0)  # type: ignore[index]
    assert chords["suggestions"]  # what might follow the last F


def test_key_tempo_chords_from_audio(tmp_path: pathlib.Path) -> None:
    project, _ = Project.loads(_progression_text(bars=16))
    path = _render(project, tmp_path / "progression.wav")
    key = _wait(analysis.key_job(path=path))
    assert key["ok"]
    assert key["key"]["name"] in ("C major", "A minor")  # type: ignore[index]
    chords = _wait(analysis.chords_job(path=path))
    assert chords["ok"]
    roots = [s["chord"] for s in chords["segments"] if s["chord"] != "N"]  # type: ignore[union-attr]
    assert roots[:4] == ["C", "G", "Am", "F"], roots
    bpm = _wait(analysis.bpm_job(path))
    assert bpm["ok"]


def test_suggest_and_scale() -> None:
    suggestions = engine_bridge.suggest_chords("C major", "G")
    assert suggestions[0]["chord"] == "C"
    assert all(s["score"] <= suggestions[0]["score"] for s in suggestions)
    assert engine_bridge.scale_of("A minor") == [9, 11, 0, 2, 4, 5, 7]
    with pytest.raises(ValueError, match="unknown key"):
        engine_bridge.suggest_chords("H major")


def test_bad_requests_are_refused() -> None:
    with pytest.raises(ValueError, match="unknown analysis"):
        engine_bridge.AnalysisJob("loudness", {})
    with pytest.raises(ValueError, match="needs the notes"):
        engine_bridge.AnalysisJob("timbre", {"path": "x.wav"})
    with pytest.raises(ValueError, match="needs phrase_ticks"):
        engine_bridge.AnalysisJob("form", {"notes": []})
    missing = engine_bridge.analyze("bpm", {"path": "no/such/file.wav"})
    assert not missing["ok"]
    assert missing["error"]


def test_melody_extractor_hidden_without_onnx(studio: MainWindow) -> None:
    """melody_extractor_onnx_off's UI half: with the flag off the entry is not there."""
    available = engine_bridge.melody_extractor_available()
    panel = studio.analysis_panel
    assert panel.melody_tab_shown() == available
    if not available:
        with pytest.raises(ValueError, match="no melody extractor"):
            analysis.melody_job("anything.wav")


def test_timbre_applies_as_one_step(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    project, _ = Project.loads(_progression_text())
    path = _render(project, tmp_path / "keys.wav")
    notes = analysis.arrangement_notes(project)
    result = _wait(analysis.timbre_job(path, notes, "Keys clone"))
    assert result["ok"], result
    assert result["notes_measured"] > 0
    harmonics = result["harmonics"]
    assert harmonics[0] == pytest.approx(1.0)  # type: ignore[index]

    session = studio.session
    before_text = session.require().dumps()
    depth = session.require().undo_depth()
    channel = analysis.apply_timbre(session, result, "Keys clone")
    assert channel in session.require().channels()
    assert session.require().undo_depth() == depth + 1
    assert session.undo()
    assert session.require().dumps() == before_text


def test_form_cleanup_applies_as_one_step(studio: MainWindow) -> None:
    session = studio.session
    project, _ = Project.loads(_progression_text(bars=16))
    session.open(project)
    before_text = session.require().dumps()
    depth = session.require().undo_depth()
    result = _wait(analysis.form_job(session.require(), "P1", "Keys", phrase_bars=4))
    assert result["ok"], result
    assert result["form"] == "AAAA"
    assert result["name"] == "Strophic"
    analysis.apply_form(session, "P1", "Keys", result)
    assert session.require().undo_depth() == depth + 1
    assert [m[2] for m in session.require().markers()] == ["A"]
    assert session.undo()
    assert session.require().dumps() == before_text


def test_cancelled_job_leaves_the_project_untouched(
    studio: MainWindow, tmp_path: pathlib.Path
) -> None:
    """analysis_jobs_cancellable, from the app: cancel arrives within 200 ms, and nothing
    is applied."""
    project, _ = Project.loads(_progression_text(bars=64))
    path = _render(project, tmp_path / "long.wav")
    text = studio.session.require().dumps()
    job = analysis.chords_job(path=path)
    while job.progress <= 0.0 and not job.done:
        time.sleep(0.001)
    asked = time.monotonic()
    job.cancel()
    result = job.result()
    assert time.monotonic() - asked < 0.2
    assert result["cancelled"] or result["ok"]  # a fast machine may finish first
    assert studio.session.require().dumps() == text


def test_analysis_panel_runs_a_job(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    """The panel: pick the source, run, the result shown, applied as one step."""
    session = studio.session
    project, _ = Project.loads(_progression_text())
    session.open(project)
    panel = studio.analysis_panel
    panel.key_chord.use_project_notes()
    panel.key_chord.run()
    panel.wait_idle()
    assert panel.key_chord.key_label.text() in ("C major", "A minor")
    assert panel.key_chord.chord_list.count() >= 4
    assert panel.key_chord.chord_list.item(0).text().startswith("C")
