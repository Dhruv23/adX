"""bench_one_hour_soak (phase_10.md 4.5, 5.10): an hour of playback and editing, flat memory.

    py -3.12 bench/bench_one_hour_soak.py [--minutes 60] [--report build/bench/soak.json]

Plays the benchmark project (bench/make_project.py) on the null backend in real time,
looping, with every analyzer on and a UI frame at 60 Hz, which also drains the Reaper
(Engine.frame pumps the engine's housekeeping). Every two seconds a note moves and is
committed, and the next edit undoes it, so the project and its undo history stay the same
size: whatever memory grows is a leak, not the user's work.

The process's private bytes are sampled every 30 s. The first ten minutes are warm-up
(allocator pools, analyzer history, Python's caches). After that, memory must be flat:

| check | limit |
|---|---|
| growth, fitted over the post-warm-up samples, per hour | < 4 MB |
| last ten minutes' mean minus the ten after warm-up | < 4 MB |
| xruns / late callbacks | 0 |

Exit status 0 when every check passes. The report holds every sample either way.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np
import psutil

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))
sys.path.insert(0, str(ROOT / "bench"))

import make_project  # noqa: E402

from adx.engine_bridge import NOTE_DTYPE, Analyzers, Engine, Project, commands  # noqa: E402

RATE = 48000
BLOCK = 256
SAMPLE_EVERY = 30.0
EDIT_EVERY = 2.0
WARMUP_MINUTES = 10.0
WINDOW_MINUTES = 10.0
LIMIT_MB = 4.0


def private_mb() -> float:
    info = psutil.Process().memory_info()
    # Windows' private bytes; elsewhere RSS is the nearest thing.
    return float(getattr(info, "private", info.rss)) / 2**20


def move_one_note(project: Project, step: int) -> None:
    pattern = project.patterns()[0]
    channel = project.pattern_channels(pattern)[0]
    moved = project.notes(pattern, channel)[:1].copy()
    moved["start"] = (int(moved["start"][0]) + 960) % (3840 * 4)
    project.execute(
        commands.edit_notes(
            pattern,
            channel,
            np.zeros(0, np.uint32),
            moved,
            np.zeros(0, dtype=NOTE_DTYPE),
            f"soak edit {step}",
        )
    )


def soak(engine: Engine, project: Project, seconds: float) -> dict[str, object]:
    analyzers = Analyzers(engine)
    analyzers.configure(
        spectrum=True, spectrogram=True, scope=True, vector=True, loudness=True, particles=True
    )
    levels = np.zeros((64, 9), dtype=np.float32)
    engine.seek(0)
    engine.play()
    xruns = engine.xruns()
    late = engine.late_callbacks()
    samples: list[tuple[float, float]] = []
    edits = loops = 0
    started = time.perf_counter()
    next_frame = next_edit = started
    next_sample = started
    while (now := time.perf_counter()) - started < seconds:
        if now >= next_frame:
            frame = engine.frame_with_analyzers(levels, None, analyzers, 1 / 60)
            if not frame.rolling:
                # The end of the project: round again.
                engine.seek(0)
                engine.play()
                loops += 1
            next_frame += 1 / 60
        if now >= next_edit:
            if edits % 2 == 0:
                move_one_note(project, edits)
            else:
                project.undo()
            engine.commit(project)
            edits += 1
            next_edit += EDIT_EVERY
        if now >= next_sample:
            samples.append(((now - started) / 60.0, private_mb()))
            next_sample += SAMPLE_EVERY
        time.sleep(0.004)
    engine.stop_playback()
    samples.append(((time.perf_counter() - started) / 60.0, private_mb()))
    return {
        "samples": samples,
        "edits": edits,
        "loops": loops,
        "xruns": engine.xruns() - xruns,
        "late_callbacks": engine.late_callbacks() - late,
    }


def verdict(result: dict[str, object], minutes: float) -> dict[str, object]:
    samples = np.array(result["samples"])  # type: ignore[arg-type]
    t, mb = samples[:, 0], samples[:, 1]
    after = t >= min(WARMUP_MINUTES, minutes / 2)
    slope = float(np.polyfit(t[after], mb[after], 1)[0]) * 60.0 if after.sum() > 2 else 0.0
    first = (t >= t[after][0]) & (t < t[after][0] + WINDOW_MINUTES) & after
    last = t >= t[-1] - WINDOW_MINUTES
    step = float(mb[last].mean() - mb[first].mean()) if first.any() else 0.0
    checks = {
        "growth_mb_per_hour": (slope, LIMIT_MB),
        "last_minus_first_window_mb": (step, LIMIT_MB),
        "xruns": (float(result["xruns"]), 0.0),  # type: ignore[arg-type]
        "late_callbacks": (float(result["late_callbacks"]), 0.0),  # type: ignore[arg-type]
    }
    return {
        "start_mb": float(mb[0]),
        "after_warmup_mb": float(mb[after][0]),
        "end_mb": float(mb[-1]),
        "peak_mb": float(mb.max()),
        **{name: value for name, (value, _limit) in checks.items()},
        "failed": [name for name, (value, limit) in checks.items() if value > limit],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--minutes", type=float, default=60.0)
    parser.add_argument("--report", type=Path, default=ROOT / "build" / "bench" / "soak.json")
    args = parser.parse_args()

    project, diagnostics = Project.load(str(make_project.build(ROOT / "build" / "bench")))
    if any(d["severity"] == "error" for d in diagnostics):
        raise SystemExit("the benchmark project has errors")
    engine = Engine(null_backend=True, sample_rate=RATE, block_frames=BLOCK)
    engine.set_project(project)
    engine.start()
    result = soak(engine, project, args.minutes * 60.0)
    engine.stop()

    report = {"minutes": args.minutes, **verdict(result, args.minutes), **result}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")
    for key, value in report.items():
        if key != "samples":
            print(f"{key:28} {value}")
    return 1 if report["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
