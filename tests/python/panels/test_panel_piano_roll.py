"""The piano roll inside the application (P5-1): a drawn note sounds once through its
channel's instrument; a dragged note sounds each new pitch once; a clicked key sounds.
The engine side - an audition voice heard without the transport and without a
snapshot rebuild - is the C++ audition_sounds_without_the_transport."""

from __future__ import annotations

import pytest
from PySide6.QtCore import QPointF, Qt
from PySide6.QtTest import QTest

from adx.engine_bridge import PPQ
from adx.mainwindow import MainWindow


@pytest.fixture
def heard(studio: MainWindow, monkeypatch: pytest.MonkeyPatch) -> list[tuple[int, int, int]]:
    """Every audition the engine was asked for: (channel id, pitch, velocity)."""
    calls: list[tuple[int, int, int]] = []

    def audition(channel: int, pitch: int, velocity: int = 100, seconds: float = 0.3) -> None:
        assert seconds > 0
        calls.append((channel, pitch, velocity))

    monkeypatch.setattr(studio.engine, "audition", audition)
    return calls


def test_a_drawn_note_sounds_once(studio: MainWindow, heard: list[tuple[int, int, int]]) -> None:
    roll = studio.piano_roll
    roll.select_clip("P1", "Lead")
    roll.on_resize(1200.0, 600.0)
    roll.set_tool("draw")
    roll.fit_pattern()
    view = roll.view
    x, y = view.x_of(PPQ), view.y_of(64 + 0.5)
    roll.input.pressed(x, y, 1, 0)
    roll.input.released(x, y, 1, 0)
    assert len(studio.project.notes("P1", "Lead")) == 1
    assert len(heard) == 1
    channel, pitch, velocity = heard[0]
    assert channel == int(studio.project.channels_info()[0]["id"])
    assert pitch == 64
    assert velocity > 0
    # Drag it up three semitones: each new pitch sounds once, not every frame.
    heard.clear()
    roll.set_tool("select")
    roll.input.pressed(x + 2, y, 1, 0)
    for semitones in (0.2, 1, 1.3, 2, 2.5, 3):
        roll.input.moved(x + 2, view.y_of(64 + semitones + 0.5), 1, 0)
    roll.input.released(x + 2, view.y_of(67 + 0.5), 1, 0)
    assert [p for _c, p, _v in heard] == [65, 66, 67]


def test_a_clicked_key_sounds(studio: MainWindow, heard: list[tuple[int, int, int]]) -> None:
    roll = studio.piano_roll
    roll.select_clip("P1", "Lead")
    roll.show()
    keyboard = roll.keyboard
    keyboard.resize(60, 400)
    pitch = keyboard.pitch_at(100.0)
    QTest.mouseClick(keyboard, Qt.MouseButton.LeftButton, pos=QPointF(20, 100).toPoint())
    assert heard == [(int(studio.project.channels_info()[0]["id"]), pitch, 100)]
