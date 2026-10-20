"""The one global selection (phase_6.md 4.7).

A typed set of ids - notes, clips, automation points, channels, inserts or slots - and
the context that makes the ids meaningful (which pattern and channel a note is in,
which lane a point is on). Exactly one kind is selected at a time: selecting clips in
the playlist deselects the notes in the roll, as in every DAW.

Delete, copy, paste and duplicate are written once, here, against the current
selection, and every panel's menu and shortcut calls them - they are not reimplemented
five times (§4.7). Each ends in commands, through the session, so each is one undo step.
The inspector renders whatever is selected.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

from PySide6.QtCore import QObject, Signal


class Kind(Enum):
    """What the selection holds."""

    NONE = "none"
    NOTES = "notes"
    CLIPS = "clips"
    POINTS = "points"
    CHANNELS = "channels"
    INSERTS = "inserts"
    SLOTS = "slots"


#: Every kind a selection can hold, for table-driven tests.
SELECTABLE: tuple[Kind, ...] = (
    Kind.NOTES,
    Kind.CLIPS,
    Kind.POINTS,
    Kind.CHANNELS,
    Kind.INSERTS,
    Kind.SLOTS,
)


@dataclass(frozen=True, slots=True)
class Context:
    """Where the selected ids live. Unused fields are empty.

    - notes:   ``pattern`` and ``channel`` (names: the piano roll addresses by name)
    - clips:   nothing - item ids are project-wide; ``track`` per id is in ``tracks``
    - points:  ``clip`` (an automation clip id)
    - slots:   ``insert``
    """

    pattern: str = ""
    channel: str = ""
    clip: int = 0
    insert: int = 0
    tracks: tuple[int, ...] = ()


@dataclass(frozen=True, slots=True)
class Selected:
    """A snapshot of the selection: kind, ids, context."""

    kind: Kind = Kind.NONE
    ids: tuple[int, ...] = ()
    context: Context = field(default_factory=Context)

    def __bool__(self) -> bool:
        return self.kind is not Kind.NONE and bool(self.ids)


class Selection(QObject):
    """The current selection, with a change signal."""

    changed = Signal()

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._current = Selected()

    @property
    def current(self) -> Selected:
        """What is selected now."""
        return self._current

    @property
    def kind(self) -> Kind:
        """The kind selected, NONE when nothing is."""
        return self._current.kind if self._current else Kind.NONE

    def ids(self, kind: Kind) -> tuple[int, ...]:
        """The selected ids of ``kind``; empty when another kind is selected."""
        return self._current.ids if self._current.kind is kind else ()

    def select(
        self, kind: Kind, ids: list[int] | tuple[int, ...], context: Context | None = None
    ) -> None:
        """Replace the selection."""
        unique = tuple(sorted({int(i) for i in ids}))
        chosen = Selected(kind, unique, context or Context()) if unique else Selected()
        if chosen != self._current:
            self._current = chosen
            self.changed.emit()

    def clear(self) -> None:
        """Select nothing."""
        self.select(Kind.NONE, ())

    def prune(self, existing: set[int]) -> None:
        """Drop ids that no longer exist (after an undo removed them)."""
        if not self._current:
            return
        kept = tuple(i for i in self._current.ids if i in existing)
        if kept != self._current.ids:
            self.select(self._current.kind, kept, self._current.context)
