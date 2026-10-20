"""The open project as the panels share it: one place every edit goes through.

Phase 6's discipline (phase_6.md 3) rests on three facts this class makes true:

- **every mutation is a command** - :meth:`Session.execute` is the only way a panel
  edits, and :meth:`Session.group` makes a multi-command gesture one undo step;
- **no panel owns project state** - after every edit, undo or redo the session emits
  :attr:`Session.changed` once, and every panel re-reads what it shows from the engine
  in its ``refresh()``. A panel that kept a copy would be a panel that disagreed;
- **the audio thread keeps up** - :meth:`Session.commit` hands the edit to the engine
  incrementally, once per change.

Knob turns are the one exception to "execute a command here": :meth:`set_param` sends
the value straight to the audio thread *and* records it as a command (coalesced, so a
drag is one step) - the engine does both, so they cannot drift.
"""

from __future__ import annotations

from collections.abc import Callable, Iterator
from contextlib import contextmanager

from PySide6.QtCore import QObject, Signal

from adx import engine_bridge
from adx.engine_bridge import Engine, Native, Project


class Session(QObject):
    """A project, its engine, and the change signal every panel listens to."""

    #: After every edit, undo, redo or load. Panels refresh from the engine.
    changed = Signal()
    #: A project was opened or created (panels reset their view state).
    opened = Signal()
    #: A command could not be built or executed: (what, why).
    failed = Signal(str, str)

    def __init__(self, engine: Engine, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self.engine = engine
        self.project: Project | None = None
        self._group_depth = 0
        self._gestures = 0
        self._pending = False
        #: Called after every change, before the signal (the main window's title).
        self.after_change: Callable[[], None] | None = None

    # --- the project ----------------------------------------------------------------

    def open(self, project: Project) -> None:
        """Make ``project`` the one everything edits and plays."""
        self.project = project
        self.engine.set_project(project)
        self.opened.emit()
        self.changed.emit()

    def require(self) -> Project:
        """The open project, or an error naming the problem."""
        if self.project is None:
            raise RuntimeError("no project is open")
        return self.project

    # --- editing --------------------------------------------------------------------

    def execute(self, command: Native) -> bool:
        """Execute one command; one undo step unless inside :meth:`group`. False (and
        :attr:`failed`) when the engine refuses it."""
        project = self.require()
        try:
            project.execute(command)
        except (ValueError, RuntimeError) as error:
            self.failed.emit("edit", str(error))
            return False
        self._changed()
        return True

    def build_and_execute(self, name: str, *args: object) -> bool:
        """``execute(commands.build(name, *args))``, reporting a bad argument instead of
        raising into a Qt slot."""
        try:
            command = engine_bridge.commands.build(name, *args)
        except (ValueError, TypeError, AttributeError) as error:
            self.failed.emit(name, str(error))
            return False
        return self.execute(command)

    @contextmanager
    def group(self, label: str) -> Iterator[None]:
        """Everything executed inside is one undo step, and one change signal."""
        project = self.require()
        project.begin_group(label)
        self._group_depth += 1
        try:
            yield
        finally:
            self._group_depth -= 1
            project.end_group()
            if self._group_depth == 0 and self._pending:
                self._pending = False
                self._changed()

    def begin_gesture(self, label: str) -> None:
        """Open a group a press starts and its release ends (:meth:`end_gesture`). Unlike
        :meth:`group`, panels still refresh after each command inside it - a painted run of
        clips appears as it is painted - but the whole gesture is one undo step."""
        self.require().begin_group(label)
        self._gestures += 1

    def end_gesture(self) -> None:
        """Close the group :meth:`begin_gesture` opened."""
        if self._gestures == 0 or self.project is None:
            return
        self._gestures -= 1
        self.project.end_group()
        self._changed()

    def seal(self) -> None:
        """A new gesture began (a press): its first command starts a new history entry,
        however soon after the last gesture it comes. Coalescing still merges the commands
        *within* the gesture - a drag stays one step."""
        if self.project is not None and not self._gestures and not self._group_depth:
            self.project.seal_coalescing()

    @property
    def in_gesture(self) -> bool:
        """Whether a gesture's group is open."""
        return self._gestures > 0

    def call(self, method: Callable[..., object], *args: object) -> object:
        """Run a Project method that edits (a drop, a preset) and announce the change."""
        try:
            result = method(*args)
        except (ValueError, RuntimeError) as error:
            self.failed.emit(getattr(method, "__name__", "edit"), str(error))
            return None
        self._changed()
        return result

    def set_param(self, path: str, value: float) -> bool:
        """A knob turn: heard next block, recorded as one coalesced command."""
        project = self.require()
        try:
            done = self.engine.set_param(project, path, value)
        except ValueError as error:
            self.failed.emit(path, str(error))
            return False
        self._changed(commit=False)
        return done

    def undo(self) -> bool:
        """Undo one step (never in the middle of a gesture)."""
        if self.project is None or self._gestures or not self.project.undo():
            return False
        self._changed()
        return True

    def redo(self) -> bool:
        """Redo one step."""
        if self.project is None or not self.project.redo():
            return False
        self._changed()
        return True

    def undo_to(self, depth: int) -> bool:
        """Jump through history so ``depth`` steps are done (the history panel)."""
        if self.project is None or not self.project.undo_to(depth):
            return False
        self._changed()
        return True

    def notify(self) -> None:
        """A command was executed on the project directly (the piano roll's model does,
        through RollModel.commit): catch the engine up and tell every panel."""
        self._changed()

    # --- after every change ---------------------------------------------------------

    def commit(self) -> None:
        """Bring the audio thread up to the project, incrementally."""
        if self.project is None:
            return
        try:
            self.engine.commit(self.project)
        except RuntimeError as error:
            # A routing cycle: the last good snapshot keeps playing.
            self.failed.emit("render", str(error))

    def _changed(self, commit: bool = True) -> None:
        if self._group_depth > 0:
            self._pending = True
            return
        if commit:
            self.commit()
        if self.after_change is not None:
            self.after_change()
        self.changed.emit()
