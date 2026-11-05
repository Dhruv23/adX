"""The UI half of bench_full_project (phase_10.md 4.5): frame time with every panel open.

    py -3.12 bench/bench_ui.py [--seconds 60]

Launches adX through adX.cmd on the real display with the benchmark project, opens the
six analyzers and the playlist, piano roll and mixer, plays, and times every frame-clock
tick from inside the app (the engine call and every panel's frame work). Prints JSON:
the work's p99 is the gate (< 16.6 ms); the tick-to-tick interval's p99 shows whether
frames are being missed (a 60 Hz timer that keeps up stays near 16.7 ms).
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

import make_project  # noqa: E402

from adx.testing.driver import Driver  # noqa: E402

PANELS = (
    "playlist",
    "piano_roll",
    "mixer",
    "spectrum",
    "spectrogram",
    "scope",
    "vectorscope",
    "loudness",
    "particles",
)
BUDGET_MS = 1000.0 / 60.0

# The analyzers share a tab group, and a tab behind another is switched off
# (analyzers/host.py on_screen), so raising each in turn leaves one running. Re-dock the
# six side by side along the bottom of the main window so every one is on screen at
# once. Not floating: each floating dock is its own top-level window with its own
# swapchain, and presenting seven of them on the GUI thread is a cost no user layout has.
FLOAT = """
from PySide6.QtCore import Qt
for panel in window.analyzers:
    window.removeDockWidget(panel)
    window.addDockWidget(Qt.DockWidgetArea.BottomDockWidgetArea, panel, Qt.Orientation.Horizontal)
    panel.show()
window.resizeDocks(list(window.analyzers), [1] * len(window.analyzers), Qt.Orientation.Horizontal)
result = True
"""
ANALYZERS = 6

HOOK = """
import time
clock = window.clock
work, stamps = [], []
original = clock.tick
def timed():
    began = time.perf_counter()
    original()
    done = time.perf_counter()
    work.append(done - began)
    stamps.append(began)
clock._timer.timeout.disconnect()
clock._timer.timeout.connect(timed)
window._bench = (work, stamps)
result = True
"""

READ = """
import numpy as np
work, stamps = window._bench
w = np.array(work) * 1000
i = np.diff(np.array(stamps)) * 1000
result = dict(frames=len(w), p50_ms=float(np.percentile(w, 50)), p99_ms=float(np.percentile(w, 99)),
              worst_ms=float(w.max()), interval_p99_ms=float(np.percentile(i, 99)),
              panels_open=sorted(p.objectName() for p in window.panels if p.isVisible()))
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seconds", type=float, default=60.0)
    args = parser.parse_args()
    project = make_project.build(ROOT / "build" / "bench")
    with Driver.launch(project, via="adX.cmd", timeout=180) as app:
        app.wait_for("project.open", timeout=120)
        for panel in PANELS:
            app.trigger(f"view.{panel}")
        app.script(FLOAT)
        deadline = time.monotonic() + 10
        while len(app.state()["analyzers"]) < ANALYZERS and time.monotonic() < deadline:
            time.sleep(0.2)
        app.script(HOOK)
        app.trigger("transport.play")
        app.wait_for("transport.playing", timeout=10)
        time.sleep(args.seconds)
        stats = dict(app.script(READ))
        stats["analyzers"] = app.state()["analyzers"]
    stats["ok"] = (
        len(stats["analyzers"]) == ANALYZERS
        and stats["p99_ms"] < BUDGET_MS
        and stats["interval_p99_ms"] < BUDGET_MS * 1.5
    )
    print(json.dumps(stats))
    return 0 if stats["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
