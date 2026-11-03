"""Audio descriptors on known signals (phase_10.5.md 5, ``describe_known_signals``): a
sine's centroid and f0, LUFS against EBU TECH 3341's cases, a known envelope's attack and
decay, and ``compare``."""

from __future__ import annotations

import numpy as np
import pytest

from adx.testing.describe import attack_decay, compare, describe, f0_track

RATE = 48000


def sine(hz: float, seconds: float, dbfs: float) -> np.ndarray:
    t = np.arange(int(seconds * RATE)) / RATE
    x = (10 ** (dbfs / 20) * np.sin(2 * np.pi * hz * t)).astype(np.float32)
    return np.stack([x, x], axis=1)


def test_sine_centroid_and_f0() -> None:
    d = describe(sine(1000.0, 2.0, -6.0), RATE)
    assert d.centroid_hz == pytest.approx(1000.0, rel=0.01)
    assert d.f0_median_hz == pytest.approx(1000.0, rel=0.005)
    assert d.peak_db == pytest.approx(-6.0, abs=0.05)
    assert d.crest_db == pytest.approx(3.01, abs=0.05)
    loudest = int(np.argmax(d.bands_db))
    assert loudest == 17  # the 1 kHz band
    assert f0_track(sine(220.0, 0.5, -12.0), RATE)[2] == pytest.approx(220.0, rel=0.005)


@pytest.mark.parametrize(
    ("segments", "want"),
    [
        ([(-23.0, 20.0)], -23.0),  # EBU TECH 3341 case 1
        ([(-33.0, 20.0)], -33.0),  # case 2
        ([(-36.0, 10.0), (-23.0, 60.0), (-36.0, 10.0)], -23.0),  # case 3
        (
            [(-72.0, 10.0), (-36.0, 10.0), (-23.0, 60.0), (-36.0, 10.0), (-72.0, 10.0)],
            -23.0,
        ),  # case 4
        ([(-26.0, 20.0), (-20.0, 20.1), (-26.0, 20.0)], -23.0),  # case 5
    ],
)
def test_lufs_ebu_tech_3341(segments: list[tuple[float, float]], want: float) -> None:
    audio = np.concatenate([sine(1000.0, seconds, dbfs) for dbfs, seconds in segments])
    assert describe(audio, RATE).integrated_lufs == pytest.approx(want, abs=0.1)


def test_attack_and_decay_of_a_known_envelope() -> None:
    t = np.arange(int(2.0 * RATE)) / RATE
    attack = 0.1
    env = np.where(t < attack, t / attack, np.exp(-(t - attack) / 0.2))
    x = (0.5 * env * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
    rise, fall = attack_decay(np.stack([x, x], axis=1), RATE)
    assert rise == pytest.approx(0.08, abs=0.01)  # 10 % to 90 % of a linear 100 ms ramp
    assert fall is not None
    assert fall == pytest.approx(0.2 * np.log(10), abs=0.02)  # e^(-t/0.2) to -20 dB


def test_compare_flags_what_differs() -> None:
    a = describe(sine(1000.0, 2.0, -12.0), RATE)
    assert compare(a, describe(sine(1000.0, 2.0, -12.2), RATE)) == []
    louder = compare(a, describe(sine(1000.0, 2.0, -6.0), RATE))
    assert any("integrated" in line for line in louder)
    moved = compare(a, describe(sine(2000.0, 2.0, -12.0), RATE))
    assert any("band" in line for line in moved)
    assert any("centroid" in line for line in moved)
