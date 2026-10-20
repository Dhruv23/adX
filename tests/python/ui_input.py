"""Shared helpers for UI-driven tests.

Pointer input is delivered the way geometry.qml delivers it: through a GeometryView's
``forward`` object, so a press is a new gesture (the view's ``gesture_started``) exactly
as it is for a user, and the panel's handler sees the same calls."""

from __future__ import annotations

import pathlib
import wave

import numpy as np

from adx.mainwindow import MainWindow
from adx.panels.geometry_view import LEFT, GeometryView, Pointer
from adx.panels.playlist.panel import PlaylistPanel


def press(view: GeometryView, at: Pointer) -> None:
    view.forward.pressed(at.x, at.y, at.button, at.modifiers)


def move(view: GeometryView, at: Pointer) -> None:
    view.forward.moved(at.x, at.y, at.button, at.modifiers)


def release(view: GeometryView, at: Pointer) -> None:
    view.forward.released(at.x, at.y, at.button, at.modifiers)


def click(view: GeometryView, at: Pointer) -> None:
    press(view, at)
    release(view, at)


def drag(view: GeometryView, start: Pointer, *path: Pointer) -> None:
    """Press at ``start``, move through ``path``, release at its end."""
    press(view, start)
    for at in path:
        move(view, at)
    release(view, path[-1] if path else start)


def write_wav(
    path: pathlib.Path, seconds: float = 1.0, rate: int = 48000, decay: float = 0.0
) -> str:
    """A short 16-bit mono sine, fading at ``decay`` per second (0: steady)."""
    t = np.arange(int(seconds * rate)) / rate
    data = (np.sin(2 * np.pi * 220 * t) * np.exp(-decay * t) * 12000).astype("<i2")
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(data.tobytes())
    return str(path)


def read_float_wav(path: pathlib.Path) -> np.ndarray:
    """The samples of a 32-bit float WAV (what render_to_file writes), mixed to mono."""
    data = path.read_bytes()
    channels = int.from_bytes(data[22:24], "little")
    at = data.index(b"data") + 8
    size = int.from_bytes(data[at - 4 : at], "little")
    samples = np.frombuffer(data[at : at + size], dtype="<f4")
    return samples.reshape(-1, channels).mean(axis=1)


def render(studio: MainWindow, path: pathlib.Path) -> np.ndarray:
    """Export through the window, as File > Export does, and read it back."""
    studio.export_wav(str(path))
    return read_float_wav(path)


def ready(studio: MainWindow, width: float = 1600.0, height: float = 400.0) -> PlaylistPanel:
    """The playlist sized and built, as it is once shown."""
    playlist = studio.playlist
    playlist.on_resize(width, height)
    playlist.refresh()
    return playlist


def point(playlist: PlaylistPanel, track: int, tick: float) -> Pointer:
    """The pixel at ``tick`` in the middle of ``track``'s lane."""
    rows = {t: (top, height) for t, top, height in playlist.track_rows()}
    top, height = rows[track]
    return Pointer(playlist.view.x_of(tick), playlist.view.y_of(top + height / 2))


__all__ = [
    "LEFT",
    "Pointer",
    "click",
    "drag",
    "move",
    "point",
    "press",
    "read_float_wav",
    "ready",
    "release",
    "render",
    "write_wav",
]
