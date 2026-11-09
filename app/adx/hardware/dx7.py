"""DX7 voices in the app (phase_11.5.md 4.3-4.5): a voice is 156 ints (the dx7
instrument's first 156 parameters, as bytes), opened as a dx7 channel, read back from
one, written to `.syx`, and - optionally, approximately - converted to the FM
instrument with a report of what was lost.
"""

from __future__ import annotations

import math
from pathlib import Path
from typing import Any

from adx import engine_bridge
from adx.session import Session

VOICE_BYTES = 156
NAME_OFFSET = 145
NAME_BYTES = 10
#: VCED offsets used here (operator n's block starts at (6 - n) * 21).
ALGORITHM, FEEDBACK, OSC_SYNC = 134, 135, 136
LFO_SPEED, LFO_DELAY, LFO_PMD, LFO_AMD, LFO_SYNC, LFO_WAVE, LFO_PMS = range(137, 144)
TRANSPOSE, OP_ENABLE = 144, 155
OP_FIELDS = (
    "eg.r1", "eg.r2", "eg.r3", "eg.r4", "eg.l1", "eg.l2", "eg.l3", "eg.l4", "kls.break",
    "kls.ldepth", "kls.rdepth", "kls.lcurve", "kls.rcurve", "krs", "ams", "kvs", "level",
    "mode", "coarse", "fine", "detune",
)  # fmt: skip


def op_byte(op: int, field: str) -> int:
    """The voice byte of operator ``op`` (1..6)'s ``field``."""
    return (6 - op) * 21 + OP_FIELDS.index(field)


def _descriptors() -> list[dict[str, Any]]:
    entry = next(
        t for t in engine_bridge.type_catalog() if t["kind"] == "instrument" and t["name"] == "dx7"
    )
    return list(entry["params"])


def param_names() -> list[str]:
    """The dx7 parameter names, in index order (the first 156 are the voice bytes)."""
    return [str(d["name"]) for d in _descriptors()]


def init_voice() -> list[int]:
    return engine_bridge.dx7_params_to_voice([float(d["default"]) for d in _descriptors()][:156])


def name_of(voice: list[int]) -> str:
    """The voice's name; bytes outside printable ASCII shown as '?'."""
    return "".join(
        chr(b) if 0x20 <= b < 0x7F else "?" for b in voice[NAME_OFFSET : NAME_OFFSET + NAME_BYTES]
    )


def with_name(voice: list[int], name: str) -> list[int]:
    """A copy named ``name`` (10 characters, space-padded; ASCII only)."""
    text = name.encode("ascii", "replace")[:NAME_BYTES].ljust(NAME_BYTES, b" ")
    out = list(voice)
    out[NAME_OFFSET : NAME_OFFSET + NAME_BYTES] = [b & 0x7F for b in text]
    return out


# --- files ---------------------------------------------------------------------------


def read_file(path: str | Path) -> dict[str, Any]:
    """Every voice in a `.syx` file (engine_bridge.dx7_read's result)."""
    return engine_bridge.dx7_read(Path(path).read_bytes())


def write_bank(path: str | Path, voices: list[list[int]]) -> Path:
    """A 32-voice bank (padded with INIT VOICE); several banks for more than 32."""
    out = Path(path)
    data = b"".join(
        engine_bridge.dx7_bank_sysex(voices[i : i + 32]) for i in range(0, max(1, len(voices)), 32)
    )
    out.write_bytes(data)
    return out


def write_voice(path: str | Path, voice: list[int]) -> Path:
    out = Path(path)
    out.write_bytes(engine_bridge.dx7_single_sysex(voice))
    return out


# --- channels ------------------------------------------------------------------------


def open_as_channel(session: Session, voice: list[int], name: str | None = None) -> str:
    """A new dx7 channel playing ``voice``, as one undo step; its name."""
    project = session.require()
    taken = {str(c["name"]) for c in project.channels_info()}
    base = (name or name_of(voice).strip() or "DX7").replace('"', "'")
    channel, n = base, 2
    while channel in taken:
        channel, n = f"{base} {n}", n + 1
    values = engine_bridge.dx7_voice_to_params(voice)
    names = param_names()
    with session.group(f"Open DX7 voice {base}"):
        session.build_and_execute("AddChannelNew", channel, "dx7", 0, "")
        cid = next(int(c["id"]) for c in project.channels_info() if c["name"] == channel)
        for index, value in enumerate(values):
            session.build_and_execute("SetChannelParam", cid, names[index], float(value))
    return channel


def channel_voice(session: Session, channel: int) -> list[int]:
    """The voice a dx7 channel plays now (a parameter it does not hold: its default)."""
    info = next(c for c in session.require().channels_info() if int(c["id"]) == channel)
    held = {str(p[0]): float(p[1]) for p in info["params"]}
    descriptors = _descriptors()[:VOICE_BYTES]
    values = [held.get(str(d["name"]), float(d["default"])) for d in descriptors]
    return engine_bridge.dx7_params_to_voice(values)


# --- conversion to the FM instrument (4.5) -------------------------------------------


def _rate_seconds(rate: int) -> float:
    """About how long a DX7 EG stage takes across its full range at ``rate``."""
    return float(41.0 * 2.0 ** (-rate / 7.0))


def _level(output_level: int) -> float:
    """A DX7 output level (0..99, about 0.75 dB a step) as a linear gain."""
    return 0.0 if output_level <= 0 else 10.0 ** ((output_level - 99) * 0.75 / 20.0)


def to_fm(voice: list[int]) -> tuple[dict[str, float], list[str]]:
    """The FM instrument's parameters for ``voice``, and every field dropped or
    approximated (empty for a voice that uses none of what FM lacks)."""
    p: dict[str, float] = {
        "algorithm": float(voice[ALGORITHM] + 1),
        "feedback": voice[FEEDBACK] / 7.0,
    }
    report: list[str] = []
    if voice[LFO_PMD] or voice[LFO_AMD]:
        report.append("LFO (speed, delay, pitch and amp depth, wave): FM has no LFO; dropped")
    if any(voice[126 + i] != 50 for i in range(4, 8)):
        report.append("pitch EG: FM has none; dropped")
    if voice[TRANSPOSE] != 24:
        report.append(f"transpose {voice[TRANSPOSE] - 24:+d}: FM has none; dropped")
    if voice[OSC_SYNC] == 0:
        report.append("oscillator key sync off: FM always starts at phase 0")
    for op in range(1, 7):

        def b(field: str, op: int = op) -> int:
            return voice[op_byte(op, field)]

        key = f"op{op}"
        on = bool(voice[OP_ENABLE] & (1 << (6 - op)))
        p[f"{key}.level"] = _level(b("level")) if on else 0.0
        if b("mode"):
            p[f"{key}.fixed"] = 1.0
            p[f"{key}.freq"] = min(20000.0, 10.0 ** ((b("coarse") & 3) + b("fine") / 100.0))
        else:
            coarse = 0.5 if b("coarse") == 0 else float(b("coarse"))
            p[f"{key}.ratio"] = max(0.5, min(32.0, coarse * (1.0 + b("fine") / 100.0)))
        p[f"{key}.detune"] = (b("detune") - 7) * 2.5
        p[f"{key}.velocity"] = b("kvs") / 7.0
        r = [b(f"eg.r{i}") for i in range(1, 5)]
        lv = [b(f"eg.l{i}") for i in range(1, 5)]
        p[f"{key}.attack"] = _rate_seconds(r[0]) * lv[0] / 99.0
        p[f"{key}.decay"] = _rate_seconds(r[1]) + (_rate_seconds(r[2]) if lv[1] != lv[2] else 0.0)
        p[f"{key}.sustain"] = _level(lv[2]) if lv[2] else 0.0
        p[f"{key}.release"] = _rate_seconds(r[3])
        if lv[0] != 99 or lv[1] != lv[2] or lv[3] != 0:
            report.append(f"{key} EG: four rates and levels fitted to an ADSR (approximate)")
        if b("kls.ldepth") or b("kls.rdepth"):
            report.append(f"{key} keyboard level scaling: dropped")
        if b("krs"):
            report.append(f"{key} keyboard rate scaling: dropped")
        if b("ams") and voice[LFO_AMD]:
            report.append(f"{key} amp mod sensitivity: dropped")
        if b("detune") != 7:
            report.append(f"{key} detune: approximated as {p[f'{key}.detune']:+.1f} cents")
    p["level"] = -6.0
    return p, report


def convert_channel_to_fm(session: Session, channel: int) -> tuple[str, list[str]]:
    """A new FM channel beside a dx7 one, from its voice (the dx7 channel is kept);
    the new channel's name and the conversion report."""
    voice = channel_voice(session, channel)
    params, report = to_fm(voice)
    project = session.require()
    taken = {str(c["name"]) for c in project.channels_info()}
    base = f"{name_of(voice).strip() or 'DX7'} (FM)"
    name, n = base, 2
    while name in taken:
        name, n = f"{base} {n}", n + 1
    with session.group(f"Convert {name_of(voice).strip()} to FM"):
        session.build_and_execute("AddChannelNew", name, "fm", 0, "")
        cid = next(int(c["id"]) for c in project.channels_info() if c["name"] == name)
        for param, value in params.items():
            if math.isfinite(value):
                session.build_and_execute("SetChannelParam", cid, param, float(value))
    return name, report
