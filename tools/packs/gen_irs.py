"""Generate the synthetic impulse responses (phase_12.md 4.10).

    python tools/packs/gen_irs.py [--out DIR]

48 kHz, 24-bit WAVs for the Convolution effect and for `physical`'s commuted body.
None of these is a measurement. Rooms and plates are decaying noise with faster
high-frequency decay. The spring is a train of dispersed chirps. The cabinets are
minimum-phase filters shaped like published speaker responses. The guitar and violin
bodies are sums of their main body modes. The stage set is early reflections from
three distances. Every random part has a fixed seed, and the output is committed
under docs/examples/generated/irs.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable
from pathlib import Path

import numpy as np
from wavio import write_wav

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "docs" / "examples" / "generated" / "irs"
RATE = 48000


def seconds(n: float) -> int:
    return round(n * RATE)


def split_bands(x: np.ndarray, crossover: float) -> tuple[np.ndarray, np.ndarray]:
    """Low and high halves of ``x`` with a raised-cosine crossover an octave wide."""
    spectrum = np.fft.rfft(x, axis=0)
    f = np.fft.rfftfreq(x.shape[0], 1 / RATE)
    octave = np.clip(np.log2(np.maximum(f, 1.0) / crossover) + 0.5, 0.0, 1.0)
    high = 0.5 - 0.5 * np.cos(np.pi * octave)
    shape = (-1,) + (1,) * (x.ndim - 1)
    lo = np.fft.irfft(spectrum * (1 - high).reshape(shape), x.shape[0], axis=0)
    hi = np.fft.irfft(spectrum * high.reshape(shape), x.shape[0], axis=0)
    return lo, hi


def band_pass(x: np.ndarray, low: float, high: float) -> np.ndarray:
    spectrum = np.fft.rfft(x, axis=0)
    f = np.fft.rfftfreq(x.shape[0], 1 / RATE)
    gain = 1.0 / np.sqrt(1 + (low / np.maximum(f, 1e-3)) ** 4) / np.sqrt(1 + (f / high) ** 4)
    shape = (-1,) + (1,) * (x.ndim - 1)
    return np.fft.irfft(spectrum * gain.reshape(shape), x.shape[0], axis=0)


def decay(length: int, rt60: float) -> np.ndarray:
    t = np.arange(length) / RATE
    return np.exp(-6.907755 * t / rt60)


def normalise(x: np.ndarray, peak: float = 0.9) -> np.ndarray:
    top = np.max(np.abs(x))
    return x * (peak / top) if top > 0 else x


def fade_out(x: np.ndarray, length: int = 2400) -> np.ndarray:
    x = x.copy()
    ramp = 0.5 + 0.5 * np.cos(np.pi * np.arange(length) / length)
    x[-length:] *= ramp[:, None] if x.ndim > 1 else ramp
    return x


def diffuse(
    seed: int,
    rt60: float,
    hf_rt60: float,
    length: float,
    predelay: float,
    crossover: float = 3500.0,
) -> np.ndarray:
    """A stereo decaying-noise tail whose top decays at ``hf_rt60``."""
    rng = np.random.default_rng(seed)
    n = seconds(length)
    noise = rng.standard_normal((n, 2))
    lo, hi = split_bands(noise, crossover)
    tail = lo * decay(n, rt60)[:, None] + hi * decay(n, hf_rt60)[:, None]
    # A few milliseconds of build-up, so the tail swells in rather than starting at its loudest.
    build = 1 - np.exp(-np.arange(n) / (0.004 * RATE))
    tail *= build[:, None]
    out = np.zeros((n + seconds(predelay), 2))
    out[seconds(predelay) :] = tail
    return out


def early(seed: int, count: int, spread: float, start: float, n: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    out = np.zeros((n, 2))
    times = np.sort(rng.uniform(start, start + spread, size=count))
    for i, at in enumerate(times):
        idx = seconds(at)
        if idx < n:
            gain = 0.7 * (1 - i / count) * rng.uniform(0.5, 1.0)
            pan = rng.uniform(-0.8, 0.8)
            out[idx, 0] += gain * (1 - pan) / 2 * 2
            out[idx, 1] += gain * (1 + pan) / 2 * 2
    return out


def room(seed: int, rt60: float, predelay: float, er_spread: float) -> np.ndarray:
    tail = diffuse(seed, rt60, rt60 * 0.45, rt60 * 1.3, predelay + er_spread * 0.6)
    n = tail.shape[0]
    out = early(seed + 1, 18, er_spread, predelay, n) + 0.35 * tail
    out[0] += 1.0  # the direct sound
    return fade_out(normalise(band_pass(out, 40.0, 16000.0)))


def plate(seed: int, rt60: float) -> np.ndarray:
    # Dense from the start and bright; no discrete early reflections.
    tail = diffuse(seed, rt60, rt60 * 0.7, rt60 * 1.2, 0.0, crossover=6000.0)
    build = 1 - np.exp(-np.arange(tail.shape[0]) / (0.0008 * RATE))
    return fade_out(normalise(band_pass(tail * build[:, None], 120.0, 14000.0)))


def spring() -> np.ndarray:
    # A dispersed chirp every 33 ms, decaying: the drip of a two-spring tank.
    n = seconds(2.4)
    chirp_len = seconds(0.03)
    f = np.fft.rfftfreq(chirp_len * 4, 1 / RATE)
    impulse = np.zeros(chirp_len * 4)
    impulse[0] = 1
    # Group delay rising with frequency: low frequencies first, the high ones later.
    phase = -2 * np.pi * (f**2) * (0.02 / (2 * 6000.0))
    chirp = np.fft.irfft(np.fft.rfft(impulse) * np.exp(1j * phase), chirp_len * 4)
    out = np.zeros((n, 2))
    rng = np.random.default_rng(7)
    for k, start in enumerate(range(0, n - len(chirp), seconds(0.033))):
        gain = 0.82**k
        jitter = int(rng.integers(0, 40))
        out[start + jitter : start + jitter + len(chirp), 0] += gain * chirp
        out[start : start + len(chirp), 1] += gain * chirp * (0.9 if k % 2 else -0.9)
    out = band_pass(out, 150.0, 5000.0) * decay(n, 2.0)[:, None]
    return fade_out(normalise(out))


def min_phase(magnitude: np.ndarray, length: int) -> np.ndarray:
    """The minimum-phase impulse with this magnitude (real cepstrum folding)."""
    full = np.concatenate([magnitude, magnitude[-2:0:-1]])
    cepstrum = np.fft.ifft(np.log(np.maximum(full, 1e-6))).real
    m = len(full)
    fold = np.zeros(m)
    fold[0] = cepstrum[0]
    fold[1 : m // 2] = 2 * cepstrum[1 : m // 2]
    fold[m // 2] = cepstrum[m // 2]
    impulse = np.fft.ifft(np.exp(np.fft.fft(fold))).real
    return impulse[:length]


def peak_db(f: np.ndarray, centre: float, gain_db: float, q: float) -> np.ndarray:
    return gain_db / (1 + (q * (f / centre - centre / np.maximum(f, 1.0))) ** 2)


def cabinet(
    highpass: float, resonance: float, presence: float, lowpass: float, drivers: int
) -> np.ndarray:
    size = 8192
    f = np.fft.rfftfreq(size, 1 / RATE)
    db = np.zeros_like(f)
    db -= 10 * np.log10(1 + (highpass / np.maximum(f, 1.0)) ** 8)  # 24 dB/oct sealed-ish roll-off
    db += peak_db(f, resonance, 5.0, 2.0)
    db += peak_db(f, presence, 4.0, 1.5)
    db -= 10 * np.log10(1 + (f / lowpass) ** 12)  # the cone's steep top end
    # Cone breakup: a ragged few dB above 1.5 kHz, the same every run.
    rng = np.random.default_rng(int(lowpass))
    ragged = np.convolve(rng.standard_normal(len(f)), np.ones(40) / 40, mode="same")
    db += np.where(f > 1500, 3.0 * ragged / np.max(np.abs(ragged)), 0.0)
    ir = min_phase(10 ** (db / 20), seconds(0.04))
    # More drivers: copies a fraction of a millisecond apart, combing the top.
    mixed = ir.copy()
    for d in range(1, drivers):
        lag = 7 * d
        mixed[lag:] += ir[:-lag] * 0.8
    window = np.ones(len(mixed))
    tail = seconds(0.01)
    window[-tail:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(tail) / tail)
    mono = normalise(mixed * window)
    return np.stack([mono, mono], axis=1)


def body(modes: list[tuple[float, float, float]], seed: int, length: float) -> np.ndarray:
    """A sum of decaying modes (frequency, gain, Q) plus a little decaying noise."""
    n = seconds(length)
    t = np.arange(n) / RATE
    out = np.zeros(n)
    for freq, gain, q in modes:
        out += gain * np.exp(-np.pi * freq * t / q) * np.sin(2 * np.pi * freq * t)
    rng = np.random.default_rng(seed)
    out += 0.05 * band_pass(rng.standard_normal(n), 200.0, 8000.0) * decay(n, 0.08)
    mono = fade_out(normalise(out), 480)
    return np.stack([mono, mono], axis=1)


GUITAR_MODES = [
    (98.0, 1.0, 25.0),  # air (Helmholtz)
    (196.0, 0.9, 35.0),  # top plate
    (225.0, 0.5, 30.0),  # back plate
    (390.0, 0.4, 40.0),
    (550.0, 0.35, 45.0),
    (780.0, 0.25, 50.0),
    (1050.0, 0.2, 55.0),
    (1600.0, 0.12, 60.0),
    (2500.0, 0.08, 60.0),
]

VIOLIN_MODES = [
    (275.0, 0.6, 30.0),  # A0, the f-hole air mode
    (405.0, 0.5, 35.0),  # CBR
    (470.0, 0.9, 40.0),  # B1-
    (550.0, 1.0, 40.0),  # B1+
    (900.0, 0.35, 45.0),
    (1350.0, 0.3, 45.0),
    (2400.0, 0.55, 25.0),  # the bridge hill
    (2900.0, 0.45, 25.0),
    (4200.0, 0.15, 40.0),
]


def stage(seed: int, distance: float) -> np.ndarray:
    """Early reflections from a stage position: further back, later, weaker direct."""
    n = seconds(1.6)
    out = early(seed, 24, 0.06 + 0.03 * distance, 0.008 + 0.012 * distance, n)
    out[0] += 1.0 / (1 + distance)
    tail = diffuse(seed + 5, 1.2, 0.6, 1.4, 0.03 + 0.02 * distance)[:n]
    out[: tail.shape[0]] += (0.1 + 0.08 * distance) * tail
    return fade_out(normalise(band_pass(out, 40.0, 15000.0)))


IRS: dict[str, Callable[[], np.ndarray]] = {
    "room-small": lambda: room(11, 0.45, 0.002, 0.02),
    "room-medium": lambda: room(12, 0.9, 0.006, 0.035),
    "room-large": lambda: room(13, 1.8, 0.012, 0.06),
    "plate-short": lambda: plate(21, 1.4),
    "plate-long": lambda: plate(22, 3.2),
    "spring": spring,
    "cab-1x12": lambda: cabinet(85.0, 110.0, 2600.0, 5500.0, 1),
    "cab-2x12": lambda: cabinet(80.0, 105.0, 2400.0, 5000.0, 2),
    "cab-4x12": lambda: cabinet(70.0, 95.0, 2200.0, 4500.0, 4),
    "body-acoustic-guitar": lambda: body(GUITAR_MODES, 31, 0.35),
    "body-violin": lambda: body(VIOLIN_MODES, 32, 0.25),
    "stage-front": lambda: stage(41, 0.0),
    "stage-mid": lambda: stage(42, 1.0),
    "stage-back": lambda: stage(43, 2.0),
}


def generate(out: Path) -> list[Path]:
    written = []
    for name, make in IRS.items():
        path = out / f"{name}.wav"
        write_wav(path, make(), RATE)
        written.append(path)
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=OUT)
    args = parser.parse_args()
    for path in generate(args.out):
        print(path.relative_to(REPO) if path.is_relative_to(REPO) else path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
