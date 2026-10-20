"""Autosave (phase_6.md 4.9): every few minutes a project with unsaved edits is written,
timestamped, to ``.adx-autosave/`` beside it - cheap and diffable, because the project is
text. An untitled project autosaves under the user's project folder. The newest ten are
kept.
"""

from __future__ import annotations

import time
from collections.abc import Callable
from pathlib import Path

from PySide6.QtCore import QObject, QTimer

from adx.session import Session

FOLDER = ".adx-autosave"
KEEP = 10


def autosave_path(project_path: str | None, fallback_dir: str, now: float | None = None) -> Path:
    """Where an autosave of ``project_path`` goes, stamped with ``now``."""
    stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(now if now is not None else time.time()))
    if project_path:
        source = Path(project_path)
        return source.parent / FOLDER / f"{source.stem}-{stamp}.adx"
    return Path(fallback_dir or ".") / FOLDER / f"untitled-{stamp}.adx"


class Autosave(QObject):
    """A timer that writes the project when it has changed since the last write."""

    def __init__(
        self,
        session: Session,
        path_of: Callable[[], str | None],
        fallback_dir: Callable[[], str],
        minutes: int = 5,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.path_of = path_of
        self.fallback_dir = fallback_dir
        self._written_revision = -1
        self.last_path: Path | None = None
        self._timer = QTimer(self)
        self._timer.timeout.connect(self.save_now)
        self.set_interval(minutes)

    def set_interval(self, minutes: int) -> None:
        """Every ``minutes`` (0 turns autosave off)."""
        if minutes <= 0:
            self._timer.stop()
            return
        self._timer.start(minutes * 60_000)

    def save_now(self) -> Path | None:
        """Write now if anything changed since the last autosave. Returns the file."""
        project = self.session.project
        if project is None:
            return None
        revision = project.revision()
        if revision == self._written_revision:
            return None
        target = autosave_path(self.path_of(), self.fallback_dir())
        target.parent.mkdir(parents=True, exist_ok=True)
        project.save(str(target))
        self._written_revision = revision
        self.last_path = target
        older = sorted(target.parent.glob(f"{target.stem.rsplit('-', 2)[0]}-*.adx"))
        for stale in older[:-KEEP]:
            stale.unlink(missing_ok=True)
        return target
