"""The zero-copy contract (phase_5.md 4.1, 5): views share C++ storage, leases pin it."""

from __future__ import annotations

import numpy as np
import pytest

from adx import engine_bridge
from adx.engine_bridge import NOTE_DTYPE, PianoRollGeometry, Project

NOTES = 3


def _built(text: str) -> tuple[Project, PianoRollGeometry]:
    project, _diagnostics = Project.loads(text)
    geometry = PianoRollGeometry()
    viewport = engine_bridge.make_viewport(0.0, 128.0, 0.01, 8.0, 3000.0, 1024.0)
    geometry.build(project, viewport, "Roll", "Lead", np.zeros(0, dtype=np.uint32))
    return project, geometry


def test_geometry_zero_copy(ten_k_text: str) -> None:
    """np.asarray shares memory with the C++ buffer: mutate C++, read it in Python."""
    _project, geometry = _built(ten_k_text)
    raw = geometry.native_buffer(NOTES)
    view = np.asarray(raw)  # the buffer protocol itself, no lease: for this test only
    assert view.dtype == np.float32
    assert view.shape == (raw.vertex_count, 3)
    assert view.shape[0] > 6 * 1000

    with geometry.view(NOTES) as leased:
        assert leased.array.ctypes.data == leased.address  # the same storage, no copy
        assert leased.array.ctypes.data == view.ctypes.data
        assert not leased.array.flags.writeable
        before = leased.array[:, 2].copy()

    # A selection change patches colours in C++, in place; the Python view sees it.
    ids = geometry.visible_ids()[:10]
    geometry.update_selection(np.sort(ids))
    after = view[:, 2]
    assert (after != before).sum() == 10 * 6
    with geometry.view(NOTES) as again:
        assert again.address == view.ctypes.data  # the storage did not move


def test_geometry_lease_prevents_resize(ten_k_text: str) -> None:
    """resize() inside a lease raises instead of reallocating under the view."""
    project, geometry = _built(ten_k_text)
    viewport = engine_bridge.make_viewport(0.0, 128.0, 0.5, 8.0, 3000.0, 1024.0)
    selected = np.sort(geometry.visible_ids()[:5])
    with geometry.view(NOTES) as leased:
        address = leased.address
        with pytest.raises(RuntimeError, match=r"(?i)lease"):
            geometry.update_selection(selected)
        with pytest.raises(RuntimeError, match=r"(?i)lease"):
            geometry.build(project, viewport, "Roll", "Lead", np.zeros(0, dtype=np.uint32))
        assert leased.address == address
    # Out of the block, the same call succeeds.
    geometry.build(project, viewport, "Roll", "Lead", np.zeros(0, dtype=np.uint32))


def test_view_does_not_outlive_its_block(ten_k_text: str) -> None:
    """A lease's view is refused once its block has ended."""
    _project, geometry = _built(ten_k_text)
    lease = geometry.native_buffer(NOTES).lease()
    with lease:
        assert np.asarray(lease).shape[1] == 3
    # Outside the block the buffer protocol refuses. (np.asarray would not raise: it
    # falls back to wrapping the object, which is no view of the storage either.)
    with pytest.raises(BufferError):
        memoryview(lease)
    assert np.asarray(lease).dtype == object


def test_notes_are_one_array(ten_k_text: str) -> None:
    """A clip crosses as ONE structured array, never a list of notes (Rule 2)."""
    project, _geometry = _built(ten_k_text)
    notes = project.notes("Roll", "Lead")
    assert isinstance(notes, np.ndarray)
    assert notes.dtype == NOTE_DTYPE
    assert len(notes) == 10000
    assert set(NOTE_DTYPE.names or ()) >= {"id", "start", "length", "pitch", "velocity", "muted"}
    assert notes["length"].min() > 0
