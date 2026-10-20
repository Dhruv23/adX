"""Drives the mixer's meters on a real GPU and prints frame-time statistics as JSON
(phase_6.md 6: "Mixer holds 60 fps with 200 inserts metering", measured with the same
protocol as Phase 5's perf_frames_driver.py).

Run by test_perf_frames.py in its own process, on the platform's real display, or by
hand:

    python tests/python/perf_mixer_driver.py [frames]

The whole application, with 200 inserts in the mixer (the master and 199 more), a
channel playing a looped pattern into every one of them through sends, and the
transport rolling. Each step is one real UI frame - FrameClock.tick(): the one engine
call that reads every meter and builds all 200 strips' vertices, then every panel's
frame handler, the mixer's upload among them - issued as soon as the previous frame has
rendered. *Frame time* is that step plus the meter view's scene-graph sync and render;
its 99th percentile is the gate. *Interval* is rendered frame to rendered frame.
"""

from __future__ import annotations

import itertools
import json
import math
import os
import pathlib
import sys
import tempfile
import time

os.environ.pop("QT_QPA_PLATFORM", None)
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from PySide6.QtCore import QSettings, Qt, QTimer

from adx import engine_bridge
from adx.application import create
from adx.engine_bridge import CALLS, PPQ

INSERTS = 200


def build(window: object) -> None:
    """200 inserts, each fed by a send from the one playing channel."""
    from adx.mainwindow import MainWindow

    assert isinstance(window, MainWindow)
    session, project = window.session, window.session.require()
    notes = engine_bridge.empty_notes(16)
    notes["start"] = [i * PPQ // 4 for i in range(16)]
    notes["length"] = PPQ // 8
    notes["pitch"] = [48 + (i * 7) % 24 for i in range(16)]
    notes["velocity"] = 110
    with session.group("setup"):
        session.execute(
            engine_bridge.commands.edit_notes(
                "P1",
                "Lead",
                engine_bridge.empty_notes()["id"],
                engine_bridge.empty_notes(),
                notes,
                "notes",
            )
        )
        lead_insert = window.mixer.add_insert()
        session.build_and_execute("SetChannelOutput", 1, lead_insert)
        for _ in range(INSERTS - 2):
            target = window.mixer.add_insert()
            session.build_and_execute("AddSend", lead_insert, target, 0.5, False)
    assert len(project.mixer_info()["inserts"]) == INSERTS


def main(frames: int) -> int:
    settings = QSettings(
        str(pathlib.Path(tempfile.mkdtemp()) / "settings.ini"), QSettings.Format.IniFormat
    )
    app, window = create(null_audio=False, settings=settings)
    window.clock.stop()  # this driver issues the frames itself
    window.new_project()
    build(window)
    window.resize(1600, 1000)
    window.show()
    window.mixer.show()
    window.mixer.raise_()
    window.engine.set_loop(0, PPQ * 4, True)
    window.engine.play()

    view = window.mixer.meter_view
    quick_window = view.quickWindow()
    stamps: list[float] = []
    work: list[float] = []
    step_ms = [0.0]
    sync_start = [0.0]
    calls_before = CALLS.count
    peaks: list[float] = []
    warm_calls = [0]
    last_calls = [0]
    steps = [0, 0]  # all steps; steps when warm-up ended

    def step() -> None:
        steps[0] += 1
        began = time.perf_counter()
        window.clock.tick()  # one engine call; every panel's frame; the meter upload
        view.update()
        step_ms[0] = (time.perf_counter() - began) * 1000.0
        strips = window.clock.strips
        peaks.append(float(window.clock.levels[:strips, 0].max()) if strips else 0.0)

    def synchronizing() -> None:
        sync_start[0] = time.perf_counter()

    def rendered() -> None:
        now = time.perf_counter()
        stamps.append(now)
        if len(stamps) == 10:
            warm_calls[0] = CALLS.count
            steps[1] = steps[0]
        if sync_start[0]:
            work.append(step_ms[0] + (now - sync_start[0]) * 1000.0)
        if len(stamps) >= frames:
            last_calls[0] = CALLS.count
            steps.append(steps[0])
            window.saved_revision = window.session.require().revision()
            QTimer.singleShot(0, app.quit)
        else:
            QTimer.singleShot(0, step)

    quick_window.beforeSynchronizing.connect(synchronizing, Qt.ConnectionType.DirectConnection)
    quick_window.afterRendering.connect(rendered, Qt.ConnectionType.DirectConnection)
    QTimer.singleShot(800, step)
    QTimer.singleShot(120_000, app.quit)  # a hung renderer is a failure, not a hang
    app.exec()
    api = str(quick_window.rendererInterface().graphicsApi()).split(".")[-1]
    strips = window.clock.strips
    calls = last_calls[0] - warm_calls[0]
    del calls_before
    window.engine.stop_playback()
    window.engine.stop()
    window.saved_revision = window.session.require().revision()

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
                "inserts": INSERTS,
                "strips_metered": strips,
                "frames": len(intervals),
                "p50_ms": round(percentile(costs, 0.50), 3),
                "p95_ms": round(percentile(costs, 0.95), 3),
                "p99_ms": round(percentile(costs, 0.99), 3),
                "max_ms": round(costs[-1], 3),
                "interval_p50_ms": round(percentile(intervals, 0.50), 3),
                "interval_p99_ms": round(percentile(intervals, 0.99), 3),
                "interval_max_ms": round(intervals[-1], 3),
                "mean_fps": round(1000.0 / (sum(intervals) / len(intervals)), 1),
                # Engine calls per UI frame step, after the 10 warm-up frames.
                "engine_calls_per_step": round(calls / max(1, steps[2] - steps[1]), 3),
                "steps": steps[2] - steps[1],
                "meters_moved": max(peaks, default=0.0) > 0.0,
            }
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1]) if len(sys.argv) > 1 else 1200))
