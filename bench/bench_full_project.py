"""bench_full_project (phase_10.md 4.5, 6): the optimization pass's gate, and Phase 11's.

    py -3.12 bench/bench_full_project.py [--minutes 10] [--report build/bench/report.json]

Generates the benchmark project (bench/make_project.py), then measures what §4.5's table
asks for, engine-side:

| metric | target |
|---|---|
| audio-thread CPU at 256 frames / 48 kHz | < 50 % of one core: mean callback time / block period |
| worst block | < 60 % of the period, over the whole run |
| dropouts | 0 xruns |
| allocator-hook violations | 0 (meaningful only where the guard is compiled in) |
| project load | < 3 s |
| snapshot rebuild after one note edit | < 2 ms (the median of 20 edits; the worst is reported) |

The engine plays on the null backend in real time, every analyzer open and built at
60 Hz as the UI would, so their cost is on the same core budget. The UI frame time
(p99 < 16.6 ms, Phase 5's GPU harness) and memory over an hour are bench_ui.py and
bench_one_hour_soak.py, which drive the app itself.

Exit status 0 when every target is met. The report holds every number either way.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))
sys.path.insert(0, str(ROOT / "bench"))

import make_project  # noqa: E402

import adx_engine  # noqa: E402
from adx.engine_bridge import NOTE_DTYPE, Analyzers, Engine, Project, commands  # noqa: E402

RATE = 48000
BLOCK = 256
PERIOD_NS = BLOCK / RATE * 1e9

TARGETS = {
    "cpu_percent": 50.0,
    "worst_block_percent": 60.0,
    "xruns": 0,
    "rt_violations": 0,
    "load_seconds": 3.0,
    "rebuild_ms_median": 2.0,
}


def load(path: Path) -> tuple[Project, float]:
    started = time.perf_counter()
    project, diagnostics = Project.load(str(path))
    took = time.perf_counter() - started
    errors = [d for d in diagnostics if d["severity"] == "error"]
    if errors:
        raise SystemExit(f"the benchmark project has errors: {errors[:3]}")
    return project, took


def rebuild_times(engine: Engine, project: Project, edits: int = 20) -> list[float]:
    """One note moved, then the audio thread brought up to date, timed per edit."""
    pattern = project.patterns()[0]
    channel = project.pattern_channels(pattern)[0]
    out = []
    for i in range(edits):
        notes = project.notes(pattern, channel)
        moved = notes[:1].copy()
        moved["start"] = (int(moved["start"][0]) + 960 * (1 if i % 2 == 0 else -1)) % (3840 * 4)
        project.execute(
            commands.edit_notes(
                pattern,
                channel,
                np.zeros(0, np.uint32),
                moved,
                np.zeros(0, dtype=NOTE_DTYPE),
                "bench edit",
            )
        )
        started = time.perf_counter()
        engine.commit(project)
        out.append((time.perf_counter() - started) * 1000.0)
    return out


def play(engine: Engine, project: Project, seconds: float) -> dict[str, float]:
    analyzers = Analyzers(engine)
    analyzers.configure(
        spectrum=True, spectrogram=True, scope=True, vector=True, loudness=True, particles=True
    )
    levels = np.zeros((64, 9), dtype=np.float32)
    adx_engine.reset_rt_violations()
    engine.seek(0)
    engine.play()
    xruns = engine.xruns()
    cursor, lost, durations = 0, 0, []
    started = time.perf_counter()
    next_frame = started
    while time.perf_counter() - started < seconds:
        now = time.perf_counter()
        if now >= next_frame:
            engine.frame_with_analyzers(levels, None, analyzers, 1 / 60)
            next_frame += 1 / 60
        got, cursor, missed = engine.callback_timings(cursor)
        lost += missed
        if len(got):
            durations.append(got)
        time.sleep(0.004)
    engine.stop_playback()
    all_ns = np.concatenate(durations) if durations else np.zeros(0)
    return {
        "callbacks": float(len(all_ns)),
        "timings_lost": float(lost),
        "cpu_percent": float(all_ns.mean() / PERIOD_NS * 100) if len(all_ns) else 0.0,
        "p99_block_percent": float(np.percentile(all_ns, 99) / PERIOD_NS * 100)
        if len(all_ns)
        else 0.0,
        "worst_block_percent": float(all_ns.max() / PERIOD_NS * 100) if len(all_ns) else 0.0,
        "xruns": float(engine.xruns() - xruns),
        "late_callbacks": float(engine.late_callbacks()),
        "rt_violations": float(adx_engine.rt_violation_count()),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--minutes", type=float, default=10.0)
    parser.add_argument("--report", type=Path, default=ROOT / "build" / "bench" / "report.json")
    args = parser.parse_args()

    path = make_project.build(ROOT / "build" / "bench")
    project, load_seconds = load(path)
    engine = Engine(null_backend=True, sample_rate=RATE, block_frames=BLOCK)
    engine.set_project(project)
    engine.start()
    rebuilds = rebuild_times(engine, project)
    played = play(engine, project, args.minutes * 60)
    engine.stop()

    report: dict[str, object] = {
        "project": {
            "channels": len(project.channels()),
            "notes": int(project.info()["notes"]),
            "inserts": int(project.info()["inserts"]),
            "seconds": float(project.info()["length_seconds"]),
        },
        "load_seconds": load_seconds,
        "rebuild_ms_median": float(np.median(rebuilds)),
        "rebuild_ms_worst": float(max(rebuilds)),
        "minutes": args.minutes,
        "rt_guard_compiled_in": bool(adx_engine.rt_guard_enabled()),
        **played,
    }
    failed = [k for k, limit in TARGETS.items() if float(report[k]) > limit]  # type: ignore[arg-type]
    report["targets"] = TARGETS
    report["failed"] = failed
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")
    for key, value in report.items():
        if key not in ("targets",):
            print(f"{key:24} {value}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
