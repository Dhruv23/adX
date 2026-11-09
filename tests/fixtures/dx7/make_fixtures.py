"""Write the DX7 test fixtures (phase_11.5.md 5): synthesized voices only, never a copied
factory or commercial bank.

    py -3.12 tests/fixtures/dx7/make_fixtures.py

The packing here is written from the DX7 SysEx format on its own, independently of
engine/midi/dx7/Dx7Codec.cpp, so the codec tests compare two implementations.
"""

from __future__ import annotations

import random
from pathlib import Path

HERE = Path(__file__).resolve().parent

OP_MAX = [99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 3, 3, 7, 3, 7, 99, 1, 31, 99, 14]
VOICE_MAX = [99] * 8 + [31, 7, 1, 99, 99, 99, 99, 1, 5, 7, 48]


def init_voice(name: str = "INIT VOICE") -> list[int]:
    """155 VCED bytes (operator 6 first)."""
    v = [0] * 155
    for op in range(6):
        b = op * 21
        v[b : b + 4] = [99, 99, 99, 99]
        v[b + 4 : b + 8] = [99, 99, 99, 0]
        v[b + 8] = 39
        v[b + 16] = 99 if op == 5 else 0  # operator 1 is the last block
        v[b + 18] = 1
        v[b + 20] = 7
    v[126:130] = [99] * 4
    v[130:134] = [50] * 4
    v[136] = 1
    v[137] = 35
    v[141] = 1
    v[143] = 3
    v[144] = 24
    v[145:155] = list(name.ljust(10)[:10].encode("ascii"))
    return v


def checksum(data: list[int]) -> int:
    return (128 - (sum(data) & 0x7F)) & 0x7F


def pack(v: list[int]) -> list[int]:
    """VCED to the 128-byte VMEM form."""
    out = [0] * 128
    for op in range(6):
        s, d = op * 21, op * 17
        out[d : d + 11] = v[s : s + 11]
        out[d + 11] = v[s + 11] | (v[s + 12] << 2)
        out[d + 12] = v[s + 13] | (v[s + 20] << 3)
        out[d + 13] = v[s + 14] | (v[s + 15] << 2)
        out[d + 14] = v[s + 16]
        out[d + 15] = v[s + 17] | (v[s + 18] << 1)
        out[d + 16] = v[s + 19]
    out[102:111] = v[126:135]
    out[111] = v[135] | (v[136] << 3)
    out[112:116] = v[137:141]
    out[116] = v[141] | (v[142] << 1) | (v[143] << 4)
    out[117:128] = v[144:155]
    return out


def bank(voices: list[list[int]], bad: bool = False) -> bytes:
    data = [b for v in voices for b in pack(v)]
    assert len(data) == 4096
    cs = checksum(data)
    if bad:
        cs = (cs + 1) & 0x7F
    return bytes([0xF0, 0x43, 0x00, 0x09, 0x20, 0x00, *data, cs, 0xF7])


def single(v: list[int]) -> bytes:
    return bytes([0xF0, 0x43, 0x00, 0x00, 0x01, 0x1B, *v, checksum(v), 0xF7])


def playable(rng: random.Random, name: str) -> list[int]:
    """A random voice that sounds within a second: fast-ish attacks, a carrier up."""
    v = init_voice(name)
    for op in range(6):
        b = op * 21
        v[b : b + 4] = [
            rng.randint(60, 99),
            rng.randint(30, 99),
            rng.randint(20, 99),
            rng.randint(30, 99),
        ]
        v[b + 4 : b + 8] = [99, rng.randint(60, 99), rng.randint(40, 99), 0]
        v[b + 8] = rng.randint(0, 99)
        v[b + 9] = rng.randint(0, 99)
        v[b + 10] = rng.randint(0, 99)
        v[b + 11] = rng.randint(0, 3)
        v[b + 12] = rng.randint(0, 3)
        v[b + 13] = rng.randint(0, 7)
        v[b + 14] = rng.randint(0, 3)
        v[b + 15] = rng.randint(0, 7)
        v[b + 16] = rng.randint(50, 99)
        v[b + 17] = 1 if rng.random() < 0.1 else 0
        v[b + 18] = rng.randint(0, 8)
        v[b + 19] = rng.randint(0, 99)
        v[b + 20] = rng.randint(0, 14)
    v[126:130] = [rng.randint(50, 99) for _ in range(4)]
    v[130:134] = [rng.randint(40, 60) for _ in range(4)]
    v[134] = rng.randint(0, 31)
    v[135] = rng.randint(0, 7)
    v[136] = rng.randint(0, 1)
    v[137] = rng.randint(0, 99)
    v[138] = rng.randint(0, 99)
    v[139] = rng.randint(0, 99)
    v[140] = rng.randint(0, 99)
    v[141] = rng.randint(0, 1)
    v[142] = rng.randint(0, 5)
    v[143] = rng.randint(0, 7)
    v[144] = rng.randint(12, 36)
    return v


def algorithms() -> list[list[int]]:
    rng = random.Random(32)
    out = []
    for k in range(32):
        v = playable(rng, f"ALG {k + 1:02d}")
        v[134] = k
        v[135] = k % 8
        out.append(v)
    return out


def extremes() -> list[list[int]]:
    """Every field at its ends: curves, scaling, EG rates and levels, LFO waves."""
    out = []
    for k in range(32):
        v = init_voice(f"EXTREME{k:02d}")
        hi = k % 2 == 1
        for op in range(6):
            b = op * 21
            v[b : b + 4] = [99, 0 if k % 4 == 2 else 99, 99 if hi else 0, 99 if hi else 50]
            v[b + 4 : b + 8] = [99, 99 if hi else 0, 0, 99 if k % 8 == 7 else 0]
            v[b + 8] = 99 if hi else 0
            v[b + 9] = 99 if k % 3 else 0
            v[b + 10] = 0 if k % 3 else 99
            v[b + 11] = k % 4
            v[b + 12] = (k // 4) % 4
            v[b + 13] = 7 if hi else 0
            v[b + 14] = 3 if hi else 0
            v[b + 15] = 7 if k % 5 else 0
            v[b + 16] = 99 if op in (5, 4) else (k * 3) % 100
            v[b + 17] = 1 if k % 7 == 3 and op == 0 else 0
            v[b + 18] = 31 if k % 6 == 5 else (0 if k % 6 == 4 else 1)
            v[b + 19] = 99 if hi else 0
            v[b + 20] = 14 if hi else 0
        v[126:130] = [99, 0, 99, 0] if hi else [0, 99, 0, 99]
        v[130:134] = [0, 99, 99, 0] if hi else [99, 0, 50, 50]
        v[134] = k
        v[135] = 7 if hi else 0
        v[136] = k % 2
        v[137] = 99 if k % 3 == 0 else 0
        v[138] = 99 if k % 3 == 1 else 0
        v[139] = 99
        v[140] = 99 if hi else 0
        v[141] = k % 2
        v[142] = k % 6
        v[143] = 7 if hi else 0
        v[144] = 0 if k % 4 == 0 else (48 if k % 4 == 1 else 24)
        out.append(v)
    return out


def check(v: list[int]) -> None:
    for i, value in enumerate(v):
        top = OP_MAX[i % 21] if i < 126 else (VOICE_MAX[i - 126] if i < 145 else 127)
        assert 0 <= value <= top, (i, value)


def main() -> None:
    algo = algorithms()
    ext = extremes()
    rng = random.Random(7)
    rand = [playable(rng, f"RANDOM {i:02d}") for i in range(32)]
    for v in [*algo, *ext, *rand]:
        check(v)
    (HERE / "algorithms.syx").write_bytes(bank(algo))
    (HERE / "extremes.syx").write_bytes(bank(ext))
    (HERE / "random.syx").write_bytes(bank(rand))
    (HERE / "bad_checksum.syx").write_bytes(bank(rand, bad=True))
    banks = []
    for b in range(8):
        r = random.Random(100 + b)
        banks.append(bank([playable(r, f"SLOOP{b}{i:02d}") for i in range(32)]))
    (HERE / "sloopdx_256.syx").write_bytes(b"".join(banks))
    (HERE / "single_voice.syx").write_bytes(single(algo[4]))
    print("wrote", sorted(p.name for p in HERE.glob("*.syx")))


if __name__ == "__main__":
    main()
