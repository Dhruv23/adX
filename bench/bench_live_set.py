"""live_set_no_dropouts (phase_11.md 5, 6): the phase gate, part 2.

    py -3.12 bench/bench_live_set.py [--minutes 30] [--report build/bench/live_set.json]

Thirty minutes of continuous performance on the Phase 10 benchmark project: adX is
launched through adX.cmd on the real audio device with the benchmark project, a session
is cut from its patterns (tests/scenarios/live_set.py), and the session dock and the
playlist are open. Inside the app a timer plays a set script on the session - a
launch, a scene, a stop or a stop-all every few seconds, nudges and taps - while the
arrangement plays underneath, looped. Every frame-clock tick is timed from inside the
app, the session grid's build included.

| metric | target |
|---|---|
| dropouts | 0 xruns over the run |
| allocator-hook violations | 0: the Debug ten-minute session_no_alloc is the hook's run |
| UI frame work, p99 | < 16.6 ms |
| launches refused | 0 |

Exit status 0 when every target is met. The report holds every number either way.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))
sys.path.insert(0, str(ROOT / "bench"))
sys.path.insert(0, str(ROOT / "tests" / "scenarios"))

import make_project  # noqa: E402
from live_set import build_set, set_script  # noqa: E402

from adx.testing.driver import Driver  # noqa: E402

BUDGET_MS = 1000.0 / 60.0

HOOK = """
import time
clock = window.clock
work, stamps = [], []
original = clock.tick
def timed():
    began = time.perf_counter()
    original()
    window.session_view.refresh_states()
    done = time.perf_counter()
    work.append(done - began)
    stamps.append(began)
clock._timer.timeout.disconnect()
clock._timer.timeout.connect(timed)
window._bench = (work, stamps)
result = True
"""

PERFORM = """
import time
from PySide6.QtCore import QTimer
steps = {steps!r}
state = {{"i": 0, "start": time.perf_counter()}}
e = window.engine
def run():
    now = time.perf_counter() - state["start"]
    while state["i"] < len(steps) and steps[state["i"]][0] <= now:
        at, verb, args = steps[state["i"]]
        state["i"] += 1
        if verb == "launch":
            e.launch_clip(args[0], args[1], args[2] / 127.0)
        elif verb == "scene":
            e.fire_scene(args[0])
        elif verb == "stop":
            e.stop_clip(args[0])
        elif verb == "stopall":
            e.stop_all_clips()
        elif verb == "tap":
            e.tap_tempo(time.monotonic())
        elif verb == "nudge":
            window.nudge(args[0])
            QTimer.singleShot(800, lambda: window.nudge(0))
timer = QTimer(window)
timer.setInterval(20)
timer.timeout.connect(run)
timer.start()
window._perform = (timer, state, len(steps))
result = len(steps)
"""

READ = """
import numpy as np
work, stamps = window._bench
w = np.array(work) * 1000
i = np.diff(np.array(stamps)) * 1000
timer, state, total = window._perform
result = dict(frames=len(w), p50_ms=float(np.percentile(w, 50)), p99_ms=float(np.percentile(w, 99)),
              worst_ms=float(w.max()), interval_p99_ms=float(np.percentile(i, 99)),
              steps_done=state["i"], steps=total, counts=window.engine.session_counts,
              rate=window.engine.session_rate)
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--minutes", type=float, default=30.0)
    parser.add_argument("--report", type=Path, default=ROOT / "build" / "bench" / "live_set.json")
    args = parser.parse_args()
    bench = make_project.build(ROOT / "build" / "bench")
    live = bench.with_name("live_set.adx")
    info = build_set(bench, live)
    # The arrangement plays too: build_set left its tracks and took its placements, so
    # the benchmark's own arrangement is put back under the session.
    text = live.read_text(encoding="utf-8")
    original = bench.read_text(encoding="utf-8")
    start = original.index("[PLAYLIST]")
    end = original.index("\n[", start + 1)
    head = text.index("[PLAYLIST]")
    tail = text.index("\n[", head + 1)
    live.write_text(text[:head] + original[start:end] + text[tail:], encoding="utf-8")
    steps = [(s.at, s.verb, list(s.args)) for s in set_script(args.minutes, info)]
    stats: dict[str, object] = {"minutes": args.minutes}
    with Driver.launch(
        live, via="adX.cmd", timeout=180, null_audio=False, env={"ADX_BEHIND": "0"}
    ) as app:
        app.wait_for("project.open", timeout=120)
        for panel in ("session", "playlist", "mixer"):
            app.trigger(f"view.{panel}")
        app.script(HOOK)
        app.trigger("transport.loop")
        app.trigger("transport.play")
        app.wait_for("transport.playing", timeout=10)
        before = int(app.state()["audio"]["xruns"])
        app.script(PERFORM.format(steps=steps))
        time.sleep(args.minutes * 60.0 + 2.0)
        stats.update(dict(app.script(READ)))
        audio = app.state()["audio"]
        stats["xruns"] = int(audio["xruns"]) - before
        stats["worst_callback_ms"] = float(audio["worst_callback_ms"])
        stats["null_backend"] = bool(audio["null"])
    counts = stats["counts"]
    assert isinstance(counts, dict)
    stats["ok"] = (
        stats["xruns"] == 0
        and float(stats["p99_ms"]) < BUDGET_MS  # type: ignore[arg-type]
        and int(counts["refused"]) == 0
        and stats["steps_done"] == stats["steps"]
        # On the timer backend there is no device to drop out: not a pass.
        and not stats["null_backend"]
    )
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(stats, indent=2), encoding="utf-8")
    print(json.dumps(stats))
    return 0 if stats["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
