"""How many dx7 voices one core renders in real time (phase_11.5.md 4.2): 48 kHz, the
msfa's 64-sample blocks, a busy six-operator voice (algorithm 1, feedback, every
operator on and audible), 64 notes held for 20 seconds.

    py -3.12 bench/bench_dx7.py [--voices 64] [--seconds 20]

Prints the render's share of one core and the voices one core holds; exits 1 below 64.
The offline render runs the same node the stream does.
"""

from __future__ import annotations

import argparse
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))

from adx import engine_bridge  # noqa: E402
from adx.engine_bridge import Project  # noqa: E402

FIXTURE = ROOT / "tests" / "fixtures" / "dx7" / "algorithms.syx"


def bars_for(seconds: float) -> int:
    """Bars of 4/4 at 120 bpm (two seconds each) that hold ``seconds``."""
    return max(1, int(seconds / 2.0) + 1)


def project_text(voices: int, seconds: float) -> str:
    voice = engine_bridge.dx7_read(FIXTURE.read_bytes())["voices"][0]  # algorithm 1
    for op in range(6):
        voice[op * 21 + 16] = 90  # every operator up
    values = engine_bridge.dx7_voice_to_params(voice)
    entry = next(
        t for t in engine_bridge.type_catalog() if t["kind"] == "instrument" and t["name"] == "dx7"
    )
    names = [str(d["name"]) for d in entry["params"]]
    params = "\n".join(f"PARAM {names[i]}={values[i]:g}" for i in range(156))
    bars = max(1, int(seconds * 2 / 4) + 1)  # 120 bpm
    notes = "\n".join(
        f"  {30 + (n % 64)} 0:0:0 {bars}:0:0 100" for n in range(voices)
    )  # raw MIDI numbers are not accepted; use names below
    del notes
    names_ = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    rows = []
    for n in range(voices):
        pitch = 30 + (n % 64)
        rows.append(f"  {names_[pitch % 12]}{pitch // 12 - 1} 0:0:0 {bars}:0:0 100")
    return f"""[PROJECT]
ADX_VERSION=2
BPM=120

[CHANNEL Bench]
INSTRUMENT=dx7
POLYPHONY={voices}
OUTPUT=insert.1
{params}
PARAM polyphony={voices}

[PATTERN Hold]
LENGTH={bars + 1}:0:0
NOTES Bench
{chr(10).join(rows)}

[PLAYLIST]
TRACK 1
  PATTERN Hold 0:0:0

[MIXER]
INSERT 1 name="Master"
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--voices", type=int, default=64)
    parser.add_argument("--seconds", type=float, default=20.0)
    args = parser.parse_args()
    project, diagnostics = Project.loads(project_text(args.voices, args.seconds))
    errors = [d for d in diagnostics if d["severity"] == "error"]
    if errors:
        print(errors, file=sys.stderr)
        return 2
    out = Path(tempfile.mkdtemp(prefix="adx-bench-dx7-")) / "dx7.wav"
    started = time.perf_counter()
    engine_bridge.render_to_file(project, str(out), tail_seconds=0.0)
    elapsed = time.perf_counter() - started
    from adx.testing.wav import read_wav

    audio, _rate = read_wav(str(out))
    peak = float(abs(audio).max())
    if peak < 1e-3:
        print("the render is silent: nothing was measured", file=sys.stderr)
        return 2
    rendered = (bars_for(args.seconds) + 1) * 2.0  # the pattern's length, 2 s a bar
    share = elapsed / rendered
    per_core = args.voices / share if share > 0 else float("inf")
    print(
        f"{args.voices} voices: {share * 100:.1f} % of one core; about {per_core:.0f} "
        f"voices per core at 48 kHz (peak {peak:.2f})"
    )
    return 0 if per_core >= 64 else 1


if __name__ == "__main__":
    sys.exit(main())
