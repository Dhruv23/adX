"""The playlist (phase_6.md 4.1, 5)."""

from __future__ import annotations

import pathlib

import numpy as np

import ui_input
from adx import engine_bridge
from adx.engine_bridge import PPQ
from adx.mainwindow import MainWindow
from adx.panels.geometry_view import Pointer
from adx.panels.playlist.panel import PlaylistPanel
from ui_input import point, ready, render, write_wav

BAR = PPQ * 4
#: The builder's preview layer (PlaylistGeometry.cpp's Layer enum).
PREVIEWS = 3


def drag(playlist: PlaylistPanel, start: Pointer, end: Pointer) -> None:
    """A drag on the playlist canvas, through its input."""
    middle = Pointer((start.x + end.x) / 2, (start.y + end.y) / 2)
    ui_input.drag(playlist.canvas, start, middle, end)


def test_playlist_place_pattern(studio: MainWindow) -> None:
    playlist = ready(studio)
    pattern = int(studio.project.patterns_info()[0]["id"])
    playlist.toolbar.choose_pattern(pattern)
    playlist.set_tool("draw")
    before = studio.project.undo_depth()
    items = len(studio.project.playlist_items())
    target = 8 * BAR + PPQ // 3  # off the grid: snaps down to the beat
    press = point(playlist, 1, target)
    ui_input.click(playlist.canvas, press)
    rows = studio.project.playlist_items()
    assert len(rows) == items + 1
    assert studio.project.undo_depth() == before + 1
    placed = rows[rows["start"] > 0]
    assert len(placed) == 1
    assert int(placed[0]["ref"]) == pattern
    assert int(placed[0]["start"]) == playlist.snap_tick(target)
    assert int(placed[0]["start"]) % playlist.snap_ticks() == 0


def test_playlist_edit_propagates(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    playlist = ready(studio)
    pattern = int(studio.project.patterns_info()[0]["id"])
    studio.session.build_and_execute("AddPatternItem", 1, pattern, 8 * BAR, 0)
    playlist.refresh()

    def previews() -> int:
        with playlist.builder.view(PREVIEWS) as lease:
            return int(lease.vertex_count)

    assert previews() == 0
    # One note, written once, in the rack: both placements show it.
    studio.channel_rack.select_pattern("P1")
    grid = studio.channel_rack.grid
    ui_input.click(grid.view, Pointer(grid.state.cell_width * 0.5, 4))
    one = previews()
    assert one > 0
    assert one % 2 == 0
    # A second note: every placement grows by the same amount.
    ui_input.click(grid.view, Pointer(grid.state.cell_width * 8.5, 4))
    assert previews() == 2 * one
    # And the render plays it at both placements: the same edit, heard twice.
    audio = render(studio, tmp_path / "both.wav")
    rate = 48000
    for at in (0, 8 * BAR):
        begin = int(studio.project.seconds_at(at) * rate)
        window = audio[begin : begin + rate // 8]
        assert float(np.abs(window).max()) > 1e-3


def test_playlist_slip_and_stretch(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    playlist = ready(studio)
    project = studio.project
    # A pattern clip: slip moves the source offset, nothing else.
    row = project.playlist_items()[0]
    item = int(row["item"])
    playlist.set_tool("slip")
    middle = int(row["start"]) + int(row["length"]) // 2
    drag(playlist, point(playlist, 1, middle), point(playlist, 1, middle - PPQ))
    after = project.playlist_items()[0]
    assert int(after["offset"]) == int(row["offset"]) + PPQ
    assert int(after["start"]) == int(row["start"])
    assert int(after["length"]) == int(row["length"])
    assert float(after["stretch"]) == float(row["stretch"])
    # An audio clip: stretch changes length and the stretch factor together.
    path = write_wav(tmp_path / "tone.wav")
    audio = project.add_audio_item(1, path, 8 * BAR)
    studio.session.notify()
    playlist.refresh()
    rows = project.playlist_items()
    audio_row = rows[rows["item"] == audio][0]
    length = int(audio_row["length"])
    assert int(audio_row["kind"]) == 1
    playlist.set_tool("stretch")
    end = int(audio_row["start"]) + length
    depth = project.undo_depth()
    drag(playlist, point(playlist, 1, end - PPQ // 8), point(playlist, 1, end + BAR - PPQ // 8))
    rows = project.playlist_items()
    stretched = rows[rows["item"] == audio][0]
    assert int(stretched["length"]) > length
    assert float(stretched["stretch"]) > float(audio_row["stretch"])
    ratio = int(stretched["length"]) / length
    assert abs(float(stretched["stretch"]) / float(audio_row["stretch"]) - ratio) < 1e-3
    assert int(stretched["offset"]) == int(audio_row["offset"])
    # The whole drag is one history entry.
    assert project.undo_depth() == depth + 1
    assert item  # the pattern clip is untouched by the audio edit
    assert int(project.playlist_items()[0]["offset"]) == int(after["offset"])


def test_playlist_lod_switch(studio: MainWindow) -> None:
    playlist = ready(studio)
    studio.channel_rack.select_pattern("P1")
    grid = studio.channel_rack.grid
    ui_input.click(grid.view, Pointer(grid.state.cell_width * 0.5, 4))

    def previews() -> int:
        with playlist.builder.view(PREVIEWS) as lease:
            return int(lease.vertex_count)

    assert previews() > 0
    # Zoom out until the 4-bar clip is under 30 px wide: previews drop, the clip stays.
    playlist.view.pixels_per_tick = 20.0 / (4 * BAR)
    playlist.rebuild(force=True)
    assert previews() == 0
    with playlist.builder.view(2) as lease:
        assert lease.vertex_count > 0
    assert engine_bridge.PlaylistGeometry  # the builder is the engine's, not Python's


def test_playlist_audio_clip_draws_its_waveform(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    """P5-4: an audio clip in the playlist draws through the ported peak cache, and a
    changed key field (here: reversed) redraws it."""
    import time

    from PySide6.QtWidgets import QApplication

    playlist = ready(studio)
    project = studio.project
    # A decaying tone, so forwards and reversed draw differently.
    path = write_wav(tmp_path / "decay.wav", 2.0, decay=3.0)
    item = project.add_audio_item(1, path, 8 * BAR)
    studio.session.notify()

    def drawn() -> np.ndarray:
        deadline = time.monotonic() + 10.0
        while True:
            playlist.rebuild(force=True)
            pending, _detailed = playlist.builder.status()
            if not pending or time.monotonic() > deadline:
                break
            QApplication.processEvents()
            time.sleep(0.02)
        assert not pending, "peaks never arrived"
        with playlist.builder.view(PREVIEWS) as lease:
            return np.array(lease.array, copy=True)

    forwards = drawn()
    assert len(forwards) > 0
    track = int(project.playlist_items()[project.playlist_items()["item"] == item][0]["track"])
    assert studio.session.build_and_execute("SetPlaylistItem", track, item, {"reverse": 1.0})
    backwards = drawn()
    assert len(backwards) > 0
    assert not np.array_equal(forwards, backwards)
