"""Comparing a render with a reference recording by harmony (phase_10.5.md 4.8): chroma
per frame, the render aligned to the reference by the offset where their chroma agree
best, then the correlation bar by bar."""

from __future__ import annotations

import numpy as np
import numpy.typing as npt

FRAME = 8192
HOP = 2400  # 50 ms at 48 kHz


def chroma(audio: npt.NDArray[np.float32], rate: int) -> npt.NDArray[np.float64]:
    """(frames, 12), each frame unit length; silent frames are zero."""
    mono = audio.mean(axis=1) if audio.ndim == 2 else audio
    window = np.hanning(FRAME)
    count = max(0, (len(mono) - FRAME) // HOP + 1)
    freqs = np.fft.rfftfreq(FRAME, 1.0 / rate)
    useful = (freqs >= 55.0) & (freqs <= 4200.0)
    classes = (np.round(12 * np.log2(freqs[useful] / 440.0)).astype(int) + 9) % 12
    out = np.zeros((count, 12))
    for i in range(count):
        spectrum = np.abs(np.fft.rfft(mono[i * HOP : i * HOP + FRAME] * window))[useful]
        np.add.at(out[i], classes, np.log1p(1000.0 * spectrum))
    norms = np.linalg.norm(out, axis=1, keepdims=True)
    loud = norms[:, 0] > 1e-6
    out[loud] /= norms[loud]
    out[~loud] = 0.0
    return out


def best_offset(
    render: npt.NDArray[np.float64], reference: npt.NDArray[np.float64], span: int
) -> int:
    """Frames to drop from the reference's start so it lines up with the render."""
    best, at = -1.0, 0
    n = min(len(render), len(reference))
    for shift in range(0, min(span, len(reference) - 1)):
        m = min(n, len(reference) - shift)
        score = float(np.mean(np.sum(render[:m] * reference[shift : shift + m], axis=1)))
        if score > best:
            best, at = score, shift
    return at


def bar_correlations(
    render: npt.NDArray[np.float64],
    reference: npt.NDArray[np.float64],
    shift: int,
    bar_frames: float,
) -> list[float]:
    """Per bar: the correlation of the summed chroma, render against reference."""
    out = []
    bars = int(min(len(render), len(reference) - shift) / bar_frames)
    for b in range(bars):
        lo, hi = int(b * bar_frames), int((b + 1) * bar_frames)
        x = render[lo:hi].sum(axis=0)
        y = reference[shift + lo : shift + hi].sum(axis=0)
        if np.linalg.norm(x) < 1e-6 or np.linalg.norm(y) < 1e-6:
            continue
        out.append(float(np.corrcoef(x, y)[0, 1]))
    return out
