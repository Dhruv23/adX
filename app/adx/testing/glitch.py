"""No glitches, as a measurement (phase_10.5.md 4.5): what a listener hears as a click, a
dropout or a stutter, found in a capture by position.

    dropout     a run of exact zeros, longer than 1 ms, that starts and ends abruptly:
                the sample before it and the audio after it are not near silence (a note
                that decays to zero and stops is not a dropout)
    repeat      a block of 64-1024 frames repeated sample for sample - a stuck buffer -
                in material that is not constant
    click       a first-difference sample more than 8 times the RMS of the first
                difference around it (10 ms either side, itself excluded), above an
                absolute floor so dither cannot trip it, and 4 times the largest step
                within 6 samples of it - a discontinuity, not a steep attack, which is
                steep for many samples running - and 4 times any step elsewhere within
                10 ms, which a square or hard-clipped wave's edges never are
    residual    with a reference (an offline render of the same project): the capture
                aligned to it, and any 1024-frame block whose difference peaks above
                -90 dBFS, outside the windows a scenario says it edited

The detector is a gate, so ``glitch_positive_controls`` plants each defect in a clean
render and requires it to be found where it was planted, and requires nothing of a clean
render (tests/python/test_glitch.py).
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass

import numpy as np
import numpy.typing as npt

from adx.testing.wav import Audio, stereo

#: Block sizes a stuck buffer would repeat: the engine's and common device sizes.
REPEAT_BLOCKS: tuple[int, ...] = (64, 128, 256, 480, 512, 1024)
CLICK_RATIO = 8.0
CLICK_FLOOR = 0.01
#: A click's step against the largest step around it.
ISOLATION = 4.0
RESIDUAL_DB = -90.0


@dataclass(frozen=True)
class Finding:
    kind: str
    #: Frames from the start of the capture.
    start: int
    end: int
    detail: str

    def seconds(self, rate: int) -> float:
        return self.start / rate

    def __str__(self) -> str:
        return f"{self.kind} at frame {self.start} ({self.end - self.start} frames): {self.detail}"


def _runs(mask: npt.NDArray[np.bool_]) -> list[tuple[int, int]]:
    """[start, end) of every run of True."""
    if mask.size == 0:
        return []
    padded = np.concatenate([[False], mask, [False]])
    edges = np.flatnonzero(np.diff(padded.astype(np.int8)))
    return [(int(a), int(b)) for a, b in zip(edges[0::2], edges[1::2], strict=True)]


def dropouts(audio: Audio, rate: int, minimum_ms: float = 1.0, loud: float = 1e-3) -> list[Finding]:
    x = stereo(audio)
    zero = np.all(x == 0.0, axis=1)
    out = []
    minimum = max(1, int(rate * minimum_ms / 1000.0))
    after_window = max(1, rate // 100)
    for start, end in _runs(zero):
        if end - start < minimum or start == 0 or end >= len(x):
            continue
        before = float(np.max(np.abs(x[start - 1])))
        after = float(np.max(np.abs(x[end : end + after_window])))
        if before > loud and after > loud:
            out.append(
                Finding("dropout", start, end, f"{(end - start) * 1000 / rate:.1f} ms of silence")
            )
    return out


def repeats(audio: Audio, blocks: Sequence[int] = REPEAT_BLOCKS) -> list[Finding]:
    x = stereo(audio)
    out: list[Finding] = []
    seen: list[tuple[int, int]] = []
    for block in blocks:
        if len(x) < 2 * block:
            continue
        same = np.all(x[block:] == x[:-block], axis=1)
        for start, end in _runs(same):
            if end - start < block:
                continue
            segment = x[start + block : end + block]
            if float(np.ptp(segment)) < 1e-6:
                continue  # silence or DC repeats trivially
            at = start + block
            if any(a <= at < b for a, b in seen):
                continue
            seen.append((at, end + block))
            out.append(Finding("repeat", at, end + block, f"{block}-frame block repeated"))
    return out


def clicks(
    audio: Audio, rate: int, ratio: float = CLICK_RATIO, floor: float = CLICK_FLOOR
) -> list[Finding]:
    x = stereo(audio).astype(np.float64)
    if len(x) < 3:
        return []
    half = max(1, rate // 100)
    out: list[Finding] = []
    for channel in range(x.shape[1]):
        d = np.diff(x[:, channel])
        sq = np.concatenate([[0.0], np.cumsum(d * d)])
        n = len(d)
        idx = np.arange(n)
        lo = np.clip(idx - half, 0, n)
        hi = np.clip(idx + half + 1, 0, n)
        total = sq[hi] - sq[lo] - (d * d)
        count = (hi - lo - 1).clip(min=1)
        local = np.sqrt(total / count)
        # Isolated, too: the steps just around it (skipping the one after, which a
        # one-sample spike makes as large) are a quarter of it or less. A steep attack
        # is steep for many samples running, so its neighbours are as large as it is.
        a = np.abs(d)
        padded = np.concatenate([np.zeros(6), a, np.zeros(6)])
        neighbours = np.max(
            np.stack([padded[6 + k : 6 + k + n] for k in (-6, -5, -4, -3, -2, -1, 2, 3, 4, 5, 6)]),
            axis=0,
        )
        hits = np.flatnonzero((a > ratio * local) & (a > floor) & (a > ISOLATION * neighbours))
        for i in hits:
            # One-off, as well: no step a quarter as large anywhere else within 10 ms. A
            # square or hard-clipped wave has a vertical edge every period, and a
            # distortion after the mix can make one of them three times the others; a
            # click is far larger than anything near it.
            window = np.concatenate([a[max(0, i - half) : max(0, i - 6)], a[i + 7 : i + half + 1]])
            if window.size and float(window.max()) > a[i] / ISOLATION:
                continue
            frame = int(i) + 1
            if out and out[-1].kind == "click" and abs(out[-1].start - frame) < half:
                continue
            out.append(
                Finding(
                    "click",
                    frame,
                    frame + 1,
                    f"step {abs(d[i]):.3f} is {abs(d[i]) / max(local[i], 1e-12):.0f}x "
                    f"the local level (channel {channel})",
                )
            )
    out.sort(key=lambda f: f.start)
    return out


def align(capture: Audio, reference: Audio, search: int = 48000) -> int:
    """The lag that best aligns ``capture`` to ``reference``: capture[i] ~ reference[i - lag]."""
    a = stereo(capture).mean(axis=1).astype(np.float64)
    b = stereo(reference).mean(axis=1).astype(np.float64)
    n = min(len(a), len(b), 4 * search)
    if n == 0:
        return 0
    size = 1 << int(np.ceil(np.log2(2 * n)))
    correlation = np.fft.irfft(np.fft.rfft(a[:n], size) * np.conj(np.fft.rfft(b[:n], size)), size)
    lags = np.concatenate([correlation[: search + 1], correlation[-search:]])
    best = int(np.argmax(lags))
    return best if best <= search else best - len(lags)


def residuals(
    capture: Audio,
    reference: Audio,
    rate: int,
    *,
    threshold_db: float = RESIDUAL_DB,
    ignore: Sequence[tuple[float, float]] = (),
    block: int = 1024,
    lag: int | None = None,
) -> list[Finding]:
    """Blocks where the aligned capture differs from the reference by more than
    ``threshold_db`` (peak), outside the ``ignore`` windows (seconds)."""
    c = stereo(capture)
    r = stereo(reference)
    shift = align(c, r) if lag is None else lag
    if shift >= 0:
        c = c[shift:]
    else:
        r = r[-shift:]
    n = min(len(c), len(r))
    difference = np.abs(c[:n].astype(np.float64) - r[:n].astype(np.float64)).max(axis=1)
    limit = 10.0 ** (threshold_db / 20.0)
    out = []
    for start in range(0, n, block):
        peak = float(difference[start : start + block].max(initial=0.0))
        if peak <= limit:
            continue
        seconds = (start + max(shift, 0)) / rate
        if any(a <= seconds <= b for a, b in ignore):
            continue
        db = 20.0 * np.log10(peak) if peak > 0 else -300.0
        out.append(
            Finding(
                "residual",
                start + max(shift, 0),
                start + max(shift, 0) + block,
                f"{db:.1f} dBFS against the reference",
            )
        )
    return out


def find_glitches(
    capture: Audio,
    rate: int,
    *,
    reference: Audio | None = None,
    ignore: Sequence[tuple[float, float]] = (),
    threshold_db: float = RESIDUAL_DB,
) -> list[Finding]:
    """Every finding, by position."""
    found = dropouts(capture, rate) + repeats(capture) + clicks(capture, rate)
    if reference is not None:
        found += residuals(capture, reference, rate, threshold_db=threshold_db, ignore=ignore)
    found.sort(key=lambda f: (f.start, f.kind))
    return found


def bit_identical(capture: Audio, reference: Audio) -> bool:
    """Equal sample for sample over the shorter of the two, after alignment."""
    c, r = stereo(capture), stereo(reference)
    shift = align(c, r)
    if shift >= 0:
        c = c[shift:]
    else:
        r = r[-shift:]
    n = min(len(c), len(r))
    return n > 0 and bool(np.array_equal(c[:n], r[:n]))
