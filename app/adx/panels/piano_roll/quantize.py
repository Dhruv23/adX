"""Quantize and humanize: the logic, and the dialogs that ask for it (phase_5.md 4.7).

Both work on a whole selection as one numpy array and produce one EditNotes command,
so a quantize over 10,000 notes is one undo step and never a Python loop per note.

Humanize is **seeded**: an unseeded humanize makes a project unreproducible, which
breaks golden renders. The generator is numpy's PCG64, whose output for a seed is the
same in every process and on every platform.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFormLayout,
    QSpinBox,
    QWidget,
)

from adx.engine_bridge import PPQ, NoteArray

#: Grid choices, as (label, ticks).
GRIDS: tuple[tuple[str, int], ...] = (
    ("1/1", PPQ * 4),
    ("1/2", PPQ * 2),
    ("1/4", PPQ),
    ("1/8", PPQ // 2),
    ("1/8T", PPQ // 3),
    ("1/16", PPQ // 4),
    ("1/16T", PPQ // 6),
    ("1/32", PPQ // 8),
    ("1/64", PPQ // 16),
)


@dataclass(frozen=True, slots=True)
class QuantizeSettings:
    """What a quantize does."""

    grid: int = PPQ // 4
    #: 0 leaves notes alone, 1 puts them exactly on the grid.
    strength: float = 1.0
    #: 0 is straight; 1 delays every off-beat grid line by half a grid step.
    swing: float = 0.0
    #: Quantize note ends as well as starts.
    ends: bool = False


@dataclass(frozen=True, slots=True)
class HumanizeSettings:
    """What a humanize does."""

    seed: int = 1
    #: Largest start offset, in ticks either way.
    timing: int = PPQ // 32
    #: Largest velocity change either way.
    velocity: int = 10


def _round_half_up(values: np.ndarray) -> np.ndarray:
    return np.floor(values + 0.5).astype(np.int64)


def _grid_targets(ticks: np.ndarray, grid: int, swing: float) -> np.ndarray:
    steps = _round_half_up(ticks / grid)
    targets = steps * grid
    if swing > 0.0:
        targets = targets + np.where(
            steps % 2 == 1, _round_half_up(np.full(steps.shape, swing * grid / 2)), 0
        )
    return targets.astype(np.int64)


def quantize(notes: NoteArray, settings: QuantizeSettings) -> NoteArray:
    """A copy of ``notes`` with starts (and optionally ends) pulled toward the grid.

    Strength 0 is a no-op; 1.0 is exact; 0.5 moves each note exactly halfway, in ticks.
    """
    out = notes.copy()
    if len(out) == 0 or settings.grid <= 0 or settings.strength <= 0.0:
        return out
    starts = out["start"].astype(np.int64)
    ends = starts + out["length"].astype(np.int64)
    strength = min(1.0, settings.strength)
    new_starts = starts + _round_half_up(
        (_grid_targets(starts, settings.grid, settings.swing) - starts) * strength
    )
    if settings.ends:
        new_ends = ends + _round_half_up(
            (_grid_targets(ends, settings.grid, settings.swing) - ends) * strength
        )
        # An end that snaps onto (or before) its start becomes one grid step long.
        lengths = np.where(new_ends > new_starts, new_ends - new_starts, settings.grid)
    else:
        lengths = out["length"].astype(np.int64)
    out["start"] = np.maximum(new_starts, 0)
    out["length"] = np.maximum(lengths, 1)
    return out


def humanize(notes: NoteArray, settings: HumanizeSettings) -> NoteArray:
    """A copy of ``notes`` with seeded timing and velocity jitter."""
    out = notes.copy()
    if len(out) == 0:
        return out
    rng = np.random.default_rng(settings.seed)
    # Order-independent: jitter is drawn in note-id order, so the same selection gets
    # the same result whatever order the array happens to be in.
    order = np.argsort(out["id"], kind="stable")
    timing = rng.integers(-settings.timing, settings.timing + 1, size=len(out))
    velocity = rng.integers(-settings.velocity, settings.velocity + 1, size=len(out))
    starts = out["start"].astype(np.int64)
    starts[order] = np.maximum(starts[order] + timing, 0)
    out["start"] = starts
    velocities = out["velocity"].astype(np.int64)
    velocities[order] = np.clip(velocities[order] + velocity, 1, 127)
    out["velocity"] = velocities.astype(np.uint8)
    return out


def grid_index(ticks: int) -> int:
    """The GRIDS entry closest to ``ticks``."""
    return min(range(len(GRIDS)), key=lambda i: abs(GRIDS[i][1] - ticks))


class QuantizeDialog(QDialog):
    """Asks for :class:`QuantizeSettings`."""

    def __init__(self, grid: int, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Quantize")
        form = QFormLayout(self)
        self._grid = QComboBox()
        for label, _ticks in GRIDS:
            self._grid.addItem(label)
        self._grid.setCurrentIndex(grid_index(grid))
        self._strength = QSpinBox()
        self._strength.setRange(0, 100)
        self._strength.setSuffix(" %")
        self._strength.setValue(100)
        self._swing = QSpinBox()
        self._swing.setRange(0, 100)
        self._swing.setSuffix(" %")
        self._ends = QCheckBox("Quantize note ends")
        form.addRow("Grid", self._grid)
        form.addRow("Strength", self._strength)
        form.addRow("Swing", self._swing)
        form.addRow("", self._ends)
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)

    def settings(self) -> QuantizeSettings:
        """What the dialog says."""
        return QuantizeSettings(
            grid=GRIDS[self._grid.currentIndex()][1],
            strength=self._strength.value() / 100.0,
            swing=self._swing.value() / 100.0,
            ends=self._ends.isChecked(),
        )


class HumanizeDialog(QDialog):
    """Asks for :class:`HumanizeSettings`, seed included."""

    def __init__(self, seed: int, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Humanize")
        form = QFormLayout(self)
        self._seed = QSpinBox()
        self._seed.setRange(0, 2**31 - 1)
        self._seed.setValue(seed)
        self._timing = QDoubleSpinBox()
        self._timing.setRange(0.0, 1.0)
        self._timing.setSingleStep(0.01)
        self._timing.setValue(1 / 32)
        self._timing.setSuffix(" beat")
        self._velocity = QSpinBox()
        self._velocity.setRange(0, 64)
        self._velocity.setValue(10)
        form.addRow("Seed", self._seed)
        form.addRow("Timing (max)", self._timing)
        form.addRow("Velocity (max)", self._velocity)
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)

    def settings(self) -> HumanizeSettings:
        """What the dialog says."""
        return HumanizeSettings(
            seed=self._seed.value(),
            timing=round(self._timing.value() * PPQ),
            velocity=self._velocity.value(),
        )
