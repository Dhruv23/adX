"""Audio descriptors (phase_10.5.md 4.4, ``adx describe`` and ``adx compare``): numbers a
scenario can assert on instead of a listener's report.

    bands       31 third-octave levels, 20 Hz - 20 kHz, dB, from Hann-windowed FFTs
    loudness    BS.1770-4 integrated, max momentary and short-term LUFS - the engine's
                own meter, which Phase 4 holds to EBU TECH 3341
    peak, rms   dBFS of the louder channel; crest is their difference
    centroid    the spectral centroid, Hz
    attack      10 % to 90 % of the envelope's peak, seconds
    decay       the peak down to -20 dB below it, seconds (None if it never falls so far)
    f0          the fundamental per 2048-frame window by autocorrelation, Hz (0 unvoiced)

Headless: numpy and the engine bridge, no Qt.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from typing import Any

import numpy as np
import numpy.typing as npt

from adx import engine_bridge
from adx.testing.wav import Audio, stereo

BAND_CENTRES: tuple[float, ...] = tuple(1000.0 * 2.0 ** ((b - 17) / 3.0) for b in range(31))


def _db(value: float) -> float:
    return 20.0 * float(np.log10(value)) if value > 0 else -200.0


def third_octaves(audio: Audio, rate: int, size: int = 8192) -> list[float]:
    mono = stereo(audio).mean(axis=1).astype(np.float64)
    if len(mono) < size:
        mono = np.concatenate([mono, np.zeros(size - len(mono))])
    window = np.hanning(size)
    power = np.zeros(size // 2 + 1)
    for start in range(0, len(mono) - size + 1, size // 2):
        power += np.abs(np.fft.rfft(mono[start : start + size] * window)) ** 2
    freqs = np.fft.rfftfreq(size, 1.0 / rate)
    levels = []
    for centre in BAND_CENTRES:
        lo, hi = centre * 2 ** (-1 / 6), centre * 2 ** (1 / 6)
        total = float(power[(freqs >= lo) & (freqs < hi)].sum())
        levels.append(10.0 * float(np.log10(total + 1e-30)))
    return levels


def centroid(audio: Audio, rate: int) -> float:
    mono = stereo(audio).mean(axis=1).astype(np.float64)
    spectrum = np.abs(np.fft.rfft(mono * np.hanning(len(mono)))) if len(mono) > 1 else np.zeros(1)
    freqs = np.fft.rfftfreq(len(mono), 1.0 / rate)
    total = float(spectrum.sum())
    return float((spectrum * freqs).sum() / total) if total > 0 else 0.0


def envelope(audio: Audio, rate: int, window_ms: float = 5.0) -> npt.NDArray[np.float64]:
    x = np.abs(stereo(audio)).max(axis=1).astype(np.float64)
    hop = max(1, int(rate * window_ms / 1000.0))
    frames = len(x) // hop
    if frames == 0:
        return np.zeros(0)
    return np.sqrt((x[: frames * hop].reshape(frames, hop) ** 2).mean(axis=1))


def attack_decay(audio: Audio, rate: int) -> tuple[float, float | None]:
    hop_s = 0.005
    env = envelope(audio, rate)
    if env.size == 0 or float(env.max()) <= 0.0:
        return 0.0, None
    peak_at = int(np.argmax(env))
    peak = float(env[peak_at])
    rising = env[: peak_at + 1]
    ten = int(np.argmax(rising >= 0.1 * peak))
    ninety = int(np.argmax(rising >= 0.9 * peak))
    attack = max(0, ninety - ten) * hop_s
    below = np.flatnonzero(env[peak_at:] <= peak * 0.1)
    decay = float(below[0]) * hop_s if below.size else None
    return attack, decay


def f0_track(
    audio: Audio, rate: int, size: int = 2048, low: float = 40.0, high: float = 2000.0
) -> list[float]:
    mono = stereo(audio).mean(axis=1).astype(np.float64)
    out = []
    lo_lag, hi_lag = int(rate / high), int(rate / low)
    for start in range(0, len(mono) - size + 1, size):
        frame = mono[start : start + size] * np.hanning(size)
        if float(np.sqrt((frame**2).mean())) < 1e-4:
            out.append(0.0)
            continue
        spectrum = np.fft.rfft(frame, 2 * size)
        ac = np.fft.irfft(spectrum * np.conj(spectrum))[:size]
        ac /= ac[0]
        lags = ac[lo_lag : min(hi_lag, size - 1)]
        if lags.size == 0:
            out.append(0.0)
            continue
        best = int(np.argmax(lags)) + lo_lag
        if ac[best] < 0.3:
            out.append(0.0)
            continue
        # Parabolic refinement around the peak.
        a, b, c = ac[best - 1], ac[best], ac[min(best + 1, size - 1)]
        denominator = a - 2 * b + c
        shift = 0.5 * (a - c) / denominator if denominator != 0 else 0.0
        out.append(rate / (best + shift))
    return out


@dataclass
class Description:
    rate: int
    seconds: float
    peak_db: float
    rms_db: float
    crest_db: float
    centroid_hz: float
    attack_s: float
    decay_s: float | None
    integrated_lufs: float
    max_momentary_lufs: float
    max_short_term_lufs: float
    bands_db: list[float] = field(default_factory=list)
    f0_median_hz: float = 0.0

    def as_dict(self) -> dict[str, Any]:
        return asdict(self)


def describe(audio: Audio, rate: int) -> Description:
    x = stereo(audio)
    peak = float(np.abs(x).max(initial=0.0))
    rms = float(np.sqrt((x.astype(np.float64) ** 2).mean(axis=0)).max()) if len(x) else 0.0
    loud = engine_bridge.measure_loudness(np.ascontiguousarray(x, dtype=np.float32), rate)
    attack, decay = attack_decay(x, rate)
    voiced = [f for f in f0_track(x, rate) if f > 0]
    return Description(
        rate=rate,
        seconds=len(x) / rate,
        peak_db=_db(peak),
        rms_db=_db(rms),
        crest_db=_db(peak) - _db(rms) if rms > 0 else 0.0,
        centroid_hz=centroid(x, rate),
        attack_s=attack,
        decay_s=decay,
        integrated_lufs=loud["integrated"],
        max_momentary_lufs=loud["max_momentary"],
        max_short_term_lufs=loud["max_short_term"],
        bands_db=third_octaves(x, rate),
        f0_median_hz=float(np.median(voiced)) if voiced else 0.0,
    )


#: What ``adx compare`` holds two files to by default.
TOLERANCES: dict[str, float] = {
    "band_db": 3.0,
    "integrated_lufs": 1.0,
    "peak_db": 1.0,
    "centroid_ratio": 0.15,
}


def compare(
    a: Description, b: Description, tolerances: dict[str, float] | None = None
) -> list[str]:
    """What differs beyond tolerance, one line each; empty when they match."""
    t = {**TOLERANCES, **(tolerances or {})}
    out = []
    loudest = max(max(a.bands_db, default=-300.0), max(b.bands_db, default=-300.0))
    for i, (x, y) in enumerate(zip(a.bands_db, b.bands_db, strict=True)):
        if max(x, y) < loudest - 60.0:
            continue  # both far below the loudest band: not part of the sound
        if abs(x - y) > t["band_db"]:
            out.append(f"band {BAND_CENTRES[i]:.0f} Hz: {x:.1f} vs {y:.1f} dB")
    if abs(a.integrated_lufs - b.integrated_lufs) > t["integrated_lufs"]:
        out.append(f"integrated loudness: {a.integrated_lufs:.1f} vs {b.integrated_lufs:.1f} LUFS")
    if abs(a.peak_db - b.peak_db) > t["peak_db"]:
        out.append(f"peak: {a.peak_db:.1f} vs {b.peak_db:.1f} dBFS")
    if (
        a.centroid_hz > 0
        and abs(a.centroid_hz - b.centroid_hz) / a.centroid_hz > t["centroid_ratio"]
    ):
        out.append(f"centroid: {a.centroid_hz:.0f} vs {b.centroid_hz:.0f} Hz")
    return out
