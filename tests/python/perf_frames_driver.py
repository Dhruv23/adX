"""Drives the piano roll on a real GPU and prints frame-time statistics as JSON.

Run by test_perf_frames.py in its own process, on the platform's real display (not the
offscreen platform the rest of the suite uses), or by hand:

    python tests/python/perf_frames_driver.py [frames]

It shows the piano roll over a 10,000-note pattern and, frame after frame, pans it
(and every 40 frames zooms it in or out by 1.6x, which crosses rebuild boundaries the
way a user does). Each step is issued as soon as the previous frame has rendered, so
the interval between consecutive rendered frames is what one frame of interaction
costs end to end: Python's handling of the input, the transform, any rebuild, and Qt's
render. Two numbers come out of each frame. The *frame time* is the work: the Python
step (input handling, transform, any rebuild and upload) plus the scene graph's own
sync and render (beforeSynchronizing to afterRendering) - everything but the wait for
the next vertical sync. Its 99th percentile is the gate (phase_5.md 4.9). The *interval*
between frames is what the
display shows: vsync-locked, it cannot go below the refresh period (16.67 ms at 60 Hz,
which is why it is not the gate), and a frame that missed its vsync shows as ~33 ms.
"""

from __future__ import annotations

import itertools
import json
import math
import os
import pathlib
import sys
import time

os.environ.pop("QT_QPA_PLATFORM", None)
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import QApplication, QMainWindow

from adx import engine_bridge
from adx.engine_bridge import CALLS, Project
from conftest import roll_text


def main(frames: int) -> int:
    # conftest (imported for roll_text) defaults the suite to the offscreen platform;
    # this measures the real one.
    os.environ.pop("QT_QPA_PLATFORM", None)
    app = QApplication(sys.argv[:1])
    engine_bridge.load_quick_plugin()
    from adx.panels.piano_roll.panel import PianoRollPanel

    project, _diagnostics = Project.loads(roll_text(10000, bars=64, seed=1, ghosts=1000))
    window = QMainWindow()
    panel = PianoRollPanel(window)
    window.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, panel)
    window.resize(1600, 1000)
    window.show()
    panel.set_project(project)
    panel.view.zoom_time(4.0, 0.0)
    panel.apply_view()

    quick_window = panel.quick.quickWindow()
    stamps: list[float] = []
    work: list[float] = []
    step_ms = [0.0]
    sync_start = [0.0]
    calls_before = CALLS.count
    rebuilds = [0]
    original = panel.rebuild

    def counting_rebuild(force: bool = False) -> None:
        before = panel.built
        original(force)
        rebuilds[0] += panel.built is not before

    panel.rebuild = counting_rebuild  # type: ignore[method-assign]

    def step() -> None:
        began = time.perf_counter()
        index = len(stamps)
        panel.view.pan(9.0 * math.sin(index / 25.0) + 6.0, 1.5 * math.cos(index / 17.0))
        if index % 40 == 39:
            panel.view.zoom_time(1.6 if (index // 40) % 2 else 1 / 1.6, 800.0)
        panel.apply_view()
        panel.quick.update()
        step_ms[0] = (time.perf_counter() - began) * 1000.0

    def synchronizing() -> None:
        sync_start[0] = time.perf_counter()

    def rendered() -> None:
        now = time.perf_counter()
        stamps.append(now)
        if sync_start[0]:
            work.append(step_ms[0] + (now - sync_start[0]) * 1000.0)
        if len(stamps) >= frames:
            QTimer.singleShot(0, app.quit)
        else:
            QTimer.singleShot(0, step)

    quick_window.beforeSynchronizing.connect(synchronizing, Qt.ConnectionType.DirectConnection)
    quick_window.afterRendering.connect(rendered, Qt.ConnectionType.DirectConnection)
    QTimer.singleShot(500, step)
    QTimer.singleShot(120_000, app.quit)  # a hung renderer is a failure, not a hang
    app.exec()
    # Read once the scene graph exists: before the first frame it reports no backend.
    api = str(quick_window.rendererInterface().graphicsApi()).split(".")[-1]

    intervals = sorted((b - a) * 1000.0 for a, b in itertools.pairwise(stamps[10:]))
    costs = sorted(work[10:])
    if not intervals or not costs:
        print(json.dumps({"api": api, "frames": 0}))
        return 1

    def percentile(values: list[float], p: float) -> float:
        return values[min(len(values) - 1, math.ceil(p * len(values)) - 1)]

    print(
        json.dumps(
            {
                "api": api,
                "frames": len(intervals),
                # Frame time: the Python step plus the scene graph's sync and render -
                # the work one frame of interaction costs, without the vsync wait.
                "p50_ms": round(percentile(costs, 0.50), 3),
                "p95_ms": round(percentile(costs, 0.95), 3),
                "p99_ms": round(percentile(costs, 0.99), 3),
                "max_ms": round(costs[-1], 3),
                # Frame interval: rendered frame to rendered frame. With vsync on it
                # cannot go below the display's refresh period; a dropped frame is ~2x.
                "interval_p50_ms": round(percentile(intervals, 0.50), 3),
                "interval_p99_ms": round(percentile(intervals, 0.99), 3),
                "interval_max_ms": round(intervals[-1], 3),
                "mean_fps": round(1000.0 / (sum(intervals) / len(intervals)), 1),
                "rebuilds": rebuilds[0],
                "engine_calls": CALLS.count - calls_before,
            }
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1]) if len(sys.argv) > 1 else 1200))
