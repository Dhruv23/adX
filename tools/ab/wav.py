"""Read a WAV file into a float array, whatever its sample format.

Python's `wave` module refuses IEEE float (format 3), which is what both engines
write, so this reads the RIFF chunks directly. Integer formats are scaled to [-1, 1).
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

import numpy as np
import numpy.typing as npt

FloatArray = npt.NDArray[np.float64]


def _chunks(data: bytes) -> dict[bytes, bytes]:
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    found: dict[bytes, bytes] = {}
    offset = 12
    while offset + 8 <= len(data):
        tag = data[offset : offset + 4]
        (size,) = struct.unpack_from("<I", data, offset + 4)
        found.setdefault(tag, data[offset + 8 : offset + 8 + size])
        offset += 8 + size + (size & 1)
    return found


def load(path: str | Path) -> tuple[FloatArray, int]:
    """(frames x channels samples, sample rate)."""
    chunks = _chunks(Path(path).read_bytes())
    fmt, raw = chunks[b"fmt "], chunks[b"data"]
    code, channels, rate = struct.unpack_from("<HHI", fmt, 0)
    (bits,) = struct.unpack_from("<H", fmt, 14)
    if code == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: the real code leads the subformat GUID
        (code,) = struct.unpack_from("<H", fmt, 24)
    if code == 3:
        samples = np.frombuffer(raw, dtype="<f4" if bits == 32 else "<f8").astype(np.float64)
    elif bits == 16:
        samples = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif bits == 24:
        triples = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        packed = triples[:, 0] | (triples[:, 1] << 8) | (triples[:, 2] << 16)
        samples = ((packed << 8) >> 8).astype(np.float64) / 8388608.0
    elif bits == 32:
        samples = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        raise ValueError(f"unsupported WAV: format {code}, {bits} bits")
    return samples.reshape(-1, channels), int(rate)


def main(argv: list[str]) -> int:
    for path in argv:
        x, rate = load(path)
        rms = float(np.sqrt(np.mean(x**2)))
        print(f"{path}: {len(x)} frames @ {rate} Hz, peak {np.abs(x).max():.4f}, rms {rms:.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
