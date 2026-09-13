"""The bridge to Phase 1's audio layer.

Three things worth asserting from Python: enumeration returns plain values rather
than opaque handles, a stream can be opened and closed without leaving the process
in a bad state, and the realtime violation counter is reachable - because that
counter is what every later phase's "did the audio thread misbehave" check reads.
"""

from __future__ import annotations

import time

import adx_engine


def test_enumerate() -> None:
    """Device enumeration returns a list of plain dicts.

    A machine with no audio hardware - every CI runner - returns an empty list rather
    than raising, which is what lets the rest of the app start there at all.
    """
    devices = adx_engine.enumerate_devices()
    assert isinstance(devices, list)
    for device in devices:
        assert isinstance(device["name"], str)
        assert device["name"]
        assert isinstance(device["sample_rates"], list)
        assert device["max_output_channels"] >= 0
        assert device["max_input_channels"] >= 0
        assert device["max_output_channels"] + device["max_input_channels"] > 0


def test_open_close_null() -> None:
    """A second of the null backend leaves no realtime violations behind."""
    adx_engine.reset_rt_violations()
    adx_engine.open_stream(sample_rate=48000, block_frames=256, null_backend=True)
    try:
        time.sleep(1.0)
        info = adx_engine.stream_info()
        assert info["is_open"]
        assert info["is_running"]
        assert info["backend"] == "Null"
        assert info["sample_rate"] == 48000
        assert info["block_frames"] == 256
        # ~187 callbacks in a second at 48 kHz / 256. Loose bounds: the assertion is
        # that it ran at roughly the right rate, not that the OS scheduler is precise.
        assert 120 < info["callback_count"] < 260
        assert info["round_trip_latency_ms"] > 0.0
    finally:
        adx_engine.close_stream()

    assert adx_engine.stream_info()["is_open"] is False

    if adx_engine.rt_guard_enabled():
        assert adx_engine.rt_violation_count() == 0


def test_close_stream_is_idempotent() -> None:
    """Closing nothing is not an error - the UI should not have to track state."""
    adx_engine.close_stream()
    adx_engine.close_stream()


def test_rt_guard_reports_its_own_absence() -> None:
    """The guard says whether it is there.

    A Release build compiles the allocator hook out, so a zero violation count proves
    nothing. Anything asserting on that count has to be able to tell the difference.
    """
    assert isinstance(adx_engine.rt_guard_enabled(), bool)
