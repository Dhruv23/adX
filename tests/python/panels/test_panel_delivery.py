"""Phase 8 in the window: the export dialog and its job, the clip editor, the record
toolbar, freeze, MIDI files and collect-and-save (phase_8.md 3)."""

from __future__ import annotations

import pathlib
import struct
import time

import pytest
from PySide6.QtWidgets import QApplication

from adx.engine_bridge import PPQ, Project
from adx.mainwindow import MainWindow
from adx.panels.export.dialog import ExportDialog
from adx.selection import Context, Kind


def _wait(condition: object, seconds: float = 60.0) -> None:
    deadline = time.monotonic() + seconds
    while not condition():  # type: ignore[operator]
        assert time.monotonic() < deadline, "timed out"
        QApplication.processEvents()
        time.sleep(0.01)


def _wav(path: pathlib.Path, frames: int = 24000) -> None:
    data = b"".join(struct.pack("<hh", (i * 37) % 9000, -((i * 37) % 9000)) for i in range(frames))
    path.write_bytes(
        struct.pack(
            "<4sI4s4sIHHIIHH4sI",
            b"RIFF",
            36 + len(data),
            b"WAVE",
            b"fmt ",
            16,
            1,
            2,
            48000,
            192000,
            4,
            16,
            b"data",
            len(data),
        )
        + data
    )


def _open_with_clip(studio: MainWindow, tmp_path: pathlib.Path) -> tuple[int, int]:
    sample = tmp_path / "loop.wav"
    _wav(sample)
    path = tmp_path / "song.adx"
    path.write_text(
        "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\n"
        f'TRACK 1 name="Audio"\n  AUDIO "{sample.as_posix()}" 0:0:0\n\n'
        '[MIXER]\nINSERT 1 name="Master"\n',
        encoding="utf-8",
    )
    studio.open_project(str(path))
    project = studio.project
    assert project is not None
    (row,) = project.playlist_items()
    return int(row["track"]), int(row["item"])


@pytest.mark.usefixtures("qapp")
def test_export_dialog_reads_as_settings() -> None:
    """Every control lands in ExportSettings, and the extension follows the format."""
    dialog = ExportDialog("C:/x/song.wav", ["verse", "chorus"], selection=(0, 3840))
    dialog.format.setCurrentIndex(dialog.format.findData("flac"))
    assert dialog.output.text().endswith("song.flac")
    dialog.source.setCurrentIndex(2)
    dialog.stems.setCurrentIndex(dialog.stems.findData("inserts"))
    dialog.range.setCurrentIndex(dialog.range.findData("markers"))
    dialog.from_marker.setCurrentText("verse")
    dialog.until_silence.setChecked(True)
    dialog.normalize.setCurrentIndex(dialog.normalize.findData("lufs"))
    dialog.block.setCurrentIndex(dialog.block.findData(512))
    dialog.midi.setChecked(True)
    settings = dialog.settings()
    assert settings.format == "flac"
    assert (settings.master, settings.stems) == (False, "inserts")
    assert (settings.range, settings.from_marker, settings.to_marker) == ("markers", "verse", "")
    assert settings.tail_until_silence
    assert settings.tail_seconds == 0.0
    assert (settings.normalize, settings.normalize_target) == ("lufs", -14.0)
    assert settings.block == 512
    assert settings.midi
    dialog.range.setCurrentIndex(dialog.range.findData("selection"))
    assert (dialog.settings().from_ticks, dialog.settings().to_ticks) == (0, 3840)
    dialog.deleteLater()


def test_export_runs_in_the_background(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    """The window's export is a job: it reports through the status bar and finishes."""
    from adx.export_settings import ExportSettings

    _open_with_clip(studio, tmp_path)
    out = tmp_path / "mix.ogg"
    studio.delivery.start_export(ExportSettings(output=str(out), format="ogg"))
    _wait(lambda: studio.delivery.last_export is not None)
    result = studio.delivery.last_export
    assert result is not None
    assert result["ok"], result
    assert out.read_bytes()[:4] == b"OggS"
    assert "Exported mix.ogg" in studio.statusBar().currentMessage()


def test_clip_editor_edits_the_selected_clip(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    """Select an audio clip: the editor shows it; a fade, an edit and a stretch are one
    undo step each and reach the model."""
    track, item = _open_with_clip(studio, tmp_path)
    editor = studio.clip_editor
    assert not editor.stretch.isEnabled()
    studio.selection.select(Kind.CLIPS, [item], Context())
    assert editor.target == (track, item)
    assert "loop.wav" in editor.heading.text()
    project = studio.project
    assert project is not None
    depth = project.undo_depth()

    editor.fade_in.setValue(480)
    editor.fade_in.editingFinished.emit()
    studio.session.seal()
    editor.add_edit("normalize", -0.3)
    studio.session.seal()
    editor.stretch.setValue(2.0)
    editor.stretch.editingFinished.emit()
    studio.session.seal()

    info = project.audio_item(track, item)
    assert info is not None
    assert info["fade_in"] == 480
    ((kind, amount),) = info["edits"]
    assert kind == "normalize"
    assert abs(amount + 0.3) < 1e-6
    assert info["stretch"] == 2.0
    assert project.undo_depth() == depth + 3
    assert editor.edits.count() == 1
    editor.pop_edit()
    assert project.audio_item(track, item)["edits"] == []  # type: ignore[index]


def test_clip_editor_seeds_warp_markers(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    """Detect from onsets writes markers at the clip's current timing."""
    sample = tmp_path / "hits.wav"
    frames = 48000 * 2
    samples = [0] * (frames * 2)
    for hit in range(4):
        start = 12000 + hit * 24000
        for i in range(400):
            value = int(20000 * (1 - i / 400)) * (1 if i % 2 else -1)
            samples[(start + i) * 2] = samples[(start + i) * 2 + 1] = value
    data = struct.pack(f"<{len(samples)}h", *samples)
    sample.write_bytes(
        struct.pack(
            "<4sI4s4sIHHIIHH4sI",
            b"RIFF",
            36 + len(data),
            b"WAVE",
            b"fmt ",
            16,
            1,
            2,
            48000,
            192000,
            4,
            16,
            b"data",
            len(data),
        )
        + data
    )
    path = tmp_path / "hits.adx"
    path.write_text(
        "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\n"
        f'TRACK 1\n  AUDIO "{sample.as_posix()}" 0:0:0\n\n[MIXER]\nINSERT 1 name="Master"\n',
        encoding="utf-8",
    )
    studio.open_project(str(path))
    project = studio.project
    assert project is not None
    (row,) = project.playlist_items()
    studio.selection.select(Kind.CLIPS, [int(row["item"])], Context())
    studio.clip_editor.detect_warps()
    info = project.audio_item(int(row["track"]), int(row["item"]))
    assert info is not None
    warps = info["warps"]
    assert len(warps) == 4
    for (source, at), hit in zip(warps, range(4), strict=True):
        assert abs(source - (0.25 + hit * 0.5)) < 0.012
        # 120 bpm: a second is two beats.
        assert abs(at - source * 2 * PPQ) <= 1


def test_record_refuses_without_an_input(studio: MainWindow) -> None:
    """The null engine has no input: arming says why and does not stay armed."""
    studio.registry.trigger("transport.record")
    assert not studio.delivery.record.arm.isChecked()
    assert "no input" in studio.statusBar().currentMessage()
    assert studio.delivery.record.latency.text().strip() == "no input open"


def test_record_settings_follow_the_controls(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    """Track from the selected clip; punch over its span; count-in and monitor as set."""
    track, item = _open_with_clip(studio, tmp_path)
    studio.selection.select(Kind.CLIPS, [item], Context())
    toolbar = studio.delivery.record
    toolbar.punch.setChecked(True)
    toolbar.count_in.setValue(1)
    toolbar.monitor.setCurrentIndex(1)
    assert studio.delivery.record_track() == track
    settings = toolbar.settings(track)
    assert settings["monitor"] == "software"
    assert settings["count_in_bars"] == 1
    (row,) = studio.project.playlist_items()  # type: ignore[union-attr]
    assert (settings["punch_in"], settings["punch_out"]) == (
        int(row["start"]),
        int(row["start"]) + int(row["length"]),
    )
    assert settings["project_directory"] == str(tmp_path)


def test_freeze_and_unfreeze_from_the_menu(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    """Freeze renders a channel beside the project; unfreeze puts it back; each is a step."""
    repo = pathlib.Path(__file__).resolve().parents[3]
    source = repo / "tests" / "golden" / "chord_steal.adx"
    path = tmp_path / "chord.adx"
    path.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
    studio.open_project(str(path))
    project = studio.project
    assert project is not None
    channel = int(project.channels_info()[0]["id"])
    studio.selection.select(Kind.CHANNELS, [channel], Context())
    studio.registry.update_enabled()
    depth = project.undo_depth()
    assert studio.registry.trigger("edit.freeze")
    assert list((tmp_path / "freeze").glob("*.wav"))
    assert project.undo_depth() == depth + 1
    assert studio.registry.trigger("edit.unfreeze")
    assert project.undo_depth() == depth + 2


def test_midi_round_trip_through_the_window(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    """Export the arrangement as MIDI, import it into a new project: same note count."""
    from adx.engine_bridge import export_midi

    repo = pathlib.Path(__file__).resolve().parents[3]
    source, _ = Project.load(str(repo / "tests" / "golden" / "chord_steal.adx"))
    midi = tmp_path / "chords.mid"
    export_midi(source, str(midi))
    notes = int(source.info()["notes"])
    studio.new_project()
    result = studio.delivery.import_midi(str(midi))
    assert result["tracks"] >= 1
    assert int(studio.project.info()["notes"]) == notes  # type: ignore[union-attr]
    assert "Imported chords.mid" in studio.statusBar().currentMessage()


def test_collect_and_save(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    """Collect saves first, then bundles the samples and zips."""
    _open_with_clip(studio, tmp_path)
    result = studio.delivery.collect_and_save(str(tmp_path / "out"))
    assert result is not None
    assert result["ok"], result
    assert result["copied"] == 1
    assert (tmp_path / "out" / "samples" / "loop.wav").is_file()
    assert pathlib.Path(result["zip"]).is_file()


def test_slice_keeps_the_clip_properties(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    """The playlist's slice cuts one clip into two that keep its fades, edits, stretch
    and envelopes - one undo step (clip::splitItem)."""
    sample = tmp_path / "loop.wav"
    _wav(sample, 48000)
    path = tmp_path / "cut.adx"
    path.write_text(
        "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\nTRACK 1\n"
        f'  AUDIO "{sample.as_posix()}" 0:0:0 length=0:2:0 stretch=2 fadein=0:0:960 '
        "fadeout=0:0:960 edit=dc\n"
        "    ENVELOPE gain\n      0:0:0 0\n      0:2:0 1\n\n"
        '[MIXER]\nINSERT 1 name="Master"\n',
        encoding="utf-8",
    )
    studio.open_project(str(path))
    project = studio.project
    assert project is not None
    (row,) = project.playlist_items()
    track, item = int(row["track"]), int(row["item"])
    depth = project.undo_depth()
    studio.playlist.slice(track, item, PPQ)
    assert project.undo_depth() == depth + 1
    rows = sorted(project.playlist_items(), key=lambda r: int(r["start"]))
    assert [(int(r["start"]), int(r["length"])) for r in rows] == [(0, PPQ), (PPQ, PPQ)]
    left = project.audio_item(track, int(rows[0]["item"]))
    right = project.audio_item(track, int(rows[1]["item"]))
    assert left is not None
    assert right is not None
    assert (left["fade_in"], left["fade_out"]) == (960, 0)
    assert (right["fade_in"], right["fade_out"]) == (0, 960)
    assert left["edits"] == right["edits"] == [("dc", 0.0)]
    assert left["stretch"] == right["stretch"] == 2.0
    # Half a beat into the source: one beat of output at a 2x stretch.
    assert int(rows[1]["offset"]) == PPQ // 2
    envelopes = [project.item_envelopes(track, int(r["item"])) for r in rows]
    assert [len(e) for e in envelopes] == [1, 1]
    assert envelopes[0][0]["points"][-1][1] == pytest.approx(0.5)
    assert envelopes[1][0]["points"][0][1] == pytest.approx(0.5)


def test_live_midi_follows_the_selected_channel(studio: MainWindow) -> None:
    """A keyboard plays the selected channel; with none selected, the first."""
    live = studio.live_midi
    project = studio.project
    assert project is not None
    studio.channel_rack.add_channel()
    studio.channel_rack.add_channel()
    ids = [int(c["id"]) for c in project.channels_info()]
    assert live.target == ids[0]
    studio.selection.select(Kind.CHANNELS, [ids[1]], Context())
    assert live.target == ids[1]
    studio.selection.clear()
    assert live.target == ids[1]  # the last one stays while it exists
    # The null engine has no MIDI service to open, and says so rather than raising.
    assert live.apply(["No such keyboard"]) == ["No such keyboard"]


@pytest.mark.usefixtures("qapp")
def test_midi_preferences_page_lists_inputs() -> None:
    """Every input the system has, plus a remembered one that is unplugged; apply writes
    the checked ones."""
    from adx.panels.preferences.pages.midi import MidiPage
    from adx.preferences import Preferences

    prefs = Preferences(midi_inputs=["Old keyboard"])
    page = MidiPage(prefs, ["Keystation 49", "loopMIDI Port"])
    assert list(page.boxes) == ["Keystation 49", "loopMIDI Port", "Old keyboard"]
    assert "not connected" in page.boxes["Old keyboard"].text()
    assert page.boxes["Old keyboard"].isChecked()
    page.boxes["Keystation 49"].setChecked(True)
    page.boxes["Old keyboard"].setChecked(False)
    page.apply()
    assert prefs.midi_inputs == ["Keystation 49"]
    page.deleteLater()
