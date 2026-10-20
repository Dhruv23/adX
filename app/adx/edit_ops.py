"""Delete, copy, paste and duplicate - written once, for every kind of selection
(phase_6.md 4.7).

Every operation reads the global selection and ends in commands through the session, so
each is exactly one undo step whichever panel asked for it. The clipboard holds notes as
``.adx`` text written by the Phase 2 writer - copy notes in adX, paste them into a text
editor, edit, copy, paste back: it is the same format (``clipboard_is_adx_text``). The
other kinds are copied by reference: pasting them duplicates what was copied, if it still
exists.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from adx import engine_bridge
from adx.engine_bridge import PPQ, Project
from adx.selection import Context, Kind, Selection
from adx.session import Session

#: The clipboard's text marker for notes: a whole project fragment (it parses).
FRAGMENT_MARKER = "ADX_VERSION=2"


@dataclass(slots=True)
class Clipboard:
    """What was last copied: ``.adx`` text for notes, or (kind, ids) by reference."""

    text: str = ""
    kind: Kind = Kind.NONE
    ids: tuple[int, ...] = ()
    context: Context = field(default_factory=Context)


def _system_clipboard_set(text: str) -> None:
    from PySide6.QtWidgets import QApplication

    if QApplication.instance() is not None:
        QApplication.clipboard().setText(text)


def _system_clipboard_get() -> str:
    from PySide6.QtWidgets import QApplication

    if QApplication.instance() is None:
        return ""
    return str(QApplication.clipboard().text())


class EditOps:
    """The four operations over a session and its selection."""

    def __init__(self, session: Session, selection: Selection) -> None:
        self.session = session
        self.selection = selection
        self.clipboard = Clipboard()
        #: Where a paste lands, in ticks; panels set it from the cursor or playhead.
        self.paste_tick = 0

    # --- delete ---------------------------------------------------------------------

    def delete(self) -> bool:
        """Remove what is selected: one undo step."""
        current = self.selection.current
        if not current:
            return False
        project = self.session.require()
        with self.session.group(f"Delete {current.kind.value}"):
            self._delete(project, current.kind, current.ids, current.context)
        self.selection.clear()
        return True

    def _delete(self, project: Project, kind: Kind, ids: tuple[int, ...], context: Context) -> None:
        build = self.session.build_and_execute
        if kind is Kind.NOTES:
            self.session.execute(
                engine_bridge.commands.edit_notes(
                    context.pattern,
                    context.channel,
                    np.asarray(ids, dtype=np.uint32),
                    engine_bridge.empty_notes(),
                    engine_bridge.empty_notes(),
                    "Delete notes",
                )
            )
        elif kind is Kind.CLIPS:
            items = project.playlist_items()
            for item in ids:
                rows = items[items["item"] == item]
                if len(rows):
                    build("RemovePlaylistItem", int(rows[0]["track"]), item)
        elif kind is Kind.POINTS:
            points = project.automation_points(context.clip)
            keep = np.delete(points, [i for i in ids if 0 <= i < len(points)], axis=0)
            build("SetBreakpoints", context.clip, keep)
        elif kind is Kind.CHANNELS:
            for channel in ids:
                build("RemoveChannelId", channel)
        elif kind is Kind.INSERTS:
            master = int(project.mixer_info()["master"])
            for insert in ids:
                if insert != master:
                    build("RemoveInsert", insert)
        elif kind is Kind.SLOTS:
            for slot in ids:
                build("RemoveSlot", slot)

    # --- copy and paste -------------------------------------------------------------

    def copy(self) -> bool:
        """Copy what is selected. Notes go to the system clipboard as ``.adx`` text."""
        current = self.selection.current
        if not current:
            return False
        project = self.session.require()
        text = ""
        if current.kind is Kind.NOTES:
            text = project.notes_fragment(
                current.context.pattern, current.context.channel, list(current.ids)
            )
            _system_clipboard_set(text)
        elif current.kind is Kind.POINTS:
            points = project.automation_points(current.context.clip)
            chosen = points[[i for i in current.ids if 0 <= i < len(points)]]
            text = repr(chosen.tolist())
        self.clipboard = Clipboard(text, current.kind, current.ids, current.context)
        return True

    def paste(self, kind: Kind | None = None, context: Context | None = None) -> bool:
        """Paste into the current selection's place (or ``context``): one undo step."""
        target_context = context or self.selection.current.context
        system = _system_clipboard_get()
        if (kind in (None, Kind.NOTES)) and FRAGMENT_MARKER in system and target_context.pattern:
            return self._paste_notes(system, target_context)
        clip = self.clipboard
        if clip.kind is Kind.NONE:
            return False
        if clip.kind is Kind.NOTES and target_context.pattern:
            return self._paste_notes(clip.text, target_context)
        with self.session.group(f"Paste {clip.kind.value}"):
            self._duplicate(clip.kind, clip.ids, clip.context, paste=True)
        return True

    def _paste_notes(self, text: str, context: Context) -> bool:
        """Notes from ``.adx`` text: whatever the fragment's first pattern holds, at the
        paste position."""
        fragment, diagnostics = Project.loads(text)
        if any(str(d["severity"]) == "error" for d in diagnostics):
            self.session.failed.emit("paste", "the clipboard is not valid .adx")
            return False
        patterns = fragment.patterns()
        channels = fragment.pattern_channels(patterns[0]) if patterns else []
        if not channels:
            return False
        notes = fragment.notes(patterns[0], channels[0])
        notes["start"] = notes["start"] + self.paste_tick
        notes["id"] = 0
        command = engine_bridge.commands.edit_notes(
            context.pattern,
            context.channel,
            np.zeros(0, dtype=np.uint32),
            engine_bridge.empty_notes(),
            notes,
            "Paste notes",
        )
        return self.session.execute(command)

    # --- duplicate ------------------------------------------------------------------

    def duplicate(self) -> bool:
        """A copy of what is selected, beside it: one undo step."""
        current = self.selection.current
        if not current:
            return False
        with self.session.group(f"Duplicate {current.kind.value}"):
            self._duplicate(current.kind, current.ids, current.context, paste=False)
        return True

    def _duplicate(self, kind: Kind, ids: tuple[int, ...], context: Context, paste: bool) -> None:
        project = self.session.require()
        if kind is Kind.NOTES:
            notes = project.notes(context.pattern, context.channel)
            chosen = notes[np.isin(notes["id"], np.asarray(ids, dtype=np.uint32))].copy()
            if len(chosen) == 0:
                return
            if paste:
                shift = self.paste_tick - int(chosen["start"].min())
            else:
                shift = int((chosen["start"] + chosen["length"]).max() - chosen["start"].min())
            chosen["start"] = chosen["start"] + shift
            chosen["id"] = 0
            self.session.execute(
                engine_bridge.commands.edit_notes(
                    context.pattern,
                    context.channel,
                    np.zeros(0, dtype=np.uint32),
                    engine_bridge.empty_notes(),
                    chosen,
                    "Duplicate notes",
                )
            )
        elif kind is Kind.CLIPS:
            self._duplicate_clips(project, ids, paste)
        elif kind is Kind.POINTS:
            lane = project.automation_points(context.clip)
            picked = lane[[i for i in ids if 0 <= i < len(lane)]].copy()
            if len(picked) == 0:
                return
            span = float(picked[:, 0].max() - picked[:, 0].min())
            start = self.paste_tick if paste else float(picked[:, 0].max()) + max(span, PPQ / 4)
            picked[:, 0] += start - float(picked[:, 0].min())
            merged = np.concatenate([lane, picked])
            merged = merged[np.argsort(merged[:, 0], kind="stable")]
            self.session.build_and_execute("SetBreakpoints", context.clip, merged)
        elif kind is Kind.CHANNELS:
            for channel in ids:
                self.session.call(project.duplicate_channel, channel)
        elif kind is Kind.INSERTS:
            for insert in ids:
                self.session.call(project.duplicate_insert, insert)
        elif kind is Kind.SLOTS:
            for slot in ids:
                self.session.call(project.duplicate_slot, slot)

    def _duplicate_clips(self, project: Project, ids: tuple[int, ...], paste: bool) -> None:
        items = project.playlist_items()
        chosen = items[np.isin(items["item"], np.asarray(ids, dtype=np.uint32))]
        if len(chosen) == 0:
            return
        first = int(chosen["start"].min())
        end = int((chosen["start"] + chosen["length"]).max())
        shift = (self.paste_tick - first) if paste else (end - first)
        samples = {sample_id: full for sample_id, _path, full in project.samples()}
        for row in chosen:
            start = int(row["start"]) + shift
            track = int(row["track"])
            length = int(row["length"]) if int(row["explicit_length"]) else 0
            if int(row["kind"]) == 0:
                self.session.build_and_execute(
                    "AddPatternItem", track, int(row["ref"]), start, length
                )
            elif int(row["kind"]) == 1 and int(row["ref"]) in samples:
                self.session.call(
                    project.add_audio_item, track, samples[int(row["ref"])], start, length
                )
