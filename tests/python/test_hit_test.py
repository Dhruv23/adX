"""Hit testing through the bridge agrees with a numpy reference (phase_5.md 4.6)."""

from __future__ import annotations

import numpy as np

from adx import engine_bridge
from adx.engine_bridge import Project

TICK0, TOP, PPT, PPS, W, H = 4.0 * 3840, 100.0, 0.08, 9.0, 1500.0, 600.0


def _project(text: str) -> tuple[Project, np.ndarray]:
    project, _diagnostics = Project.loads(text)
    return project, project.notes("Roll", "Lead")


def test_hit_test_note_matches_numpy(ten_k_text: str) -> None:
    project, notes = _project(ten_k_text)
    viewport = engine_bridge.make_viewport(TICK0, TOP, PPT, PPS, W, H)
    rng = np.random.default_rng(3)
    starts = notes["start"].astype(np.float64)
    ends = starts + notes["length"]
    hits = 0
    for x, y in rng.uniform((0, 0), (W, H), size=(1000, 2)):
        tick = TICK0 + x / PPT
        row = int(np.floor(TOP - y / PPS))
        under = np.nonzero((notes["pitch"] == row) & (starts <= tick) & (tick < ends))[0]
        expected = int(notes["id"][under[-1]]) if len(under) else 0
        got, part = engine_bridge.hit_test_note(
            project, "Roll", "Lead", viewport, float(x), float(y)
        )
        assert got == expected
        assert (part == engine_bridge.PART_NONE) == (got == 0)
        hits += bool(got)
    assert hits > 20


def test_hit_test_rect_matches_numpy(ten_k_text: str) -> None:
    project, notes = _project(ten_k_text)
    viewport = engine_bridge.make_viewport(TICK0, TOP, PPT, PPS, W, H)
    rng = np.random.default_rng(4)
    starts = notes["start"].astype(np.float64)
    ends = starts + notes["length"]
    pitches = notes["pitch"].astype(np.float64)
    for x0, y0, x1, y1 in rng.uniform((0, 0, 0, 0), (W, H, W, H), size=(200, 4)):
        t0, t1 = TICK0 + min(x0, x1) / PPT, TICK0 + max(x0, x1) / PPT
        high, low = TOP - min(y0, y1) / PPS, TOP - max(y0, y1) / PPS
        mask = (pitches + 1 > low) & (pitches < high) & (ends > t0) & (starts <= t1)
        got = engine_bridge.hit_test_rect(project, "Roll", "Lead", viewport, (x0, y0, x1, y1))
        assert got.dtype == np.uint32
        assert set(got.tolist()) == set(notes["id"][mask].tolist())


def test_hit_test_lane_reports_value(ten_k_text: str) -> None:
    project, notes = _project(ten_k_text)
    viewport = engine_bridge.make_viewport(0.0, TOP, PPT, PPS, W, H)
    first = notes[np.argmin(notes["start"])]
    x = (float(first["start"]) - 0.0) * PPT + 1.0
    note, value = engine_bridge.hit_test_lane(project, "Roll", "Lead", viewport, 100.0, x, 25.0)
    assert note != 0
    assert abs(value - 0.75) < 1e-6
