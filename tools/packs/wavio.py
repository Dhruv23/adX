"""24-bit PCM WAV in and out, for the pack generators.

24-bit rather than float: half the size of float32 for a table or an IR, about
-144 dBFS of quantisation, and every reader handles it (miniaudio, Serum, Vital).
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np

FULL = 8388607  # 2**23 - 1


def write_wav(path: Path, samples: np.ndarray, rate: int) -> None:
    """Write ``samples`` (frames, or frames x channels, in -1..1) as 24-bit PCM."""
    data = np.asarray(samples, dtype=np.float64)
    if data.ndim == 1:
        data = data[:, None]
    channels = data.shape[1]
    ints = np.clip(np.round(data * FULL), -FULL - 1, FULL).astype("<i4")
    raw = ints.reshape(-1).view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
    block = channels * 3
    header = b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * block, block, 24)
    header += b"data" + struct.pack("<I", len(raw))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + raw)


def read_wav(path: Path) -> tuple[np.ndarray, int]:
    """A file ``write_wav`` wrote: (frames x channels floats, rate)."""
    blob = path.read_bytes()
    channels, rate = struct.unpack_from("<HI", blob, 22)
    size = struct.unpack_from("<I", blob, 40)[0]
    raw = np.frombuffer(blob, dtype=np.uint8, count=size, offset=44).reshape(-1, 3)
    padded = np.zeros((raw.shape[0], 4), dtype=np.uint8)
    padded[:, 1:] = raw
    ints = padded.view("<i4").reshape(-1) >> 8
    return (ints.astype(np.float64) / FULL).reshape(-1, channels), rate
