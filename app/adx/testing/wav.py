"""Reading and writing WAV for the testing tools, with numpy alone (no Qt, no scipy):
16- and 24-bit PCM and 32-bit float, any rate, mono or stereo, RF64 not needed for the
lengths a scenario captures."""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
import numpy.typing as npt

Audio = npt.NDArray[np.float32]


def read_wav(path: str | Path) -> tuple[Audio, int]:
    """(frames x channels float32 in -1..1, sample rate)."""
    data = Path(path).read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF WAVE file")
    at = 12
    fmt: tuple[int, int, int, int] | None = None
    while at + 8 <= len(data):
        chunk, size = data[at : at + 4], struct.unpack_from("<I", data, at + 4)[0]
        body = data[at + 8 : at + 8 + size]
        if chunk == b"fmt ":
            tag, channels, rate = struct.unpack_from("<HHI", body, 0)
            bits = struct.unpack_from("<H", body, 14)[0]
            if tag == 0xFFFE and len(body) >= 26:  # WAVE_FORMAT_EXTENSIBLE: the real tag
                tag = struct.unpack_from("<H", body, 24)[0]
            fmt = (tag, channels, rate, bits)
        elif chunk == b"data":
            if fmt is None:
                raise ValueError(f"{path}: data before fmt")
            tag, channels, rate, bits = fmt
            if tag == 3 and bits == 32:
                samples = np.frombuffer(body, dtype="<f4").astype(np.float32)
            elif tag == 1 and bits == 16:
                samples = np.frombuffer(body, dtype="<i2").astype(np.float32) / 32768.0
            elif tag == 1 and bits == 24:
                raw = (
                    np.frombuffer(body[: len(body) // 3 * 3], dtype=np.uint8)
                    .reshape(-1, 3)
                    .astype(np.int32)
                )
                ints = raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)
                ints = np.where(ints >= 1 << 23, ints - (1 << 24), ints)
                samples = (ints / float(1 << 23)).astype(np.float32)
            elif tag == 1 and bits == 32:
                samples = (np.frombuffer(body, dtype="<i4") / float(1 << 31)).astype(np.float32)
            else:
                raise ValueError(f"{path}: unsupported WAV format {tag}/{bits}")
            return samples.reshape(-1, channels), rate
        at += 8 + size + (size & 1)
    raise ValueError(f"{path}: no data chunk")


def write_wav(path: str | Path, audio: Audio, rate: int) -> None:
    """32-bit float WAV."""
    frames = np.ascontiguousarray(audio, dtype="<f4")
    if frames.ndim == 1:
        frames = frames.reshape(-1, 1)
    channels = frames.shape[1]
    body = frames.tobytes()
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + len(body),
        b"WAVE",
        b"fmt ",
        16,
        3,
        channels,
        rate,
        rate * channels * 4,
        channels * 4,
        32,
        b"data",
        len(body),
    )
    Path(path).write_bytes(header + body)


def stereo(audio: npt.ArrayLike) -> Audio:
    """(frames, 2) float32 from mono, stereo or interleaved-flat input."""
    a = np.asarray(audio, dtype=np.float32)
    if a.ndim == 1:
        return np.stack([a, a], axis=1)
    if a.shape[1] == 1:
        return np.concatenate([a, a], axis=1)
    return a[:, :2]
