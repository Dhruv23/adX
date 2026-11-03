"""Exclusive-mode audio from the app (STATE.md P9-9): the preference, and how the
application opens the engine with it - exclusive first, shared when that is refused,
and the rate and buffer from the preferences either way."""

from __future__ import annotations

import pathlib
from typing import Any, ClassVar

import pytest
from PySide6.QtCore import QSettings

from adx import application
from adx.panels.preferences.pages.audio import AudioPage
from adx.preferences import Preferences


class _Engine:
    """Stands in for the engine: records how it was made; exclusive ones refuse."""

    made: ClassVar[list[dict[str, object]]] = []
    refuse_exclusive = True

    def __init__(self, **kwargs: object) -> None:
        self.kwargs = kwargs
        _Engine.made.append(kwargs)

    def start(self) -> None:
        if self.kwargs.get("exclusive") and _Engine.refuse_exclusive:
            raise RuntimeError("another application is using the output device exclusively")

    @property
    def stream(self) -> dict[str, Any]:
        return {"block_frames": 144, "output_latency_ms": 6.0}


@pytest.fixture
def fake_engine(monkeypatch: pytest.MonkeyPatch) -> type[_Engine]:
    _Engine.made = []
    _Engine.refuse_exclusive = True
    monkeypatch.setattr(application, "Engine", _Engine)
    return _Engine


def test_audio_page_stores_exclusive(qapp: object, tmp_path: pathlib.Path) -> None:
    settings = QSettings(str(tmp_path / "settings.ini"), QSettings.Format.IniFormat)
    prefs = Preferences.load(settings)
    assert not prefs.exclusive_audio
    page = AudioPage(prefs, 0.0)
    page.exclusive.setChecked(True)
    page.buffer.setCurrentText("128")
    page.apply()
    prefs.save(settings)
    again = Preferences.load(settings)
    assert again.exclusive_audio
    assert again.buffer_frames == 128
    page.deleteLater()


def test_exclusive_refused_falls_back_to_shared(fake_engine: type[_Engine]) -> None:
    _, note = application.start_engine(
        False, 0, sample_rate=44100, block_frames=128, exclusive=True
    )
    assert [m.get("exclusive", False) for m in fake_engine.made] == [True, False]
    assert all(m["sample_rate"] == 44100 and m["block_frames"] == 128 for m in fake_engine.made)
    assert note.startswith("audio device open")
    assert "exclusive mode refused" in note


def test_exclusive_open_says_so_and_has_no_input(fake_engine: type[_Engine]) -> None:
    fake_engine.refuse_exclusive = False
    _engine, note = application.start_engine(False, 2, block_frames=64, exclusive=True)
    assert len(fake_engine.made) == 1
    assert "exclusive mode (144 frames, 6.0 ms)" in note
    assert "no recording input" in note


def test_shared_mode_uses_the_preferred_rate_and_buffer(fake_engine: type[_Engine]) -> None:
    _engine, note = application.start_engine(False, 0, sample_rate=96000, block_frames=512)
    assert fake_engine.made == [
        {"null_backend": False, "sample_rate": 96000, "block_frames": 512, "input_channels": 0}
    ]
    assert note == "audio device open"
