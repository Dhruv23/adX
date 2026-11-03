"""The only module that imports ``adx_engine`` (phase_5.md 4.8).

Every FFI call the application makes is in this file, which is what makes FINAL_PLAN
2.2's Rule 2 auditable: crossing the boundary is O(interactions), never O(notes). A
function here that returned a list of per-note objects, or a caller that looped over
notes calling one of these, would be the bug the rule exists to prevent - and
``tools/lint.py rules`` fails the build if any other module imports the engine.

Each call is counted in :data:`CALLS`. The performance gate asserts on those counts:
exactly zero engine calls per simulated pan frame, exactly one per meter frame
(phase_5.md 4.9). A count is deterministic; it cannot flake.

The bridge also owns the UI's 60 Hz frame clock (one ``Engine.frame`` call per tick:
playhead, transport state and every meter) and turns engine errors into a Qt signal.
"""

from __future__ import annotations

import ctypes
import functools
import importlib.util
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any, NamedTuple, Protocol, cast

import numpy as np
import numpy.typing as npt

import adx_engine as _native

__all__ = [
    "CALLS",
    "COLOR_ROLES",
    "NOTE_DTYPE",
    "PPQ",
    "Engine",
    "GeometryView",
    "PeakCache",
    "Peaks",
    "PianoRollGeometry",
    "Plugins",
    "Project",
    "QuickPluginError",
    "WaveformGeometry",
    "commands",
    "diagnostic_codes",
    "diff_text",
    "engine_version",
    "format_text",
    "git_sha",
    "hit_test_lane",
    "hit_test_note",
    "hit_test_rect",
    "ideal_tier",
    "load_quick_plugin",
    "make_viewport",
    "validate_text",
]

#: Ticks per quarter note.
PPQ: int = int(_native.PPQ)
#: The structured dtype of one note row (bindings/Notes.h, NoteRecord).
NOTE_DTYPE: np.dtype[Any] = _native.NOTE_DTYPE
#: engine/geometry/ColorPalette.h's roles, in index order.
COLOR_ROLES: tuple[str, ...] = tuple(str(name) for name in _native.COLOR_ROLES)
FLOATS_PER_VERTEX: int = int(_native.FLOATS_PER_VERTEX)
LANE_BAR_PIXELS: float = float(_native.LANE_BAR_PIXELS)
DENSITY_PIXELS_PER_SIXTEENTH: float = float(_native.DENSITY_PIXELS_PER_SIXTEENTH)

#: Hit-test parts (engine/geometry/HitTest.h).
PART_NONE, PART_BODY, PART_LEFT_EDGE, PART_RIGHT_EDGE = 0, 1, 2, 3

NoteArray = npt.NDArray[np.void]


class Native(Protocol):
    """An opaque handle the engine owns - a built command, a viewport, a buffer.

    Python never looks inside one; it hands it back to the engine.
    """


IdArray = npt.NDArray[np.uint32]


class CallCounter:
    """Counts engine calls made through the bridge."""

    def __init__(self) -> None:
        self.count = 0

    @contextmanager
    def measure(self) -> Iterator[list[int]]:
        """Yield a one-element list that holds the calls made inside the block."""
        start = self.count
        result = [0]
        try:
            yield result
        finally:
            result[0] = self.count - start


CALLS = CallCounter()


def _ffi[**P, R](function: Callable[P, R]) -> Callable[P, R]:
    """Mark a bridge function as one engine call."""

    @functools.wraps(function)
    def counted(*args: P.args, **kwargs: P.kwargs) -> R:
        CALLS.count += 1
        return function(*args, **kwargs)

    return counted


# --- versions and the format ---------------------------------------------------------


@_ffi
def engine_version() -> str:
    """The engine's version, "MAJOR.MINOR.PATCH"."""
    return str(_native.version())


@_ffi
def git_sha() -> str:
    """The commit the engine was configured from, or "unknown"."""
    return str(_native.git_sha())


@_ffi
def format_text(text: str) -> tuple[str, list[dict[str, Any]], int]:
    """Canonical text, diagnostics, and the version the text was read as."""
    formatted, raw, version = _native.format_text(text)
    return str(formatted), list(raw), int(version)


@_ffi
def validate_text(text: str, base_dir: str, check_sample_files: bool) -> list[dict[str, Any]]:
    """Every diagnostic for ``text``."""
    return list(_native.validate_text(text, base_dir, check_sample_files))


@_ffi
def diff_text(before: str, after: str) -> list[tuple[str, str, str]]:
    """Semantic changes between two texts: (kind, subject, detail)."""
    return [(str(k), str(s), str(d)) for k, s, d in _native.diff_text(before, after)]


@_ffi
def diagnostic_codes() -> list[tuple[str, str, str]]:
    """Every diagnostic code the engine can emit."""
    return [(str(c), str(s), str(m)) for c, s, m in _native.diagnostic_codes()]


# --- commands ------------------------------------------------------------------------


class _Commands:
    """Command constructors. Building one is an engine call; so is executing it."""

    @_ffi
    def edit_notes(
        self,
        pattern: str,
        channel: str,
        remove: IdArray,
        update: NoteArray,
        add: NoteArray,
        label: str,
    ) -> Native:
        """One gesture's note edit (engine/project/commands/EditNotes.h)."""
        # numpy drops a structured dtype's padding when it concatenates or promotes;
        # astype matches fields by name, so any array of NOTE_DTYPE's fields is accepted.
        result: Native = _native.commands.EditNotes(
            pattern,
            channel,
            np.ascontiguousarray(remove, dtype=np.uint32),
            np.ascontiguousarray(update.astype(NOTE_DTYPE, copy=False)),
            np.ascontiguousarray(add.astype(NOTE_DTYPE, copy=False)),
            label,
        )
        return result

    @_ffi
    def set_slide(
        self,
        pattern: str,
        channel: str,
        note: int,
        target_cents: int | None,
        start: int,
        length: int,
        curve: int,
    ) -> Native:
        """Set (or, with ``target_cents=None``, clear) a note's slide."""
        result: Native = _native.commands.SetNoteSlide(
            pattern, channel, note, target_cents, start, length, curve
        )
        return result

    @_ffi
    def set_pitch_curve(
        self, pattern: str, channel: str, note: int, points: npt.NDArray[np.int64]
    ) -> Native:
        """Replace a note's pitch curve with (at_ticks, cents[, curve]) rows."""
        result: Native = _native.commands.SetPitchCurve(pattern, channel, note, points)
        return result

    @_ffi
    def set_lyric(self, pattern: str, channel: str, note: int, lyric: str) -> Native:
        """Set a note's lyric; empty clears it."""
        result: Native = _native.commands.SetLyric(pattern, channel, note, lyric)
        return result

    @_ffi
    def set_tempo(self, bpm: float, at_ticks: int = 0) -> Native:
        """A tempo change."""
        result: Native = _native.commands.SetTempo(bpm, at_ticks)
        return result

    @_ffi
    def add_marker(self, at_ticks: int, name: str) -> Native:
        """A marker on the ruler."""
        result: Native = _native.commands.AddMarker(at_ticks, name)
        return result

    @_ffi
    def build(self, name: str, *args: object) -> Native:
        """Phase 6's id-addressed constructors (bindings/edit.cpp), by name:
        ``commands.build("SetChannelField", channel_id, "volume", 0.8)``. One engine
        call; an unknown name or a bad argument raises before anything executes."""
        constructor = getattr(_native.commands, name, None)
        if constructor is None:
            raise AttributeError(f"no command {name}")
        result: Native = constructor(*args)
        return result


commands = _Commands()


# --- the project ---------------------------------------------------------------------


def empty_notes(count: int = 0) -> NoteArray:
    """A zeroed note array of ``count`` rows (no engine call)."""
    return np.zeros(count, dtype=NOTE_DTYPE)


class Project:
    """A project and its undo history. Every method is one engine call."""

    def __init__(self, native: Native) -> None:
        self.native: Any = native

    @staticmethod
    @_ffi
    def load(path: str) -> tuple[Project, list[dict[str, Any]]]:
        """Load a file: the project and its diagnostics."""
        native, diagnostics = _native.Project.load(path)
        return Project(native), list(diagnostics)

    @staticmethod
    @_ffi
    def loads(text: str, base_dir: str = "") -> tuple[Project, list[dict[str, Any]]]:
        """Parse text: the project and its diagnostics."""
        native, diagnostics = _native.Project.loads(text, base_dir)
        return Project(native), list(diagnostics)

    @_ffi
    def save(self, path: str) -> None:
        """Write the canonical form to ``path``."""
        self.native.save(path)

    @_ffi
    def dumps(self) -> str:
        """The canonical text."""
        return str(self.native.dumps())

    @_ffi
    def channels(self) -> list[str]:
        """Channel names, in id order."""
        return [str(name) for name in self.native.channels]

    @_ffi
    def patterns(self) -> list[str]:
        """Pattern names, in id order."""
        return [str(name) for name in self.native.patterns]

    @_ffi
    def instruments(self) -> list[tuple[str, bool]]:
        """Each channel's instrument type and whether the engine has it."""
        return [(str(kind), bool(known)) for kind, known in self.native.instruments]

    @_ffi
    def info(self) -> dict[str, Any]:
        """Counts, duration and tempo range."""
        return dict(self.native.info())

    @_ffi
    def revision(self) -> int:
        """The change token: bumped by every edit, undo and redo."""
        return int(self.native.revision)

    @_ffi
    def execute(self, command: Native) -> None:
        """Apply one command: one undo step."""
        self.native.execute(command)

    @_ffi
    def undo(self) -> bool:
        """Undo one step."""
        return bool(self.native.undo())

    @_ffi
    def redo(self) -> bool:
        """Redo one step."""
        return bool(self.native.redo())

    @_ffi
    def history(self) -> list[str]:
        """Undo history labels, oldest first."""
        return [str(label) for label in self.native.history()]

    @_ffi
    def notes(self, pattern: str, channel: str) -> NoteArray:
        """One clip's notes as ONE structured array (a copy)."""
        result: NoteArray = self.native.notes(pattern, channel)
        return result

    @_ffi
    def note_extras(self, pattern: str, channel: str, note: int) -> dict[str, Any]:
        """One note's slide, pitch curve and lyric."""
        return dict(self.native.note_extras(pattern, channel, note))

    @_ffi
    def lyric_cells(
        self, pattern: str, channel: str, tick_start: int, tick_end: int
    ) -> tuple[IdArray, npt.NDArray[np.int64], npt.NDArray[np.int64], list[str], list[str]]:
        """The lyric lane's visible cells: ids, starts, lengths, lyrics, aliases."""
        ids, starts, lengths, lyrics, aliases = self.native.lyric_cells(
            pattern, channel, tick_start, tick_end
        )
        return ids, starts, lengths, [str(s) for s in lyrics], [str(s) for s in aliases]

    @_ffi
    def pattern_length(self, pattern: str) -> int:
        """A pattern's length in ticks."""
        return int(self.native.pattern_length(pattern))

    @_ffi
    def pattern_channels(self, pattern: str) -> list[str]:
        """Channels with notes in a pattern."""
        return [str(name) for name in self.native.pattern_channels(pattern)]

    @_ffi
    def pattern_placements(self, pattern: str) -> npt.NDArray[np.int64]:
        """(n, 3) rows of start, end and source offset: where a pattern is placed."""
        result: npt.NDArray[np.int64] = self.native.pattern_placements(pattern)
        return result

    @_ffi
    def channel_info(self, channel: str) -> dict[str, Any]:
        """A channel's instrument type, whether it is a Voice, and its colour."""
        return dict(self.native.channel_info(channel))

    @_ffi
    def meter_at(self, ticks: int = 0) -> tuple[int, int]:
        """The meter in force at ``ticks``."""
        numerator, denominator = self.native.meter_at(ticks)
        return int(numerator), int(denominator)

    @_ffi
    def bpm_at(self, ticks: int = 0) -> float:
        """The tempo at ``ticks``."""
        return float(self.native.bpm_at(ticks))

    @_ffi
    def seconds_at(self, ticks: int) -> float:
        """Wall-clock seconds at ``ticks``."""
        return float(self.native.seconds_at(ticks))

    @_ffi
    def ticks_at_seconds(self, seconds: float) -> int:
        """The tick that sounds ``seconds`` in: seconds_at's inverse."""
        return int(self.native.ticks_at_seconds(seconds))

    def apply(self, command: Native) -> None:
        """Execute a command built by :data:`commands` (the build was its own call)."""
        self.execute(command)

    # --- Phase 6: the structure the panels read (bindings/structure.cpp) ---------------

    @_ffi
    def channels_info(self) -> list[dict[str, Any]]:
        """Every channel's settings, in id order."""
        return [dict(d) for d in self.native.channels_info()]

    @_ffi
    def mixer_info(self) -> dict[str, Any]:
        """Inserts (with slots and sends), routes and the master's id."""
        return dict(self.native.mixer_info())

    @_ffi
    def midi_bindings(self) -> list[dict[str, Any]]:
        """Learned MIDI controls: kind, number, path, channel (0 any), device, range."""
        return [dict(d) for d in self.native.midi_bindings()]

    @_ffi
    def add_midi_binding(
        self,
        kind: str,
        number: int,
        path: str,
        channel: int = 0,
        device: str = "",
        minimum: float = 0.0,
        maximum: float = 1.0,
        toggle: bool = False,
    ) -> None:
        """Bind a control to a parameter path, replacing whatever that control drove."""
        self.native.add_midi_binding(kind, number, path, channel, device, minimum, maximum, toggle)

    @_ffi
    def remove_midi_binding(self, index: int) -> None:
        """Forget the binding at ``index``."""
        self.native.remove_midi_binding(index)

    @_ffi
    def midi_routes(self) -> list[dict[str, Any]]:
        """The project's MIDI input routes (empty: every input plays the selection)."""
        return [dict(d) for d in self.native.midi_routes()]

    @_ffi
    def set_midi_routes(self, routes: list[dict[str, Any]]) -> None:
        """Replace the routes (one command)."""
        self.native.set_midi_routes(routes)

    @_ffi
    def patterns_info(self) -> list[dict[str, Any]]:
        """Every pattern: id, name, colour, length, channels, automation clips."""
        return [dict(d) for d in self.native.patterns_info()]

    @_ffi
    def playlist_info(self) -> dict[str, Any]:
        """Tracks and arrangements."""
        return dict(self.native.playlist_info())

    @_ffi
    def playlist_items(self) -> npt.NDArray[np.void]:
        """Every placement as ONE structured array (ITEM_DTYPE)."""
        result: npt.NDArray[np.void] = self.native.playlist_items()
        return result

    @_ffi
    def automation_clips(self) -> list[dict[str, Any]]:
        """Every automation clip: id, pattern (0: playlist), target, validity."""
        return [dict(d) for d in self.native.automation_clips()]

    @_ffi
    def automation_points(self, clip: int) -> npt.NDArray[np.float64]:
        """A clip's points: (n, 8) tick, value, curve kind, tension, c1x, c1y, c2x, c2y."""
        result: npt.NDArray[np.float64] = self.native.automation_points(clip)
        return result

    @_ffi
    def item_envelopes(self, track: int, item: int) -> list[dict[str, Any]]:
        """A placement's clip envelopes."""
        return [dict(d) for d in self.native.item_envelopes(track, item)]

    @_ffi
    def controllers_info(self) -> list[dict[str, Any]]:
        """Every controller's settings."""
        return [dict(d) for d in self.native.controllers_info()]

    @_ffi
    def markers(self) -> list[tuple[int, int, str]]:
        """(id, tick, name) per marker."""
        return [(int(i), int(t), str(n)) for i, t, n in self.native.markers()]

    @_ffi
    def audio_item(self, track: int, item: int) -> dict[str, Any] | None:
        """An audio placement's fades, curves, crossfade, edits, warps and lane; None when
        the item is not audio."""
        info = self.native.audio_item(track, item)
        return None if info is None else dict(info)

    @_ffi
    def samples(self) -> list[tuple[int, str, str]]:
        """(id, path as written, resolved path) per pool entry."""
        return [(int(i), str(p), str(f)) for i, p, f in self.native.samples()]

    @_ffi
    def history_info(self) -> tuple[list[str], list[str]]:
        """(done labels oldest first, redo labels next first)."""
        done, redo = self.native.history_info()
        return [str(s) for s in done], [str(s) for s in redo]

    @_ffi
    def param_value(self, path: str) -> float | None:
        """The model's value at a parameter path, or None."""
        value = self.native.param_value(path)
        return None if value is None else float(value)

    @_ffi
    def param_descriptor(self, path: str) -> dict[str, Any] | None:
        """A parameter path's range, unit and scale, or None."""
        value = self.native.param_descriptor(path)
        return None if value is None else dict(value)

    @_ffi
    def param_valid(self, path: str) -> bool:
        """Whether a parameter path names something."""
        return bool(self.native.param_valid(path))

    @_ffi
    def notes_fragment(self, pattern: str, channel: str, ids: list[int]) -> str:
        """Notes as .adx text, written by the Phase 2 writer (the clipboard)."""
        return str(self.native.notes_fragment(pattern, channel, ids))

    @_ffi
    def content_length(self) -> int:
        """The end of the last placement, in ticks."""
        return int(self.native.content_length())

    # --- Phase 6: groups, history and drops (bindings/session.cpp) -----------------------

    @_ffi
    def begin_group(self, label: str) -> None:
        """Everything executed until end_group() is one undo step."""
        self.native.begin_group(label)

    @_ffi
    def end_group(self) -> None:
        """Close the group begin_group() opened."""
        self.native.end_group()

    @_ffi
    def seal_coalescing(self) -> None:
        """The next command starts a new history entry: a new gesture began."""
        self.native.seal_coalescing()

    @_ffi
    def undo_depth(self) -> int:
        """Steps that undo() could undo."""
        return int(self.native.undo_depth)

    @_ffi
    def undo_to(self, depth: int) -> bool:
        """Undo or redo until ``depth`` steps are done."""
        return bool(self.native.undo_to(depth))

    @_ffi
    def add_audio_item(self, track: int, path: str, start: int, length: int = 0) -> int:
        """A sample on the playlist: pool entry and placement, one step. The item id."""
        return int(self.native.add_audio_item(track, path, start, length))

    @_ffi
    def split_item(self, track: int, item: int, at: int, length: int) -> int | None:
        """Cut a placement in two at ``at``, every property kept on both halves, one
        step. ``length`` is its length as the playlist shows it. The new item's id."""
        created = self.native.split_item(track, item, at, length)
        return None if created is None else int(created)

    @_ffi
    def add_sampler_channel(self, name: str, path: str, output: int = 0) -> int:
        """A sampler channel playing ``path``, one step. The channel id."""
        return int(self.native.add_sampler_channel(name, path, output))

    @_ffi
    def apply_preset(self, channel: int, path: str) -> list[dict[str, Any]]:
        """An instrument preset, samples included, onto a channel: one step."""
        return list(self.native.apply_preset(channel, path))

    @_ffi
    def apply_effect_preset(
        self, insert: int, slot: int, path: str
    ) -> tuple[int, list[dict[str, Any]]]:
        """An effect preset onto a slot (slot 0: a new one on ``insert``): one step."""
        slot_id, diagnostics = self.native.apply_effect_preset(insert, slot, path)
        return int(slot_id), list(diagnostics)

    @_ffi
    def routing_matrix(self) -> tuple[list[int], npt.NDArray[np.uint8]]:
        """(insert ids ascending, n x n states): 0 none, 1 route, 2 send, 3 sidechain,
        4 would close a cycle, 5 self. Rows are sources."""
        ids, matrix = self.native.routing_matrix()
        return [int(i) for i in ids], matrix

    @_ffi
    def add_zone(
        self, channel: int, path: str, key_low: int = 0, key_high: int = 127, root: int = 60
    ) -> None:
        """One more sampler zone playing ``path``, one step."""
        self.native.add_zone(channel, path, key_low, key_high, root)

    @_ffi
    def zones(self, channel: int) -> list[dict[str, Any]]:
        """A sampler channel's zones."""
        return [dict(d) for d in self.native.zones(channel)]

    @_ffi
    def duplicate_channel(self, channel: int) -> int:
        """A copy of a channel, one step. The new id."""
        return int(self.native.duplicate_channel(channel))

    @_ffi
    def duplicate_insert(self, insert: int) -> int:
        """A copy of an insert with its effects, sends and routes, one step."""
        return int(self.native.duplicate_insert(insert))

    @_ffi
    def duplicate_slot(self, slot: int) -> int:
        """A copy of an effect slot on its insert, one step."""
        return int(self.native.duplicate_slot(slot))

    @_ffi
    def set_slot_impulse(self, slot: int, path: str) -> None:
        """A Convolution slot's impulse response file (empty: synthetic), one step."""
        self.native.set_slot_impulse(slot, path)

    @_ffi
    def set_slot_effect(self, slot: int, effect_type: str) -> None:
        """Replace a slot's effect with a fresh one of ``effect_type``."""
        self.native.set_slot_effect(slot, effect_type)


# --- the engine ----------------------------------------------------------------------

#: Columns of a levels row: insert id, peak L/R, RMS L/R, momentary, short-term,
#: integrated (LUFS), true peak.
LEVEL_COLUMNS = 9


class Engine:
    """The render engine and its transport. Every method is one engine call."""

    def __init__(
        self,
        null_backend: bool = False,
        sample_rate: int = 48000,
        block_frames: int = 256,
        input_channels: int = 0,
        input_device: int | None = None,
        loopback: int | None = None,
        exclusive: bool = False,
    ) -> None:
        """``input_channels`` > 0 opens an input for recording. ``loopback`` makes a
        test's engine: no device, rendered by :meth:`render_offline`, its outputs wired
        back to its inputs that many frames later. ``exclusive`` plays the default
        output in WASAPI exclusive mode (output only; other apps lose the device)."""
        CALLS.count += 1
        self.native = _native.Engine(
            sample_rate=sample_rate,
            block_frames=block_frames,
            null_backend=null_backend,
            input_channels=input_channels,
            input_device=input_device,
            loopback=loopback,
            exclusive=exclusive,
        )
        self.null_backend = null_backend
        #: Inputs the stream was opened with: 0 means it cannot record.
        self.input_channels = input_channels
        self._sample_rate = sample_rate

    @property
    @_ffi
    def audio_pending(self) -> bool:
        """An audio clip still plays a silent stand-in (decoding, or rendering its
        stretch); :meth:`commit` swaps the real buffer in once it is ready."""
        return bool(self.native.audio_pending)

    @_ffi
    def midi_inputs(self) -> list[str]:
        """The system's MIDI input ports (after start())."""
        return [str(name) for name in self.native.midi_inputs()]

    @_ffi
    def open_midi(self, name: str) -> None:
        """Open a MIDI input (raises RuntimeError); it plays the MIDI target channel."""
        self.native.open_midi(name)

    @_ffi
    def close_midi(self, name: str) -> None:
        """Close a MIDI input."""
        self.native.close_midi(name)

    @property
    @_ffi
    def midi_opened(self) -> list[str]:
        """The open MIDI inputs."""
        return [str(name) for name in self.native.midi_opened]

    @_ffi
    def set_midi_target(self, channel: int) -> None:
        """The channel live MIDI plays (0: none)."""
        self.native.set_midi_target(channel)

    @_ffi
    def render_offline(self, frames: int) -> None:
        """A loopback engine only: render ``frames`` now, as a device would have."""
        self.native.render_offline(frames)

    @_ffi
    def start(self) -> None:
        """Open and start the stream."""
        self.native.start()

    @_ffi
    def stop(self) -> None:
        """Stop the stream."""
        self.native.stop()

    @_ffi
    def set_project(self, project: Project) -> None:
        """Snapshot ``project`` for the audio thread."""
        self.native.set_project(project.native)

    @_ffi
    def commit(self, project: Project) -> bool:
        """Bring the audio thread up to the project's latest edit."""
        return bool(self.native.commit(project.native))

    @_ffi
    def frame(self, levels: npt.NDArray[np.float32]) -> tuple[int, bool, int, int]:
        """One UI frame: (position_ticks, rolling, strip_count, position_samples); fills
        ``levels`` and pumps the engine's housekeeping."""
        ticks, rolling, strips, samples = self.native.frame(levels)
        return int(ticks), bool(rolling), int(strips), int(samples)

    @property
    def sample_rate(self) -> int:
        """The stream's rate (read once, at construction)."""
        return self._sample_rate

    @_ffi
    def play(self) -> None:
        """Start the arrangement transport."""
        self.native.transport.play()

    @_ffi
    def stop_playback(self) -> None:
        """Stop the arrangement transport."""
        self.native.transport.stop()

    @_ffi
    def seek(self, ticks: int) -> None:
        """Move the playhead."""
        self.native.transport.seek_ticks(ticks)

    @_ffi
    def set_loop(self, start: int, end: int, enabled: bool) -> None:
        """Set the transport loop."""
        self.native.transport.set_loop(start, end, enabled)

    @_ffi
    def position_ticks(self) -> int:
        """Where the transport is."""
        return int(self.native.transport.position_ticks())

    @_ffi
    def state(self) -> str:
        """stopped, playing, recording or paused."""
        return str(self.native.transport.state())

    @_ffi
    def worst_callback_ms(self) -> float:
        """The slowest audio callback since the stream started."""
        return float(self.native.worst_callback_ms)

    @_ffi
    def frame_with_meters(
        self, levels: npt.NDArray[np.float32], meters: MeterGeometry
    ) -> tuple[int, bool, int, int, int, int, int]:
        """frame(), also building every strip's meter vertices - still ONE call. Adds
        (meter address, vertex count, revision); upload before any other engine call."""
        ticks, rolling, strips, samples, address, count, revision = self.native.frame_with_meters(
            levels, meters.native
        )
        return (
            int(ticks),
            bool(rolling),
            int(strips),
            int(samples),
            int(address),
            int(count),
            int(revision),
        )

    @_ffi
    def frame_with_analyzers(
        self,
        levels: npt.NDArray[np.float32],
        meters: MeterGeometry | None,
        analyzers: Analyzers,
        dt: float,
    ) -> AnalyzerFrame:
        """frame(), the meters (or none) and every open analyzer built - still ONE call
        (visualizer_one_ffi_call). Upload what it returns before any other engine call."""
        ticks, rolling, strips, samples, meter, layers, image, readouts = (
            self.native.frame_with_analyzers(
                levels, meters.native if meters is not None else None, analyzers.native, dt
            )
        )
        return AnalyzerFrame(
            int(ticks),
            bool(rolling),
            int(strips),
            int(samples),
            None if meter is None else (int(meter[0]), int(meter[1]), int(meter[2])),
            {
                name: (int(a), int(c), int(r))
                for name, (a, c, r) in zip(ANALYZER_LAYERS, layers, strict=True)
            },
            (int(image[0]), int(image[1]), int(image[2]), int(image[3]), int(image[4])),
            (float(readouts[0]), float(readouts[1]), float(readouts[2]), float(readouts[3])),
        )

    @_ffi
    def set_param(self, project: Project, path: str, value: float) -> bool:
        """A knob turn: into the project and undo, and to the audio thread with no
        rebuild. Consecutive turns of one control coalesce into one step."""
        return bool(self.native.set_param(project.native, path, value))

    @_ffi
    def audition(self, channel: int, pitch: int, velocity: int = 100, seconds: float = 0.3) -> None:
        """Play one note through a channel's instrument, outside the arrangement."""
        self.native.audition(channel, pitch, velocity, seconds)

    @_ffi
    def stop_audition(self) -> None:
        """Release every audition note."""
        self.native.stop_audition()

    @_ffi
    def set_metronome(self, enabled: bool, beat_ticks: int = PPQ, beats_per_bar: int = 4) -> None:
        """The click, realtime only."""
        self.native.set_metronome(enabled, beat_ticks, beats_per_bar)

    @_ffi
    def eq_spectrum(self, slot: int, geometry: EqCurveGeometry) -> bool:
        """Fill ``geometry``'s spectrum from a running ParametricEq's tap."""
        return bool(self.native.eq_spectrum(slot, geometry.native))

    @_ffi
    def gain_reduction(self, slot: int) -> float:
        """A compressor's or limiter's gain reduction over its last block, dB."""
        return float(self.native.gain_reduction(slot))

    @_ffi
    def latency_samples(self) -> int:
        """Latency along the slowest path: the preferences' readout."""
        return int(self.native.latency_samples)

    @property
    @_ffi
    def stream(self) -> dict[str, Any]:
        """What the device actually opened: block size, output latency, xruns, and the
        exclusive-mode sample format ("" otherwise)."""
        return {
            "exclusive": bool(self.native.exclusive),
            "block_frames": int(self.native.stream_block_frames),
            "output_latency_ms": float(self.native.stream_output_latency_ms),
            "xruns": int(self.native.stream_xruns),
            "format": str(self.native.output_format),
        }

    @_ffi
    def xruns(self) -> int:
        """Underruns since the stream opened: read before and after; any rise is a glitch."""
        return int(self.native.xruns())

    @_ffi
    def callback_timings(self, since: int) -> tuple[npt.NDArray[np.uint32], int, int]:
        """Callback durations (ns) from ring position ``since``: (durations, next, lost)."""
        durations, following, lost = self.native.callback_timings(since)
        return durations, int(following), int(lost)

    @_ffi
    def late_callbacks(self) -> int:
        """Callbacks that overran their block's real-time duration."""
        return int(self.native.late_callbacks())

    @_ffi
    def capture_start(self) -> None:
        """Start capturing the master bus (phase_10.5.md 4.5)."""
        self.native.capture_start()

    @_ffi
    def capture_frames(self) -> int:
        """Frames captured so far."""
        return int(self.native.capture_frames())

    @_ffi
    def capture_stop(self, path: str | None = None) -> dict[str, Any]:
        """Stop; a float WAV at ``path``, or the samples (interleaved) without one.
        ``lost`` is 0 for a whole capture."""
        return dict(self.native.capture_stop(path))

    @_ffi
    def set_callback_stall_us(self, microseconds: int) -> None:
        """Fault injection for tests: each callback spins this long (0 is off)."""
        self.native.set_callback_stall_us(microseconds)

    @_ffi
    def arm_output_onset(self, threshold: float) -> None:
        """Exclusive mode only: note the next output frame louder than ``threshold``."""
        self.native.arm_output_onset(threshold)

    @_ffi
    def output_onset_time(self) -> int:
        """When the armed onset plays by the device clock (QPC, 100 ns); 0 until heard."""
        return int(self.native.output_onset_time())

    # --- hosted plugins and MIDI learn (phase_9.md) ----------------------------------

    @_ffi
    def plugin_instance_of_slot(self, slot: int) -> int:
        """The live plugin instance in a slot, or 0."""
        return int(self.native.plugin_instance_of_slot(slot))

    @_ffi
    def plugin_instance_of_channel(self, channel: int) -> int:
        """A channel's live plugin instrument, or 0."""
        return int(self.native.plugin_instance_of_channel(channel))

    @_ffi
    def sync_plugin_states(self, project: Project) -> int:
        """Store every live plugin's state in the project; how many changed."""
        return int(self.native.sync_plugin_states(project.native))

    @_ffi
    def apply_plugin_echoes(self, project: Project) -> int:
        """Put parameter changes made in plugins' own editors into the project."""
        return int(self.native.apply_plugin_echoes(project.native))

    @_ffi
    def apply_learned(self, project: Project) -> int:
        """Put the values learned MIDI controls wrote into the project."""
        return int(self.native.apply_learned(project.native))

    @_ffi
    def midi_last_control(self) -> dict[str, Any] | None:
        """The newest controller message since the last call, or None."""
        control = self.native.midi_last_control()
        return None if control is None else dict(control)

    @_ffi
    def refresh(self, project: Project) -> bool:
        """Rebuild the whole graph (a plugin's latency changed)."""
        return bool(self.native.refresh(project.native))


# --- geometry ------------------------------------------------------------------------


@_ffi
def make_viewport(
    tick_start: float,
    pitch_top: float,
    pixels_per_tick: float,
    pixels_per_semitone: float,
    width: float,
    height: float,
) -> Native:
    """A native Viewport (engine/geometry/Viewport.h)."""
    result: Native = _native.Viewport(
        tick_start, pitch_top, pixels_per_tick, pixels_per_semitone, width, height
    )
    return result


@dataclass(frozen=True, slots=True)
class GeometryView:
    """A zero-copy view of one buffer, valid only inside its ``with`` block."""

    array: npt.NDArray[np.float32]
    address: int
    vertex_count: int
    revision: int


@contextmanager
def _lease(buffer: Native) -> Iterator[GeometryView]:
    with cast(Any, buffer).lease() as lease:
        yield GeometryView(
            array=np.asarray(lease),
            address=int(lease.address),
            vertex_count=int(lease.vertex_count),
            revision=int(lease.revision),
        )


class PianoRollGeometry:
    """The piano roll's vertex builder (engine/geometry/PianoRollGeometry.h)."""

    LAYERS = ("rows", "grid", "ghosts", "notes", "lanes", "curves")

    def __init__(self) -> None:
        CALLS.count += 1
        self.native = _native.PianoRollGeometry()

    @_ffi
    def build(
        self,
        project: Project,
        viewport: Native,
        pattern: str,
        channel: str,
        selected: IdArray,
        lane: int = 0,
        scale_root: int = 0,
        scale_mask: int = 0,
        grid_division: int = 960,
        ghosts: bool = True,
    ) -> None:
        """Fill every layer: O(visible notes), in C++, GIL released."""
        self.native.build(
            project.native,
            viewport,
            pattern,
            channel,
            selected,
            lane,
            scale_root,
            scale_mask,
            grid_division,
            ghosts,
        )

    @_ffi
    def update_selection(self, selected: IdArray) -> None:
        """Recolour for a new selection, in place."""
        self.native.update_selection(selected)

    @_ffi
    def density(self) -> bool:
        """Whether the last build drew density runs."""
        return bool(self.native.density)

    @_ffi
    def visible_ids(self) -> IdArray:
        """The notes the last build drew, as one array."""
        result: IdArray = self.native.visible_ids()
        return result

    @contextmanager
    def view(self, layer: int) -> Iterator[GeometryView]:
        """Lease one layer's buffer: one engine call, a zero-copy view inside."""
        CALLS.count += 1
        with _lease(getattr(self.native, self.LAYERS[layer])) as view:
            yield view

    def native_buffer(self, layer: int) -> Native:
        """The native buffer itself, for tests of the lease contract."""
        buffer: Native = getattr(self.native, self.LAYERS[layer])
        return buffer


@_ffi
def hit_test_note(
    project: Project, pattern: str, channel: str, viewport: Native, x: float, y: float
) -> tuple[int, int]:
    """The topmost note under a pixel, and which part of it: (id or 0, part)."""
    note, part = _native.hit_test_note(project.native, pattern, channel, viewport, x, y)
    return int(note), int(part)


@_ffi
def hit_test_rect(
    project: Project,
    pattern: str,
    channel: str,
    viewport: Native,
    rect: tuple[float, float, float, float],
) -> IdArray:
    """Every note a pixel rectangle touches: one array."""
    result: IdArray = _native.hit_test_rect(project.native, pattern, channel, viewport, *rect)
    return result


@_ffi
def hit_test_lane(
    project: Project,
    pattern: str,
    channel: str,
    viewport: Native,
    lane_height: float,
    x: float,
    y: float,
) -> tuple[int, float]:
    """The note whose lane bar is under x, and the lane value at y."""
    note, value = _native.hit_test_lane(
        project.native, pattern, channel, viewport, lane_height, x, y
    )
    return int(note), float(value)


# --- Phase 6: catalog, presets, render ----------------------------------------------

#: The structured dtype of one placement row (bindings/structure.cpp, ItemRecord).
ITEM_DTYPE: np.dtype[Any] = _native.ITEM_DTYPE
CLIP_DETAIL_PIXELS: float = float(_native.CLIP_DETAIL_PIXELS)
METER_FLOOR_DB: float = float(_native.METER_FLOOR_DB)


@_ffi
def enumerate_devices() -> list[dict[str, Any]]:
    """Every audio device the system exposes (the preferences' device list)."""
    return [dict(d) for d in _native.enumerate_devices()]


@_ffi
def type_catalog() -> list[dict[str, Any]]:
    """Every instrument and effect type with its parameter descriptors."""
    return [dict(d) for d in _native.type_catalog()]


@_ffi
def curve_evaluate(
    kind: int, tension: float, c1x: float, c1y: float, c2x: float, c2y: float, t: float
) -> float:
    """core::Curve::evaluate - the engine's own curve, for a tooltip or a test."""
    return float(_native.curve_evaluate(kind, tension, c1x, c1y, c2x, c2y, t))


@_ffi
def scan_presets(directories: list[str]) -> list[dict[str, Any]]:
    """Every .adxpreset under the folders: name, type, pack, tags, path, kind."""
    return [dict(d) for d in _native.scan_presets(directories)]


@_ffi
def read_preset(path: str) -> dict[str, Any]:
    """One preset's summary."""
    return dict(_native.read_preset(path))


@_ffi
def render_to_file(project: Project, path: str, tail_seconds: float = 2.0) -> dict[str, Any]:
    """Render the arrangement offline through the realtime engine's code and write a
    32-bit float WAV: the statistics, including the golden-corpus hash."""
    return dict(_native.render_offline(project.native, path, tail_seconds=tail_seconds))


@_ffi
def steps_of(
    project: Project, pattern: int, channel: int, step_ticks: int, steps: int
) -> npt.NDArray[np.uint32]:
    """One channel's steps: (n, 2) note id (0: none) and velocity."""
    result: npt.NDArray[np.uint32] = _native.steps_of(
        project.native, pattern, channel, step_ticks, steps
    )
    return result


@_ffi
def hit_test_lane_point(
    points: npt.NDArray[np.float64],
    lane: tuple[float, float, bool],
    tick_start: float,
    pixels_per_tick: float,
    height_px: float,
    x: float,
    y: float,
) -> int:
    """The automation point under a pixel, or -1."""
    return int(
        _native.hit_test_lane_point(
            points, lane[0], lane[1], lane[2], tick_start, pixels_per_tick, height_px, x, y
        )
    )


def lane_normalise(value: float, lane: tuple[float, float, bool]) -> float:
    """A value's position in its range, 0..1, as the engine computes it."""
    CALLS.count += 1
    return float(_native.lane_normalise(value, lane[0], lane[1], lane[2]))


def lane_positions(
    values: npt.NDArray[np.float64], lane: tuple[float, float, bool]
) -> npt.NDArray[np.float64]:
    """Every value's position in its range, 0..1: one call, one array."""
    CALLS.count += 1
    result: npt.NDArray[np.float64] = _native.lane_positions(
        np.ascontiguousarray(values, dtype=np.float64), lane[0], lane[1], lane[2]
    )
    return result


def lane_denormalise(position: float, lane: tuple[float, float, bool]) -> float:
    """The inverse of :func:`lane_normalise`."""
    CALLS.count += 1
    return float(_native.lane_denormalise(position, lane[0], lane[1], lane[2]))


@_ffi
def fm_algorithm(number: int) -> tuple[list[int], int, int, int]:
    """The FM instrument's routing for algorithm 1..32: (modulator bitmask per
    operator, carrier bitmask, feedback from, feedback to)."""
    modulators, carriers, source, target = _native.fm_algorithm(number)
    return [int(m) for m in modulators], int(carriers), int(source), int(target)


@_ffi
def wavetable_frames(bank: int, samples_per_frame: int = 128) -> npt.NDArray[np.float32]:
    """A built-in wavetable's frames, (frames, samples)."""
    result: npt.NDArray[np.float32] = _native.wavetable_frames(bank, samples_per_frame)
    return result


@_ffi
def grossbeat_curve(pattern: int, points: int = 128) -> npt.NDArray[np.float64]:
    """GrossBeat's time map for a pattern, at evenly spaced positions."""
    result: npt.NDArray[np.float64] = _native.grossbeat_curve(pattern, points)
    return result


@_ffi
def eq_axis_position(frequency: float) -> float:
    """Where a frequency sits on the EQ curve's 20 Hz - 20 kHz log axis, 0..1."""
    return float(_native.eq_axis_position(frequency))


@_ffi
def eq_axis_frequency(x: float) -> float:
    """The frequency at an EQ axis position."""
    return float(_native.eq_axis_frequency(x))


class _Builder:
    """A native geometry builder whose buffers are leased by name."""

    LAYERS: tuple[str, ...] = ()

    def __init__(self, native: object) -> None:
        CALLS.count += 1
        self.native: Any = native

    @contextmanager
    def view(self, layer: int) -> Iterator[GeometryView]:
        """Lease one layer's buffer: one engine call, a zero-copy view inside."""
        CALLS.count += 1
        with _lease(getattr(self.native, self.LAYERS[layer])) as view:
            yield view


class PlaylistGeometry(_Builder):
    """engine/geometry/PlaylistGeometry.h."""

    LAYERS = ("lanes", "grid", "clips", "previews", "curves")

    def __init__(self) -> None:
        super().__init__(_native.PlaylistGeometry())

    @_ffi
    def build(
        self,
        project: Project,
        view: tuple[float, float, float, float, float, float],
        selected: IdArray,
        peaks: PeakCache | None = None,
    ) -> None:
        """Fill every layer: ``view`` is (tick_start, tick_end, row_start, row_end,
        pixels_per_tick, pixels_per_row)."""
        self.native.build(project.native, *view, selected, None if peaks is None else peaks.native)

    @_ffi
    def rows(self) -> npt.NDArray[np.float64]:
        """(n, 3): track id, top row, height - the last build's layout."""
        result: npt.NDArray[np.float64] = self.native.rows()
        return result

    @_ffi
    def visible(self) -> npt.NDArray[np.float64]:
        """(n, 8) per drawn clip: item, track, start, end, top, height, kind, ref."""
        result: npt.NDArray[np.float64] = self.native.visible()
        return result

    @_ffi
    def hit_test(
        self, view: tuple[float, float, float, float], x: float, y: float
    ) -> tuple[int, int, int]:
        """(track, item or 0, part) under a pixel; view is (tick_start, row_start,
        pixels_per_tick, pixels_per_row)."""
        track, item, part = self.native.hit_test(*view, x, y)
        return int(track), int(item), int(part)

    @_ffi
    def status(self) -> tuple[bool, int]:
        """(peaks still pending, clips drawn with detail)."""
        return bool(self.native.pending_peaks), int(self.native.detailed)


class StepGridGeometry(_Builder):
    """engine/geometry/StepGridGeometry.h."""

    LAYERS = ("cells", "velocity")

    def __init__(self) -> None:
        super().__init__(_native.StepGridGeometry())

    @_ffi
    def build(
        self,
        project: Project,
        pattern: int,
        channels: IdArray,
        step_ticks: int,
        steps: int,
        group_size: int = 4,
    ) -> int:
        """The whole grid in one C++ build; returns the lit cell count."""
        self.native.build(project.native, pattern, channels, step_ticks, steps, group_size)
        return int(self.native.lit)


class AutomationGeometry(_Builder):
    """engine/geometry/AutomationGeometry.h."""

    LAYERS = ("line", "points")

    def __init__(self) -> None:
        super().__init__(_native.AutomationGeometry())

    @_ffi
    def build(
        self,
        points: npt.NDArray[np.float64],
        lane: tuple[float, float, bool],
        view: tuple[float, float, float, float],
        selected: npt.NDArray[np.uint8],
        segments: int = 0,
        place: tuple[float, float, float] = (0.0, 0.0, 1.0),
    ) -> None:
        """Tessellate with the engine's curve: view is (tick_start, tick_end,
        pixels_per_tick, height_px); ``place`` is (tick offset, y top, y scale) for an
        envelope drawn on its clip."""
        self.native.build(points, lane[0], lane[1], lane[2], *view, segments, selected, *place)


class MeterGeometry(_Builder):
    """engine/geometry/MeterGeometry.h. Engine.frame_with_meters() builds it."""

    LAYERS = ("bars",)

    def __init__(self) -> None:
        super().__init__(_native.MeterGeometry())

    @_ffi
    def build(self, levels: npt.NDArray[np.float32], count: int) -> None:
        """Build from a levels array (tests; the frame clock uses frame_with_meters)."""
        self.native.build(levels, count)

    @_ffi
    def reset_clips(self) -> None:
        """Clear every latched clip indicator."""
        self.native.reset_clips()


#: Analyzers' layers, in the order frame_with_analyzers returns their uploads.
ANALYZER_LAYERS: tuple[str, ...] = tuple(str(name) for name in _native.Analyzers.layers)


class AnalyzerFrame(NamedTuple):
    """What Engine.frame_with_analyzers returns."""

    ticks: int
    rolling: bool
    strips: int
    samples: int
    #: (address, vertex count, revision), or None without meters.
    meters: tuple[int, int, int] | None
    #: Per ANALYZER_LAYERS name: (address, vertex count, revision).
    layers: dict[str, tuple[int, int, int]]
    #: The spectrogram image: (address, rows, columns, head, rows pushed).
    spectrogram: tuple[int, int, int, int, int]
    #: (phase correlation, momentary, short-term, integrated LUFS).
    readouts: tuple[float, float, float, float]


class Analyzers:
    """The analyzers on an engine's master tap (engine/geometry/AnalyzerHub.h); the FFT
    runs on its own worker. Engine.frame_with_analyzers builds them."""

    def __init__(self, engine: Engine) -> None:
        CALLS.count += 1
        self.native = _native.Analyzers(engine.native)

    @_ffi
    def configure(self, **settings: object) -> None:
        """Open or close analyzers and set them (keys: bindings/analyzers.cpp)."""
        self.native.configure(settings)

    @_ffi
    def settings(self) -> dict[str, Any]:
        """The current settings."""
        return dict(self.native.settings())

    @property
    @_ffi
    def transforms(self) -> int:
        """FFTs the worker has run."""
        return int(self.native.transforms)


class EqCurveGeometry(_Builder):
    """engine/geometry/EqCurveGeometry.h."""

    LAYERS = ("curve", "handles", "spectrum")

    def __init__(self) -> None:
        super().__init__(_native.EqCurveGeometry())

    @_ffi
    def build(
        self, effect_type: str, params: npt.NDArray[np.float32], sample_rate: float = 48000.0
    ) -> npt.NDArray[np.float64]:
        """The response of ``effect_type`` with ``params`` (descriptor order); returns
        the response in dB at each point."""
        self.native.build(effect_type, params, sample_rate)
        result: npt.NDArray[np.float64] = self.native.response()
        return result


# --- waveforms -----------------------------------------------------------------------


class Peaks:
    """One clip's tiered peaks, filling in on a worker."""

    def __init__(self, native: Native) -> None:
        self.native: Any = native

    @_ffi
    def status(self) -> tuple[bool, bool, int, int, int]:
        """(complete, failed, progress, frames, sample_rate) in one call."""
        n = self.native
        return bool(n.complete), bool(n.failed), int(n.progress), int(n.frames), int(n.sample_rate)

    @_ffi
    def error(self) -> str:
        """Why decoding failed, if it did."""
        return str(self.native.error)


class PeakCache:
    """engine/geometry/ClipPeakCache.h: never blocks on analysis."""

    def __init__(self) -> None:
        CALLS.count += 1
        self.native = _native.ClipPeakCache()

    @_ffi
    def request_file(self, path: str, gain: float = 1.0) -> Peaks:
        """Peaks for a file, decoded and analysed on the worker."""
        return Peaks(self.native.request_file(path, gain))

    @_ffi
    def request_array(
        self, name: str, samples: npt.NDArray[np.float32], sample_rate: int, gain: float = 1.0
    ) -> Peaks:
        """Peaks for samples already in memory."""
        return Peaks(self.native.request_array(name, samples, sample_rate, gain))

    @_ffi
    def wait_idle(self) -> None:
        """Block until the worker is idle (tests)."""
        self.native.wait_idle()


class WaveformGeometry:
    """Peaks to a min/max triangle strip: two vertices per column."""

    def __init__(self) -> None:
        CALLS.count += 1
        self.native = _native.WaveformGeometry()

    @_ffi
    def build(self, peaks: Peaks, frame_start: float, frame_end: float, columns: int) -> int:
        """Build over a frame range; returns the tier read (-1: still pending)."""
        self.native.build(peaks.native, frame_start, frame_end, columns)
        return int(self.native.tier)

    @contextmanager
    def view(self) -> Iterator[GeometryView]:
        """Lease the strip: one engine call."""
        CALLS.count += 1
        with _lease(self.native.strip) as view:
            yield view


@_ffi
def ideal_tier(samples_per_column: float) -> int:
    """The peak tier a zoom level reads."""
    return int(_native.ideal_tier(samples_per_column))


# --- the scene-graph plugin ----------------------------------------------------------


class QuickPluginError(RuntimeError):
    """adx_quick is missing or was built against a different Qt."""


_quick_loaded = False


def quick_plugin_path() -> Path:
    """Where adx_quick.dll is installed: beside the engine extension."""
    spec = importlib.util.find_spec("adx_engine")
    origin = spec.origin if spec is not None else None
    if origin is None:
        raise QuickPluginError("adx_engine is not installed")
    return Path(origin).parent / "adx_quick.dll"


def _quick_library() -> ctypes.CDLL:
    """adx_quick, loaded against PySide6's Qt (which must be loaded first)."""
    import os

    import PySide6
    from PySide6 import QtQml, QtQuick  # noqa: F401 - loads Qt6Qml/Qt6Quick for the DLL

    path = quick_plugin_path()
    if not path.exists():
        raise QuickPluginError(
            f"{path.name} was not built: the scene-graph plugin needs a Qt C++ SDK. "
            "Run `python tools/fetch_qt.py`, then `pip install -e .` (phase_5.md 4.3)."
        )
    # The plugin's own imports (Qt6Quick.dll and friends) resolve against PySide6's Qt.
    os.add_dll_directory(str(Path(PySide6.__file__).parent))
    library = ctypes.CDLL(str(path))
    library.adx_quick_qt_version.restype = ctypes.c_char_p
    return library


def load_quick_plugin() -> None:
    """Load adx_quick into the running Qt and register ``import Adx 1.0``. Idempotent.

    Must run before QML that imports Adx is loaded.
    """
    global _quick_loaded
    if _quick_loaded:
        return
    from PySide6.QtCore import qVersion

    library = _quick_library()
    built = bytes(library.adx_quick_qt_version()).decode()
    if library.adx_quick_register() != 0:
        raise QuickPluginError(f"adx_quick was built against Qt {built}; running Qt {qVersion()}")
    _quick_loaded = True


def quick_plugin_qt_version() -> str:
    """The Qt version adx_quick was built against."""
    return bytes(_quick_library().adx_quick_qt_version()).decode()


# --- the text-first layer (phase_7.md) -----------------------------------------------

#: Live mode's write debounce and the watcher's quiet window (engine/sync/ReloadPolicy.h).
LIVE_WRITE_DEBOUNCE_MS = 300
WATCHER_DEBOUNCE_MS = 150


class TextSync:
    """The bidirectional coordinator (engine/sync/TextSync.h): one project and its file.

    Every apply returns ``{status, commands, structural, diagnostics}``; status is one of
    ``applied``, ``unchanged``, ``invalid``, ``conflict``, ``self-write``. Every method is
    one engine call.
    """

    def __init__(self, project: Project, base_dir: str = "") -> None:
        CALLS.count += 1
        self.native = _native.sync.TextSync(project.native, base_dir)

    @_ffi
    def set_mode(self, mode: str) -> None:
        """``off``, ``on-save`` or ``live``."""
        self.native.set_mode(mode)

    @_ffi
    def mode(self) -> str:
        """The current mode."""
        return str(self.native.mode())

    @_ffi
    def mark_synced(self, text: str) -> None:
        """The file and the model agree on ``text``."""
        self.native.mark_synced(text)

    @_ffi
    def render_text(self) -> str:
        """The model's canonical text, spelled as the file is."""
        return str(self.native.render_text())

    @_ffi
    def apply_text(self, text: str) -> dict[str, Any]:
        """The editor's buffer changed."""
        return dict(self.native.apply_text(text))

    @_ffi
    def apply_external(self, text: str) -> dict[str, Any]:
        """The file changed on disk."""
        return dict(self.native.apply_external(text))

    @_ffi
    def apply_forced(self, text: str) -> dict[str, Any]:
        """Take theirs: apply over unsynced model edits."""
        return dict(self.native.apply_forced(text))

    @_ffi
    def note_write(self, text: str) -> None:
        """adX wrote ``text``; the watcher event it causes is an echo."""
        self.native.note_write(text)

    @_ffi
    def model_dirty(self) -> bool:
        """The model has edits the file has not seen."""
        return bool(self.native.model_dirty())

    @_ffi
    def synced_text(self) -> str:
        """What the file held when the two last agreed."""
        return str(self.native.synced_text())


class FileWatcher:
    """A file on disk (engine/sync/FileWatcher.h): :meth:`poll` is true once per save."""

    def __init__(self, path: str, debounce_ms: int = WATCHER_DEBOUNCE_MS) -> None:
        CALLS.count += 1
        self.path = path
        self.native = _native.sync.FileWatcher(path, debounce_ms)

    @_ffi
    def poll(self) -> bool:
        """True once per burst of changes, after the debounce window."""
        return bool(self.native.poll())

    @_ffi
    def watching(self) -> bool:
        """False when the directory could not be watched."""
        return bool(self.native.watching())


class IncrementalParse:
    """The editor's diagnostics, kept current keystroke by keystroke (phase_7.md 4.3)."""

    def __init__(self) -> None:
        CALLS.count += 1
        self.native = _native.sync.IncrementalParse()

    @_ffi
    def reset(self, text: str) -> None:
        """A full parse."""
        self.native.reset(text)

    @_ffi
    def update(self, text: str) -> list[dict[str, Any]]:
        """Re-diagnose ``text``; returns its diagnostics."""
        self.native.update(text)
        return list(self.native.diagnostics())

    def last_ms(self) -> float:
        """How long the last update took (no engine call: a read of a stored value)."""
        return float(self.native.last_ms())

    def last_incremental(self) -> bool:
        """Whether the last update took the cheap path."""
        return bool(self.native.last_incremental())


@_ffi
def semantic_tokens(text: str) -> npt.NDArray[np.int32]:
    """Rows of (line 1-based, byte column, byte length, kind index) (Tokenize.h)."""
    return cast(npt.NDArray[np.int32], _native.sync.semantic_tokens(text))


@functools.cache
def semantic_kinds() -> tuple[str, ...]:
    """Token kind names, by index. Fixed for a build, so asked once."""
    CALLS.count += 1
    return tuple(str(name) for name in _native.sync.semantic_kinds())


@_ffi
def mini_compile(
    text: str,
    cycle_start: int = 0,
    cycle_length: int = PPQ * 4,
    cycle_index: int = 0,
    seed: int = 0,
) -> dict[str, Any]:
    """One cycle of mini-notation: ``{ok, notes, problems}`` (PatternCompiler.h)."""
    return dict(_native.mini.compile(text, cycle_start, cycle_length, cycle_index, seed))


@_ffi
def mini_realize(
    text: str, from_tick: int, to_tick: int, cycle_length: int
) -> npt.NDArray[np.int64]:
    """Rows of (start, length, pitch, velocity, span_start, span_length, cycle, index)."""
    return cast(npt.NDArray[np.int64], _native.mini.realize(text, from_tick, to_tick, cycle_length))


@_ffi
def mini_cycle_length(project: Project) -> int:
    """One cycle in ticks: a bar of the meter at tick 0."""
    return int(_native.mini.cycle_length(project.native))


@_ffi
def mini_sources(project: Project) -> list[tuple[str, str, list[str]]]:
    """Every MINI block as (pattern, channel, lines)."""
    return [
        (str(p), str(c), [str(x) for x in lines])
        for p, c, lines in _native.mini.sources(project.native)
    ]


@_ffi
def mini_join(lines: list[str]) -> str:
    """The text a MINI block's lines compile as."""
    return str(_native.mini.join_lines(lines))


@_ffi
def script_position(project: Project, text: str) -> int:
    """Ticks of a position in the file's spelling (raises ValueError)."""
    return int(_native.script.position(project.native, text))


@_ffi
def script_duration(project: Project, text: str) -> int:
    """Ticks of a duration in the file's spelling (raises ValueError)."""
    return int(_native.script.duration(project.native, text))


@_ffi
def script_mini_notes(text: str, length: int, cycle: int) -> npt.NDArray[np.int64]:
    """Rows of (start, length, pitch, velocity) ``text`` plays over ``length`` ticks."""
    return cast(npt.NDArray[np.int64], _native.script.mini_notes(text, length, cycle))


@_ffi
def script_add_mini_notes(project: Project, pattern: str, channel: str, text: str) -> int:
    """Compile ``text`` into notes in (pattern, channel): one command."""
    return int(_native.script.add_mini_notes(project.native, pattern, channel, text))


@_ffi
def script_set_mini(project: Project, pattern: str, channel: str, text: str) -> None:
    """Set (or with empty text remove) a live MINI block: one command."""
    _native.script.set_mini(project.native, pattern, channel, text)


@_ffi
def script_add_effect(
    project: Project, insert: int, effect: str, params: dict[str, float], mix: float = 1.0
) -> int:
    """Add an effect with its parameters: one command. Returns the slot id."""
    return int(_native.script.add_effect(project.native, insert, effect, params, mix))


# --- export, MIDI files, bundles, freeze (phase_8.md) ---------------------------------

#: The export formats ``export_project`` takes, as the CLI and the dialog spell them.
EXPORT_FORMATS: tuple[str, ...] = ("wav16", "wav24", "wav32f", "mp3", "flac", "ogg")


@_ffi
def export_project(project: Project, request: dict[str, Any]) -> dict[str, Any]:
    """Export as ``request`` describes and wait (keys: bindings/export.cpp). The result
    has ok, cancelled, error, files (path, label, frames, sample_rate, peak, rms,
    hash), gain_db and loudness."""
    return dict(_native.export_project(project.native, request))


class ExportJob:
    """An export on its own thread with its own copy of the project."""

    def __init__(self, project: Project, request: dict[str, Any]) -> None:
        CALLS.count += 1
        self.native = _native.ExportJob(project.native, request)

    @property
    @_ffi
    def progress(self) -> float:
        """0 to 1."""
        return float(self.native.progress)

    @property
    @_ffi
    def done(self) -> bool:
        """Finished, failed or cancelled."""
        return bool(self.native.done)

    @_ffi
    def cancel(self) -> None:
        """Ask the job to stop; result() then reports cancelled."""
        self.native.cancel()

    @_ffi
    def result(self) -> dict[str, Any]:
        """Wait, then what export_project would have returned."""
        return dict(self.native.result())


@_ffi
def import_midi(project: Project, path: str) -> dict[str, Any]:
    """A Standard MIDI File into ``project``, one undo step: pattern, tracks, division."""
    return dict(_native.import_midi(project.native, path))


@_ffi
def export_midi(project: Project, path: str) -> None:
    """The arrangement as a type-1 Standard MIDI File."""
    _native.export_midi(project.native, path)


@_ffi
def bundle_project(
    path: str, destination: str, zip_it: bool = False, library: list[str] | None = None
) -> dict[str, Any]:
    """Collect-and-save: ok, error, project, zip, copied, missing, rewritten."""
    return dict(_native.bundle_project(path, destination, zip_it, library or []))


@_ffi
def freeze_channel(project: Project, channel: int) -> dict[str, Any]:
    """Render a channel to audio and play that in its place, one undo step."""
    return dict(_native.freeze_channel(project.native, channel))


@_ffi
def unfreeze_channel(project: Project, channel: int) -> None:
    """Put a frozen channel's instrument and effects back."""
    _native.unfreeze_channel(project.native, channel)


@_ffi
def estimate_bpm(path: str) -> float | None:
    """An audio file's tempo, or None when it has no steady pulse."""
    bpm = _native.estimate_bpm(path)
    return None if bpm is None else float(bpm)


@_ffi
def detect_onsets(path: str) -> npt.NDArray[np.float64]:
    """Onset times of an audio file, in seconds."""
    return cast(npt.NDArray[np.float64], _native.detect_onsets(path))


# --- analysis (phase_10.md 4.1) --------------------------------------------------------

#: What AnalysisJob runs (bindings/analysis.cpp).
ANALYSIS_KINDS: tuple[str, ...] = ("key", "chords", "bpm", "timbre", "melody", "form")


class AnalysisJob:
    """One analysis on its own thread: ``kind`` is one of ANALYSIS_KINDS and ``params``
    its inputs (bindings/analysis.cpp). Nothing it does touches a project; the caller
    applies the result."""

    def __init__(self, kind: str, params: dict[str, Any]) -> None:
        CALLS.count += 1
        self.kind = kind
        self.native = _native.AnalysisJob(kind, params)

    @property
    @_ffi
    def progress(self) -> float:
        """0 to 1."""
        return float(self.native.progress)

    @property
    @_ffi
    def done(self) -> bool:
        """Finished, failed or cancelled."""
        return bool(self.native.done)

    @_ffi
    def cancel(self) -> None:
        """Ask the job to stop; result() then reports cancelled."""
        self.native.cancel()

    @_ffi
    def result(self) -> dict[str, Any]:
        """Wait; then ok, cancelled, error and the kind's own fields."""
        return dict(self.native.result())


@_ffi
def analyze(kind: str, params: dict[str, Any]) -> dict[str, Any]:
    """AnalysisJob(kind, params).result(), waited for in one call."""
    return dict(_native.analyze(kind, params))


@_ffi
def melody_extractor_available() -> bool:
    """True when this build has the ONNX melody extractor."""
    return bool(_native.melody_extractor_available())


@_ffi
def suggest_chords(key: str, previous: str = "") -> list[dict[str, Any]]:
    """Ranked next chords in ``key`` ("A minor") after ``previous`` ("F"), best first."""
    return [dict(s) for s in _native.suggest_chords(key, previous)]


@_ffi
def scale_of(key: str) -> list[int]:
    """The key's seven pitch classes, tonic first."""
    return [int(c) for c in _native.scale_of(key)]


@_ffi
def measure_loudness(audio: npt.NDArray[np.float32], rate: int) -> dict[str, float]:
    """BS.1770-4 integrated, max momentary and max short-term LUFS of (N, 2) audio."""
    return {k: float(v) for k, v in _native.measure_loudness(audio, rate).items()}


# --- the genre suites (phase_10.md 4.6) ------------------------------------------------


@_ffi
def genre_styles() -> list[dict[str, Any]]:
    """id, name, description and pack of every suite."""
    return [dict(s) for s in _native.genre_styles()]


@_ffi
def generate_track(
    style: str, packs: str, seed: int, bars: int, bpm: float | None, key: str | None
) -> dict[str, Any]:
    """A whole track's text, bpm, key and notes (bindings/generate.cpp)."""
    return dict(_native.generate_track(style, packs, seed, bars, bpm, key))


@_ffi
def style_template(
    style: str, packs: str, seed: int, bars: int, bpm: float | None, key: str | None
) -> dict[str, Any]:
    """The same without notes."""
    return dict(_native.style_template(style, packs, seed, bars, bpm, key))


# --- recording (phase_8.md 3.1) -------------------------------------------------------


class RecordSession:
    """One take on a playlist track. ``settings``: track, from, punch_in, punch_out,
    count_in_bars, click, monitor, project_directory, name, round_trip."""

    def __init__(self, engine: Engine, project: Project, settings: dict[str, Any]) -> None:
        CALLS.count += 1
        self.native = _native.RecordSession(engine.native, project.native, settings)

    @_ffi
    def start(self) -> None:
        """Arm, count in and roll (raises RuntimeError when the take cannot start)."""
        self.native.start()

    @_ffi
    def stop(self) -> dict[str, Any]:
        """Stop and place the take: ok, error, item, file, frames, overran, latency, lane."""
        return dict(self.native.stop())

    @property
    @_ffi
    def latency(self) -> dict[str, int]:
        """input, output, graph, measured and compensation, in samples."""
        return {str(k): int(v) if v is not None else -1 for k, v in self.native.latency.items()}

    @property
    @_ffi
    def frames(self) -> int:
        """Frames captured so far."""
        return int(self.native.frames)

    @property
    @_ffi
    def overruns(self) -> int:
        """Samples the writer fell behind by; anything above 0 marks the take."""
        return int(self.native.overruns)


class Calibration:
    """Loopback calibration: output wired to input, begin(), run the engine, finish()."""

    def __init__(self, engine: Engine) -> None:
        CALLS.count += 1
        self.native = _native.Calibration(engine.native)

    @_ffi
    def begin(self, project: Project, ping_at: int = 4800) -> None:
        """Send the ping ``ping_at`` samples from now."""
        self.native.begin(project.native, ping_at)

    @_ffi
    def finish(self) -> int | None:
        """The measured round trip in samples, or None when nothing came back."""
        value = self.native.finish()
        return None if value is None else int(value)


@_ffi
def comp_takes(project: Project, track: int, ranges: list[tuple[int, int, int]]) -> None:
    """Build lane 0 of ``track`` from (from, to, take item) ranges, one undo step."""
    _native.comp_takes(project.native, track, ranges)


# --- hosted plugins (phase_9.md 4.2-4.6) -------------------------------------------------


class Plugins:
    """The plugin host: scanning, the cache, live instances. Every method is one call;
    the ones that talk to a sandbox process release the GIL in the engine."""

    @staticmethod
    @_ffi
    def host_path() -> str:
        return str(_native.plugin_host_path())

    @staticmethod
    @_ffi
    def set_host(path: str) -> None:
        _native.set_plugin_host(path)

    @staticmethod
    @_ffi
    def available() -> bool:
        """adx_plugin_host.exe is where the engine looks for it."""
        return bool(_native.plugin_host_available())

    @staticmethod
    @_ffi
    def load_cache(path: str = "") -> tuple[bool, str]:
        ok, error = _native.plugin_load_cache(path)
        return bool(ok), str(error)

    @staticmethod
    @_ffi
    def scan(directories: list[str] | None = None, force: bool = False) -> int:
        """Scan out of process into the cache; how many files were scanned."""
        return int(_native.plugin_scan(directories or [], force))

    @staticmethod
    @_ffi
    def rescan(format_name: str, path: str) -> dict[str, Any]:
        return dict(_native.plugin_rescan(format_name, path))

    @staticmethod
    @_ffi
    def forget(path: str) -> bool:
        return bool(_native.plugin_forget(path))

    @staticmethod
    @_ffi
    def listing() -> list[dict[str, Any]]:
        """Every plugin the cache knows that scanned cleanly."""
        return [dict(d) for d in _native.plugin_list()]

    @staticmethod
    @_ffi
    def scan_results() -> list[dict[str, Any]]:
        """Every scanned file, failed ones (the blacklist) included."""
        return [dict(d) for d in _native.plugin_scan_results()]

    @staticmethod
    @_ffi
    def describe(format_name: str, uid: str, path: str = "") -> dict[str, Any] | None:
        found = _native.plugin_describe(format_name, uid, path)
        return None if found is None else dict(found)

    @staticmethod
    @_ffi
    def notices() -> list[dict[str, Any]]:
        """Crashes, hangs, mutes and latency changes since the last call."""
        return [dict(d) for d in _native.plugin_notices()]

    @staticmethod
    @_ffi
    def restart(instance: int = 0) -> int:
        return int(_native.plugin_restart(instance))

    @staticmethod
    @_ffi
    def instances() -> list[dict[str, Any]]:
        return [dict(d) for d in _native.plugin_instances()]

    @staticmethod
    @_ffi
    def param_values(instance: int) -> list[tuple[int, float]]:
        return [(int(i), float(v)) for i, v in _native.plugin_param_values(instance)]

    @staticmethod
    @_ffi
    def param_text(instance: int, param: int, value: float) -> str | None:
        text = _native.plugin_param_text(instance, param, value)
        return None if text is None else str(text)

    @staticmethod
    @_ffi
    def open_editor(instance: int) -> tuple[int, int, int] | None:
        opened = _native.plugin_open_editor(instance)
        return None if opened is None else (int(opened[0]), int(opened[1]), int(opened[2]))

    @staticmethod
    @_ffi
    def close_editor(instance: int) -> None:
        _native.plugin_close_editor(instance)

    @staticmethod
    @_ffi
    def reenable(instance: int) -> None:
        _native.plugin_reenable(instance)

    @staticmethod
    @_ffi
    def set_isolated(instance: int, isolated: bool) -> None:
        _native.plugin_set_isolated(instance, isolated)

    @staticmethod
    @_ffi
    def check_latency(instance: int) -> int:
        return int(_native.plugin_check_latency(instance))

    @staticmethod
    @_ffi
    def search_paths() -> list[str]:
        return [str(p) for p in _native.plugin_search_paths()]
