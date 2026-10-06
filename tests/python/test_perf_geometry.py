"""The blocking performance gate, Python side (phase_5.md 4.9).

The C++ budgets (build times, hitTestRect, steady-state storage) are
tests/cpp/geometry/bench_piano_roll.cpp. This file holds the parts only Python can
measure: the FFI round trip, and the two architectural assertions - FFI calls per
simulated pan frame (exactly 0) and per meter frame (exactly 1). Those are counts, so
they are deterministic and cannot flake.

Timing budgets are enforced on an optimised engine (the wheel pip builds is Release);
a debug engine reports only.
"""

from __future__ import annotations

import time

import numpy as np
import pytest

from adx import engine_bridge
from adx.engine_bridge import CALLS, Engine, FrameClock, PianoRollGeometry, Project
from conftest import dispose


def _best_ms(runs: int, body: object) -> float:
    best = float("inf")
    for _ in range(runs):
        start = time.perf_counter()
        body()  # type: ignore[operator]
        best = min(best, (time.perf_counter() - start) * 1000.0)
    return best


def test_round_trip_10k(ten_k_text: str) -> None:
    """Python -> C++ -> numpy for 10k notes: the clip as one array, and a geometry view."""
    project, _diagnostics = Project.loads(ten_k_text)
    geometry = PianoRollGeometry()
    viewport = engine_bridge.make_viewport(0.0, 128.0, 0.0065, 8.0, 0.0065 * 64 * 4 * 3840, 1024.0)
    geometry.build(project, viewport, "Roll", "Lead", np.zeros(0, dtype=np.uint32))

    def round_trip() -> None:
        notes = project.notes("Roll", "Lead")
        assert len(notes) == 10000
        with geometry.view(3) as view:
            assert view.vertex_count >= 6 * 5000

    ms = _best_ms(50, round_trip)
    print(f"perf_geometry: round trip, 10k notes {ms:.4f} ms (budget 0.50 ms)")
    assert ms < 0.5


def test_build_10k_through_the_bridge(ten_k_text: str) -> None:
    """The 2 ms C++ budget holds through the FFI too (GIL released, one call)."""
    project, _diagnostics = Project.loads(ten_k_text)
    geometry = PianoRollGeometry()
    viewport = engine_bridge.make_viewport(0.0, 128.0, 0.0065, 8.0, 0.0065 * 64 * 4 * 3840, 1024.0)
    selected = np.zeros(0, dtype=np.uint32)
    ms = _best_ms(30, lambda: geometry.build(project, viewport, "Roll", "Lead", selected))
    print(f"perf_geometry: build via bridge, 10k notes {ms:.4f} ms (budget 2.00 ms)")
    assert ms < 2.0


def test_meter_frame_is_one_call(qapp: object) -> None:
    """Every 60 Hz frame - playhead, transport state, every meter - is one engine call."""
    project, _diagnostics = Project.load("docs/examples/suffocation.adx")
    engine = Engine(null_backend=True)
    engine.set_project(project)
    engine.start()
    try:
        clock = FrameClock(engine)
        clock.tick()  # grows the level buffer once if it has to
        with CALLS.measure() as calls:
            for _ in range(100):
                clock.tick()
        assert calls[0] == 100
        assert clock.strips > 0
    finally:
        engine.stop()


@pytest.mark.usefixtures("quick")
def test_pan_frame_is_zero_calls(ten_k_text: str) -> None:
    """A pan inside the built range moves the transform and makes no engine call."""
    from adx.panels.piano_roll.panel import PianoRollPanel

    project, _diagnostics = Project.loads(ten_k_text)
    panel = PianoRollPanel()
    panel.resize(1400, 800)
    panel.on_resize(1200.0, 700.0)
    panel.set_project(project)
    panel.view.zoom_time(8.0, 0.0)  # into a few bars, so a pan has somewhere to go
    panel.apply_view()
    panel.rebuild(force=True)
    with CALLS.measure() as calls:
        for step in range(120):
            panel.view.pan(4.0 if step < 60 else -4.0, 0.5 if step % 2 else -0.5)
            panel.apply_view()
    assert calls[0] == 0
    # Zooming inside the 2x band is a transform too.
    with CALLS.measure() as calls:
        for _ in range(5):
            panel.view.zoom_time(1.05, 600.0)
            panel.apply_view()
    assert calls[0] == 0
    # Leaving the built range is the one thing that rebuilds.
    panel.view.pan(10_000.0, 0.0)
    with CALLS.measure() as calls:
        panel.apply_view()
    assert calls[0] > 0
    dispose(panel)


@pytest.mark.usefixtures("quick")
def test_selection_and_playhead_rebuild_nothing(ten_k_text: str) -> None:
    """A selection recolours in place; the playhead moves its own node."""
    from adx.panels.piano_roll.panel import PianoRollPanel

    project, _diagnostics = Project.loads(ten_k_text)
    panel = PianoRollPanel()
    panel.on_resize(1200.0, 700.0)
    panel.set_project(project)
    assert panel.model is not None
    built = panel.built
    panel.model.select(panel.model.notes()["id"][:500])
    with CALLS.measure() as calls:
        panel._selection_changed()
    assert calls[0] == 3  # update_selection + two leases (notes, lanes); no build
    assert panel.built is built
    with CALLS.measure() as calls:
        for tick in range(0, 3840 * 16, 480):
            panel.frame(tick, True)
    assert calls[0] == 0
    dispose(panel)
