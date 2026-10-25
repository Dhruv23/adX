"""The open project's file, kept in step with the model (phase_7.md 3, 4.1, 4.4).

Three sync directions meet here and in the editor panel; all of them end in commands
(engine/sync/TextSync.h):

- **model -> text**: in Live mode, every edit is written to the file
  :data:`~adx.engine_bridge.LIVE_WRITE_DEBOUNCE_MS` after the last one; otherwise on
  save. adX's own writes are recognised when the watcher hears them, and ignored.
- **text (external) -> model**: the watcher fires, the whole file is re-read and
  patched into the model as one undo step. A file that does not parse is shown, not
  applied. A change that would discard model edits the file has not seen raises
  :attr:`FileSync.conflict` - the window asks: keep mine, take theirs, or show the diff.
- **text (in-app) -> model** is the editor panel's (``panels/editor``).

Everything runs on the main thread, from Qt timers.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

from PySide6.QtCore import QObject, QTimer, Signal

from adx.engine_bridge import (
    LIVE_WRITE_DEBOUNCE_MS,
    WATCHER_DEBOUNCE_MS,
    FileWatcher,
    Project,
    TextSync,
)
from adx.session import Session

#: Sync modes, as the preferences and the engine spell them.
MODES = ("off", "on-save", "live")


def write_atomically(path: str, text: str) -> None:
    """Write ``text`` to ``path`` the way careful editors do: a temporary file in the same
    directory, then a rename over the target - so another program never reads half a
    file."""
    target = Path(path)
    handle, temporary = tempfile.mkstemp(prefix=f".{target.name}.", dir=str(target.parent))
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="") as stream:
            stream.write(text)
        os.replace(temporary, target)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


class FileSync(QObject):
    """One project's file: watched, written in Live mode, reloaded when it changes."""

    #: An external change was patched in: (commands, structural).
    reloaded = Signal(int, bool)
    #: The file changed while the model had edits it does not contain: its new text.
    conflict = Signal(str)
    #: The file changed but does not parse: (first error's line, message).
    invalid = Signal(int, str)
    #: adX wrote the file (Live mode, or a conflict resolved as "keep mine").
    wrote = Signal()

    def __init__(self, session: Session, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self.session = session
        self.sync: TextSync | None = None
        self.watcher: FileWatcher | None = None
        self.path: str | None = None
        self.mode = "on-save"
        #: The watcher's quiet window (tests shorten it).
        self.debounce_ms = WATCHER_DEBOUNCE_MS
        #: The conflicting text, while the user decides.
        self.pending_conflict: str | None = None
        self._poll = QTimer(self)
        self._poll.setInterval(50)
        self._poll.timeout.connect(self.poll)
        self._live = QTimer(self)
        self._live.setSingleShot(True)
        self._live.setInterval(LIVE_WRITE_DEBOUNCE_MS)
        self._live.timeout.connect(self.write_live)
        session.changed.connect(self._model_changed)

    # --- attaching --------------------------------------------------------------------

    def attach(self, project: Project, path: str | None, text: str | None = None) -> None:
        """A project was opened (``path`` None: not saved yet). ``text`` is what the file
        held when it was loaded."""
        base = str(Path(path).parent) if path else ""
        self.sync = TextSync(project, base)
        self.sync.set_mode(self.mode)
        self.sync.mark_synced(text if text is not None else self.sync.render_text())
        self.pending_conflict = None
        self._watch(path)

    def _watch(self, path: str | None) -> None:
        self.path = path
        self.watcher = None
        self._poll.stop()
        if path and self.mode != "off":
            self.watcher = FileWatcher(path, self.debounce_ms)
            self._poll.start()

    def set_mode(self, mode: str) -> None:
        """``off``, ``on-save`` or ``live``."""
        if mode not in MODES:
            raise ValueError(f"sync mode is one of {', '.join(MODES)}")
        self.mode = mode
        if self.sync is not None:
            self.sync.set_mode(mode)
        self._watch(self.path)
        if mode == "live":
            self._model_changed()

    def saved(self, path: str, text: str) -> None:
        """The window saved ``text`` to ``path`` (perhaps a new path)."""
        if self.sync is None:
            return
        self.sync.note_write(text)
        self.sync.mark_synced(text)
        if path != self.path:
            self._watch(path)

    # --- model -> text ----------------------------------------------------------------

    def _model_changed(self) -> None:
        if self.mode == "live" and self.sync is not None and self.path:
            self._live.start()

    def write_live(self) -> bool:
        """Write the model to the file now if it has edits the file has not seen."""
        if self.sync is None or not self.path or not self.sync.model_dirty():
            return False
        if self.pending_conflict is not None:
            return False  # never overwrite while the user is choosing
        # Never write over a change someone else saved since the two last agreed: that
        # is an external edit (or a conflict), and the watcher's event for it may simply
        # not have been read yet.
        on_disk = self._read()
        if on_disk is not None and on_disk != self.sync.synced_text():
            self.apply_external(on_disk)
            return False
        text = self.sync.render_text()
        self.sync.note_write(text)
        write_atomically(self.path, text)
        self.sync.mark_synced(text)
        self.wrote.emit()
        return True

    # --- text (external) -> model -----------------------------------------------------

    def poll(self) -> bool:
        """The watcher's tick: reload if the file changed. True when the model changed."""
        if self.watcher is None or self.sync is None or not self.watcher.poll():
            return False
        return self.reload_from_disk()

    def _read(self) -> str | None:
        """The file's text, or None when it cannot be read (mid-rename, not UTF-8)."""
        if not self.path:
            return None
        try:
            return Path(self.path).read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            return None

    def reload_from_disk(self) -> bool:
        """Re-read the whole file and apply it."""
        if self.sync is None:
            return False
        text = self._read()
        if text is None:
            return False  # the next event will bring it
        return self.apply_external(text)

    def apply_external(self, text: str) -> bool:
        """Apply text the file now holds. True when the model changed."""
        if self.sync is None:
            return False
        result = self.sync.apply_external(text)
        status = str(result["status"])
        if status == "applied":
            self.session.notify()
            self.reloaded.emit(int(result["commands"]), bool(result["structural"]))
            return True
        if status == "conflict":
            self.pending_conflict = text
            self.conflict.emit(text)
        elif status == "invalid":
            errors = [d for d in result["diagnostics"] if str(d["severity"]) == "error"]
            line = int(str(errors[0]["line"])) if errors else 0
            message = str(errors[0]["message"]) if errors else "does not parse"
            self.invalid.emit(line, message)
        return False

    # --- resolving a conflict ---------------------------------------------------------

    def keep_mine(self) -> None:
        """Write the model over the file."""
        self.pending_conflict = None
        if self.sync is None or not self.path:
            return
        text = self.sync.render_text()
        self.sync.note_write(text)
        write_atomically(self.path, text)
        self.sync.mark_synced(text)
        self.wrote.emit()

    def take_theirs(self) -> bool:
        """Apply the file's text over the model's unsaved edits (they stay one undo away)."""
        text, self.pending_conflict = self.pending_conflict, None
        if self.sync is None or text is None:
            return False
        result = self.sync.apply_forced(text)
        if str(result["status"]) == "applied":
            self.session.notify()
            self.reloaded.emit(int(result["commands"]), bool(result["structural"]))
            return True
        return False

    def model_dirty(self) -> bool:
        """The model has edits the file has not seen."""
        return self.sync is not None and self.sync.model_dirty()
