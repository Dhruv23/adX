"""Third-octave band levels of a render, and the difference between two renders.

The measurement behind the Tranche A gate (phase_4.md §4.3, `suffocation_spectral_match`):
31 bands from 20 Hz to 20 kHz, read from a Welch-averaged power spectrum of the mono
sum. The FFT is 65536 points: anything shorter smears a bass tone across a band edge
and reports a deficit in one band and a surplus in the next.

    python tools/ab/bands.py v1.wav v2.wav [--tolerance 1.5]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from wav import FloatArray, load

FFT_SIZE = 65536

#: Band centres, 1 kHz * 2^(k/3) for k in -17..13: 20 Hz .. 20 kHz.
CENTRES: FloatArray = 1000.0 * 2.0 ** (np.arange(-17, 14) / 3.0)


def band_levels(samples: FloatArray, rate: int) -> FloatArray:
    """Level of each of the 31 bands, in dB, of a frames x channels array."""
    mono = samples.mean(axis=1)
    window = np.hanning(FFT_SIZE)
    hop = FFT_SIZE // 2
    power = np.zeros(FFT_SIZE // 2 + 1)
    count = 0
    for start in range(0, len(mono) - FFT_SIZE, hop):
        power += np.abs(np.fft.rfft(mono[start : start + FFT_SIZE] * window)) ** 2
        count += 1
    if count == 0:
        raise ValueError("render shorter than one FFT frame")
    power /= count
    power *= 2.0 / (FFT_SIZE * float((window**2).sum()))
    frequency = np.fft.rfftfreq(FFT_SIZE, 1.0 / rate)
    levels = np.empty(len(CENTRES))
    for i, centre in enumerate(CENTRES):
        selected = (frequency >= centre * 2 ** (-1 / 6)) & (frequency < centre * 2 ** (1 / 6))
        levels[i] = 10.0 * np.log10(power[selected].sum() + 1e-30)
    return levels


def file_levels(path: str | Path) -> FloatArray:
    samples, rate = load(path)
    return band_levels(samples, rate)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("reference")
    parser.add_argument("candidate")
    parser.add_argument("--tolerance", type=float, default=1.5)
    args = parser.parse_args(argv)

    reference = file_levels(args.reference)
    candidate = file_levels(args.candidate)
    delta = candidate - reference
    for centre, a, b, d in zip(CENTRES, reference, candidate, delta, strict=True):
        flag = "*" if abs(d) > args.tolerance else ""
        print(f"{centre:8.0f} Hz  ref {a:7.2f}  new {b:7.2f}  diff {d:+6.2f} {flag}")
    over = int((np.abs(delta) > args.tolerance).sum())
    print(f"max |diff| {np.abs(delta).max():.2f} dB; {over} band(s) over {args.tolerance} dB")
    return 0 if over == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
