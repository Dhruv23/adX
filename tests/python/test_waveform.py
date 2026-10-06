"""The waveform view over a 10-minute file (phase_5.md 4.5, 6): opening never blocks on
peak analysis, every zoom level draws in O(pixels), and a pan or zoom inside the
built range makes no engine call."""

from __future__ import annotations

import pathlib
import time
import wave

import numpy as np
import pytest
from PySide6.QtWidgets import QApplication

from adx.engine_bridge import CALLS
from conftest import dispose

RATE = 48000
MINUTES = 10


@pytest.fixture(scope="module")
def ten_minutes(tmp_path_factory: pytest.TempPathFactory) -> pathlib.Path:
    """A 10-minute mono 16-bit WAV: a slow sweep under a beating envelope."""
    path = tmp_path_factory.mktemp("audio") / "ten_minutes.wav"
    frames = RATE * 60 * MINUTES
    t = np.arange(frames, dtype=np.float64) / RATE
    signal = (
        0.8 * np.sin(2 * np.pi * (110.0 + t * 0.5) * t) * (0.5 + 0.5 * np.sin(2 * np.pi * 0.2 * t))
    )
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes((signal * 32767).astype("<i2").tobytes())
    return path


@pytest.mark.usefixtures("quick")
def test_ten_minute_file_every_zoom(ten_minutes: pathlib.Path) -> None:
    from adx.panels.waveform.widget import WaveformView

    view = WaveformView()
    view.resize(1200, 300)
    view.on_resize(1200.0)

    start = time.perf_counter()
    view.open(str(ten_minutes))
    opened_ms = (time.perf_counter() - start) * 1000.0
    # Decoding and analysis happen on the worker: open() returns at once.
    assert opened_ms < 50.0, opened_ms
    assert "analysing" in view.status.text()

    deadline = time.monotonic() + 60.0
    first_drawn = None
    while view.peaks is not None and not view.peaks.status()[0] and time.monotonic() < deadline:
        QApplication.processEvents()
        if first_drawn is None and view.tier >= 0:
            first_drawn = view.tier
        time.sleep(0.01)
    QApplication.processEvents()
    assert view.peaks is not None
    complete, failed, _progress, frames, rate = view.peaks.status()
    assert complete
    assert not failed
    assert frames == RATE * 60 * MINUTES
    assert rate == RATE
    view._poll_peaks()

    # Every zoom from the whole file down to a few hundred samples on screen.
    worst_ms = 0.0
    tiers = set()
    span = view.view.duration
    while span > 0.01:
        view.view.pixels_per_second = view.view.width / span
        view.view.seconds_start = max(0.0, view.view.duration / 2 - span / 2)
        begin = time.perf_counter()
        view.rebuild(force=True)
        worst_ms = max(worst_ms, (time.perf_counter() - begin) * 1000.0)
        tiers.add(view.tier)
        span /= 4.0
    print(f"waveform: 10-minute file, worst rebuild {worst_ms:.3f} ms over tiers {sorted(tiers)}")
    assert worst_ms < 8.0  # O(columns): no zoom level scans the samples
    assert len(tiers) >= 6  # the zoom walked the mipmap

    # Inside the built range a pan or zoom is a transform: zero engine calls.
    with CALLS.measure() as calls:
        for _ in range(50):
            view.view.pan(3.0)
            view.apply_view()
        view.view.zoom(1.2, 600.0)
        view.apply_view()
    assert calls[0] == 0
    dispose(view)
