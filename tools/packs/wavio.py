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


def read_any(path: Path) -> tuple[np.ndarray, int]:
    """PCM 16/24/32 or float32 WAV -> (frames x channels floats, rate)."""
    blob = path.read_bytes()
    pos = 12
    fmt: tuple[int, int, int, int] | None = None
    while pos + 8 <= len(blob):
        tag = blob[pos : pos + 4]
        size = struct.unpack_from("<I", blob, pos + 4)[0]
        body = pos + 8
        if tag == b"fmt ":
            code, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", blob, body)
            fmt = (code, channels, rate, bits)
        elif tag == b"data" and fmt is not None:
            code, channels, rate, bits = fmt
            raw = blob[body : body + size]
            if code == 3 and bits == 32:
                data = np.frombuffer(raw, dtype="<f4")
            elif bits == 16:
                data = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
            elif bits == 24:
                triples = np.frombuffer(raw, dtype=np.uint8)[: len(raw) // 3 * 3].reshape(-1, 3)
                ints = (
                    triples[:, 0].astype(np.int32)
                    | (triples[:, 1].astype(np.int32) << 8)
                    | (triples[:, 2].astype(np.int32) << 16)
                )
                ints = np.where(ints >= 1 << 23, ints - (1 << 24), ints)
                data = ints.astype(np.float32) / 8388608.0
            elif bits == 32:
                data = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0
            else:
                raise ValueError(f"{path.name}: {bits}-bit format {code} not supported")
            return data[: len(data) // channels * channels].reshape(-1, channels), rate
        pos = body + size + (size & 1)
    raise ValueError(f"{path.name}: no audio data")
