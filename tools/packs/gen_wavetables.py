"""Generate the synthesized wavetables (phase_12.md 4.10).

    python tools/packs/gen_wavetables.py [--out DIR]

Each table is a WAV of 2048-sample frames, one after another, the layout the
Wavetable instrument imports (and Serum and Vital read). Everything is built
additively, harmonic by harmonic up to 1023, so a frame is band-limited by
construction; the random ones come from fixed seeds. The output is committed under
docs/examples/generated/wavetables, so a fresh clone has every table without
running this.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable
from pathlib import Path

import numpy as np
from wavio import write_wav

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "docs" / "examples" / "generated" / "wavetables"
N = 2048
HARMONICS = np.arange(1, N // 2)
THETA = np.arange(N) / N  # one cycle, in turns
RATE = 44100  # nominal: a table has no rate of its own


def additive(amps: np.ndarray, phases: np.ndarray | None = None) -> np.ndarray:
    """One frame from harmonic amplitudes (index 0 is harmonic 1) and sine phases."""
    spectrum = np.zeros(N // 2 + 1, dtype=np.complex128)
    count = min(len(amps), N // 2 - 1)
    ph = np.zeros(count) if phases is None else phases[:count]
    # sin(2 pi n t + p) is the bin's -i e^{i p} times N/2.
    spectrum[1 : count + 1] = amps[:count] * (-1j) * np.exp(1j * ph) * (N / 2)
    return np.fft.irfft(spectrum, N)


def band_limit(frame: np.ndarray) -> np.ndarray:
    """A time-domain frame with its Nyquist bin and DC removed."""
    spectrum = np.fft.rfft(frame)
    spectrum[0] = 0
    spectrum[-1] = 0
    return np.fft.irfft(spectrum, N)


def normalise(frame: np.ndarray) -> np.ndarray:
    frame = frame - frame.mean()
    peak = np.max(np.abs(frame))
    return frame * (0.98 / peak) if peak > 0 else frame


def saw_amps() -> np.ndarray:
    return 1.0 / HARMONICS


def hypersaw() -> list[np.ndarray]:
    # Seven saws, phase-spread wider frame by frame: the supersaw's comb in one cycle.
    offsets = np.array([-3, -2, -1, 0, 1, 2, 3]) / 7.0
    frames = []
    for k in range(64):
        spread = k / 63.0
        acc = np.zeros(N)
        for o in offsets:
            acc += additive(saw_amps(), 2 * np.pi * HARMONICS * o * spread * 0.5)
        frames.append(acc)
    return frames


def juno_pwm_saw_mult() -> list[np.ndarray]:
    """Saw times an inverted pulse: one hole (frames 0-31), then two (32-63)."""
    saw = additive(saw_amps())
    frames = []
    for k in range(64):
        holes = 1 if k < 32 else 2
        width = 0.05 + 0.4 * ((k % 32) / 31.0)
        t = (THETA * holes) % 1.0
        pulse = np.where(t < width, -1.0, 1.0)
        frames.append(band_limit(saw * pulse))
    return frames


def juno_pwm_saw_detune() -> list[np.ndarray]:
    """Two saws, the second moved against the first: one saw at frame 0, and a notch in
    the cycle that widens until at half a cycle the odd harmonics cancel."""
    frames = []
    for k in range(64):
        delta = 0.5 * k / 63.0
        frames.append(additive(saw_amps()) + additive(saw_amps(), 2 * np.pi * HARMONICS * delta))
    return frames


# (F1, F2, F3) and bandwidths in Hz, Peterson & Barney-ish male averages.
VOWELS = {
    "a": ((730, 1090, 2440), (80, 90, 120)),
    "e": ((530, 1840, 2480), (60, 100, 120)),
    "i": ((270, 2290, 3010), (60, 90, 100)),
    "o": ((570, 840, 2410), (70, 80, 100)),
    "u": ((300, 870, 2240), (50, 80, 100)),
}
F0 = 110.0


def vowel_amps(vowel: str, tilt: float = 1.0) -> np.ndarray:
    formants, widths = VOWELS[vowel]
    f = HARMONICS * F0
    envelope = np.zeros_like(f)
    for centre, width in zip(formants, widths, strict=True):
        envelope += 1.0 / (1.0 + ((f - centre) / (width / 2.0)) ** 2)
    glottal = 1.0 / HARMONICS**tilt
    return glottal * (envelope + 0.01)


def blend(a: np.ndarray, b: np.ndarray, count: int) -> list[np.ndarray]:
    return [additive(a + (b - a) * (i / count)) for i in range(count)]


def formant_vowels() -> list[np.ndarray]:
    order = ["a", "e", "i", "o", "u"]
    frames: list[np.ndarray] = []
    for i in range(4):
        frames += blend(vowel_amps(order[i]), vowel_amps(order[i + 1]), 16)
    return frames


def fm_sweeps() -> list[np.ndarray]:
    # 2-op FM at ratio 1, index 0..12. Bessel sidebands fall fast past index + 1, so
    # 1023 harmonics hold the whole spectrum and the frame needs no filtering.
    return [
        band_limit(np.sin(2 * np.pi * THETA + (12.0 * k / 63.0) * np.sin(2 * np.pi * THETA)))
        for k in range(64)
    ]


def ppg_digital() -> list[np.ndarray]:
    rng = np.random.default_rng(1983)
    frames = []
    for k in range(64):
        count = 4 + (k % 16) * 2
        amps = np.zeros(len(HARMONICS))
        picks = rng.choice(np.arange(64), size=count, replace=False)
        amps[picks] = rng.uniform(0.2, 1.0, size=count) / (1 + picks * 0.05)
        frame = additive(amps, rng.uniform(0, 2 * np.pi, size=len(HARMONICS)))
        frame = frame / np.max(np.abs(frame))
        # Coarse steps in time and amplitude: the 8-bit, low-rate PPG grit.
        hold = 2 ** (1 + (k // 16))
        levels = 2 ** (8 - (k // 10))
        held = np.repeat(frame[::hold], hold)[:N]
        frames.append(band_limit(np.round(held * levels) / levels))
    return frames


def chip() -> list[np.ndarray]:
    frames = [band_limit(np.where(w > THETA, 1.0, -1.0)) for w in (0.125, 0.25, 0.5)]
    # NES triangle: 32 steps of a 4-bit (16-level) ramp up and down.
    steps = np.concatenate([np.arange(15, -1, -1), np.arange(16)])
    frames.append(band_limit(np.repeat(steps / 7.5 - 1.0, N // 32)))
    # Game Boy wave channel: 32 four-bit samples, here a bright saw-sine mix.
    idx = np.arange(32)
    wave = np.round(7.5 + 7.5 * (0.6 * np.sin(2 * np.pi * idx / 32) + 0.4 * (1 - idx / 16.0)))
    wave = np.clip(wave, 0, 15)
    frames.append(band_limit(np.repeat(wave / 7.5 - 1.0, N // 32)))
    return frames


# Drawbar footages as harmonics of the 16' sub: 16, 5 1/3, 8, 4, 2 2/3, 2, 1 3/5, 1 1/3, 1.
DRAWBARS = np.array([1, 3, 2, 4, 6, 8, 10, 12, 16])
REGISTRATIONS = [
    "888000000",
    "008080800",
    "868000006",
    "888888888",
    "800000888",
    "006802000",
    "880000000",
    "688600000",
]


def organ_drawbars() -> list[np.ndarray]:
    frames = []
    for registration in REGISTRATIONS:
        amps = np.zeros(len(HARMONICS))
        for bar, level in zip(DRAWBARS, registration, strict=True):
            if level != "0":
                amps[bar - 1] += 10 ** ((int(level) - 8) * 3 / 20)
        frames.append(additive(amps))
    return frames


def vocal_ah() -> list[np.ndarray]:
    # "ah" from dark and breathy (steep glottal tilt) to bright and pressed.
    return [additive(vowel_amps("a", 2.0 - 1.4 * k / 63.0)) for k in range(64)]


def growl() -> list[np.ndarray]:
    # o -> a -> o with an FM edge that rises toward the middle: the neuro "yoi".
    frames = []
    for k in range(64):
        x = 1.0 - abs(k - 31.5) / 31.5
        amps = vowel_amps("o") * (1 - x) + vowel_amps("a") * x
        body = additive(amps)
        index = 4.0 * x
        frames.append(
            band_limit(
                np.tanh(2.0 * body / np.max(np.abs(body)))
                + 0.3 * np.sin(2 * np.pi * THETA + index * np.sin(4 * np.pi * THETA))
            )
        )
    return frames


TABLES: dict[str, Callable[[], list[np.ndarray]]] = {
    "hypersaw": hypersaw,
    "juno-pwm-saw-mult": juno_pwm_saw_mult,
    "juno-pwm-saw-detune": juno_pwm_saw_detune,
    "formant-vowels": formant_vowels,
    "fm-sweeps": fm_sweeps,
    "ppg-digital": ppg_digital,
    "chip": chip,
    "organ-drawbars": organ_drawbars,
    "vocal-ah": vocal_ah,
    "growl": growl,
}


def generate(out: Path) -> list[Path]:
    written = []
    for name, make in TABLES.items():
        frames = [normalise(f) for f in make()]
        path = out / f"{name}.wav"
        write_wav(path, np.concatenate(frames), RATE)
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
