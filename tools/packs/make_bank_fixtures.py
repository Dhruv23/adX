"""Writes the tiny SF2 and DLS test banks (phase_12.md 4.7.4, 5).

    python tools/packs/make_bank_fixtures.py [--out tests/data/banks]

Both banks hold the same two things, so the two parsers can be held to one answer
(dls_parse_and_map):

  bank 0, program 0   "Sine": one looped cycle of a 440 Hz sine at 44.1 kHz, root A4,
                      attack 0.05 s, hold 0.1 s, decay 0.4 s to -12 dB, release 0.3 s,
                      vibrato 5 Hz +-20 cents after 0.2 s, filter 8 kHz
  bank 128, program 0 "Kit": closed hat on key 42 and open hat on key 46, noise bursts,
                      both in exclusive class 1

Deterministic: the noise is seeded, and the same script writes the same bytes.
"""

from __future__ import annotations

import argparse
import math
import random
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "tests" / "data" / "banks"
RATE = 44100

# The sine: 100 cycles of 441 Hz is exactly 10000 frames, so the loop is seamless and
# the pitch exactly 441 Hz at root - a test reads 441 * 2^((key - 69) / 12).
SINE_FRAMES = 10000
SINE_HZ = 441.0


def sine() -> list[int]:
    return [round(16000 * math.sin(2 * math.pi * SINE_HZ * n / RATE)) for n in range(SINE_FRAMES)]


def noise(seed: int, frames: int, decay: float) -> list[int]:
    rng = random.Random(seed)
    return [
        round(12000 * rng.uniform(-1, 1) * math.exp(-n / (decay * RATE))) for n in range(frames)
    ]


def timecents(seconds: float) -> int:
    return -12000 if seconds <= 0 else round(1200 * math.log2(seconds))


def chunk(cid: bytes, body: bytes) -> bytes:
    pad = b"\0" if len(body) % 2 else b""
    return cid + struct.pack("<I", len(body)) + body + pad


def lst(kind: bytes, body: bytes) -> bytes:
    return chunk(b"LIST", kind + body)


def zstr(text: str, width: int) -> bytes:
    raw = text.encode("ascii")[: width - 1]
    return raw + b"\0" * (width - len(raw))


# --- SF2 -----------------------------------------------------------------------------

# Generator ids (SoundFont 2.04 8.1.2).
G = {
    "initialFilterFc": 8,
    "pan": 17,
    "delayVibLFO": 23,
    "freqVibLFO": 24,
    "vibLfoToPitch": 6,
    "attackVolEnv": 34,
    "holdVolEnv": 35,
    "decayVolEnv": 36,
    "sustainVolEnv": 37,
    "releaseVolEnv": 38,
    "instrument": 41,
    "keyRange": 43,
    "sampleID": 53,
    "sampleModes": 54,
    "exclusiveClass": 57,
    "overridingRootKey": 58,
}


def gen(name: str, value: int) -> bytes:
    return struct.pack("<Hh", G[name], value)


def key_range(lo: int, hi: int) -> bytes:
    return struct.pack("<HBB", G["keyRange"], lo, hi)


def sf2() -> bytes:
    samples = [
        ("Sine", sine(), 69, True),
        ("Closed", noise(1, 4410, 0.03), 60, False),
        ("Open", noise(2, 22050, 0.25), 60, False),
    ]
    smpl = b""
    shdr = b""
    offset = 0
    for name, data, root, _looped in samples:
        start = offset
        smpl += struct.pack(f"<{len(data)}h", *data) + b"\0\0" * 46  # 46 zero guard frames
        end = start + len(data)
        # A looped sample loops the whole of itself; the loop of one that is not is unused.
        loop_start, loop_end = start, end
        shdr += zstr(name, 20) + struct.pack(
            "<IIIIIBbHH", start, end, loop_start, loop_end, RATE, root, 0, 0, 1
        )
        offset = end + 46
    shdr += zstr("EOS", 20) + struct.pack("<IIIIIBbHH", 0, 0, 0, 0, 0, 0, 0, 0, 0)

    # Instruments: 0 Sine, 1 Kit. Each zone's generators, sampleID last.
    sine_zone = b"".join(
        [
            gen("attackVolEnv", timecents(0.05)),
            gen("holdVolEnv", timecents(0.1)),
            gen("decayVolEnv", timecents(0.4)),
            gen("sustainVolEnv", 120),
            gen("releaseVolEnv", timecents(0.3)),
            gen("delayVibLFO", timecents(0.2)),
            gen("freqVibLFO", round(1200 * math.log2(5 / 8.176))),
            gen("vibLfoToPitch", 20),
            gen("initialFilterFc", round(1200 * math.log2(8000 / 8.176))),
            gen("sampleModes", 1),
            gen("sampleID", 0),
        ]
    )
    closed = b"".join(
        [
            key_range(42, 42),
            gen("exclusiveClass", 1),
            gen("overridingRootKey", 42),
            gen("sampleID", 1),
        ]
    )
    opened = b"".join(
        [
            key_range(46, 46),
            gen("exclusiveClass", 1),
            gen("releaseVolEnv", timecents(0.5)),
            gen("overridingRootKey", 46),
            gen("sampleID", 2),
        ]
    )
    igen = sine_zone + closed + opened + struct.pack("<Hh", 0, 0)
    counts = [len(sine_zone) // 4, len(closed) // 4, len(opened) // 4]
    ibag = b""
    g = 0
    for c in counts:
        ibag += struct.pack("<HH", g, 0)
        g += c
    ibag += struct.pack("<HH", g, 0)
    inst = zstr("Sine", 20) + struct.pack("<H", 0) + zstr("Kit", 20) + struct.pack("<H", 1)
    inst += zstr("EOI", 20) + struct.pack("<H", 3)
    imod = b"\0" * 10

    pgen = gen("instrument", 0) + gen("instrument", 1) + struct.pack("<Hh", 0, 0)
    pbag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 1, 0) + struct.pack("<HH", 2, 0)
    phdr = zstr("Sine", 20) + struct.pack("<HHHIII", 0, 0, 0, 0, 0, 0)
    phdr += zstr("Kit", 20) + struct.pack("<HHHIII", 0, 128, 1, 0, 0, 0)
    phdr += zstr("EOP", 20) + struct.pack("<HHHIII", 0, 0, 2, 0, 0, 0)
    pmod = b"\0" * 10

    info = lst(b"INFO", chunk(b"ifil", struct.pack("<HH", 2, 4)) + chunk(b"INAM", b"adX tiny GM\0"))
    sdta = lst(b"sdta", chunk(b"smpl", smpl))
    pdta = lst(
        b"pdta",
        chunk(b"phdr", phdr)
        + chunk(b"pbag", pbag)
        + chunk(b"pmod", pmod)
        + chunk(b"pgen", pgen)
        + chunk(b"inst", inst)
        + chunk(b"ibag", ibag)
        + chunk(b"imod", imod)
        + chunk(b"igen", igen)
        + chunk(b"shdr", shdr),
    )
    return chunk(b"RIFF", b"sfbk" + info + sdta + pdta)


# --- DLS -----------------------------------------------------------------------------


def connection(source: int, dest: int, scale: float) -> bytes:
    return struct.pack("<HHHHi", source, 0, dest, 0, round(scale * 65536))


def art1(blocks: list[bytes]) -> bytes:
    return chunk(b"art1", struct.pack("<II", 8, len(blocks)) + b"".join(blocks))


def wsmp(unity: int, loop: tuple[int, int] | None) -> bytes:
    body = struct.pack("<IHhiII", 20, unity, 0, 0, 0, 1 if loop else 0)
    if loop:
        body += struct.pack("<IIII", 16, 0, loop[0], loop[1])
    return chunk(b"wsmp", body)


def region(
    lo: int,
    hi: int,
    cue: int,
    key_group: int,
    root: int,
    loop: tuple[int, int] | None,
    articulation: bytes = b"",
) -> bytes:
    rgnh = chunk(b"rgnh", struct.pack("<HHHHHH", lo, hi, 0, 127, 0, key_group))
    wlnk = chunk(b"wlnk", struct.pack("<HHII", 0, 0, 1, cue))
    return lst(b"rgn ", rgnh + wsmp(root, loop) + wlnk + articulation)


def dls() -> bytes:
    datas = [sine(), noise(1, 4410, 0.03), noise(2, 22050, 0.25)]
    waves = b""
    offsets = []
    for data in datas:
        offsets.append(len(waves))
        fmt = chunk(b"fmt ", struct.pack("<HHIIHH", 1, 1, RATE, RATE * 2, 2, 16))
        waves += lst(b"wave", fmt + chunk(b"data", struct.pack(f"<{len(data)}h", *data)))
    ptbl = chunk(
        b"ptbl",
        struct.pack("<II", 8, len(offsets)) + b"".join(struct.pack("<I", o) for o in offsets),
    )
    # The same envelope, LFO and filter as the SF2's sine, as connection blocks.
    sine_art = lst(
        b"lart",
        art1(
            [
                connection(0, 0x0206, timecents(0.05)),
                connection(0, 0x020C, timecents(0.1)),
                connection(0, 0x0207, timecents(0.4)),
                connection(0, 0x020A, 1000 * 10 ** (-12 / 20)),
                connection(0, 0x0209, timecents(0.3)),
                connection(0, 0x0115, timecents(0.2)),
                connection(0, 0x0114, 6900 + 1200 * math.log2(5 / 440)),
                connection(9, 0x0003, 20),
                connection(0, 0x0500, 6900 + 1200 * math.log2(8000 / 440)),
            ]
        ),
    )
    sine_ins = lst(
        b"ins ",
        chunk(b"insh", struct.pack("<III", 1, 0, 0))
        + lst(b"lrgn", region(0, 127, 0, 0, 69, (0, SINE_FRAMES)))
        + sine_art
        + lst(b"INFO", chunk(b"INAM", b"Sine\0")),
    )
    release = lst(b"lart", art1([connection(0, 0x0209, timecents(0.5))]))
    kit = lst(
        b"ins ",
        chunk(b"insh", struct.pack("<III", 2, 0x80000000, 0))
        + lst(b"lrgn", region(42, 42, 1, 1, 42, None) + region(46, 46, 2, 1, 46, None, release))
        + lst(b"INFO", chunk(b"INAM", b"Kit\0")),
    )
    colh = chunk(b"colh", struct.pack("<I", 2))
    lins = lst(b"lins", sine_ins + kit)
    info = lst(b"INFO", chunk(b"INAM", b"adX tiny GM\0"))
    return chunk(b"RIFF", b"DLS " + colh + lins + ptbl + lst(b"wvpl", waves) + info)


def generate(out: Path = OUT) -> list[Path]:
    out.mkdir(parents=True, exist_ok=True)
    paths = [out / "tiny_gm.sf2", out / "tiny_gm.dls"]
    paths[0].write_bytes(sf2())
    paths[1].write_bytes(dls())
    return paths


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=OUT)
    args = parser.parse_args(argv)
    for path in generate(args.out):
        print(f"{path} ({path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
