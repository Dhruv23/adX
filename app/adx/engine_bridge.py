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
from typing import Any, Protocol, cast

import numpy as np
import numpy.typing as npt
from PySide6.QtCore import QObject, QTimer, Signal

import adx_engine as _native

__all__ = [
    "CALLS",
    "COLOR_ROLES",
    "NOTE_DTYPE",
    "PPQ",
    "Engine",
    "FrameClock",
    "GeometryView",
    "PeakCache",
    "Peaks",
    "PianoRollGeometry",
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

    def __init__(self, null_backend: bool = False, sample_rate: int = 48000) -> None:
        CALLS.count += 1
        self.native = _native.Engine(sample_rate=sample_rate, null_backend=null_backend)
        self.null_backend = null_backend
        self._sample_rate = sample_rate

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


class FrameClock(QObject):
    """The 60 Hz UI frame: one :meth:`Engine.frame` call per tick (phase_5.md 4.8).

    Emits ``frame(position_ticks, rolling)`` and keeps the latest meter rows in
    :attr:`levels`, a buffer allocated once and refilled in place. With :attr:`meters`
    set (the mixer sets it), the same one call also builds every strip's meter vertices,
    and :attr:`meter_upload` holds (address, vertex count, revision) for this frame's
    upload - which must happen before any other engine call (phase_6.md 4.3).
    """

    frame = Signal(int, bool)

    def __init__(
        self, engine: Engine, interval_ms: int = 16, parent: QObject | None = None
    ) -> None:
        super().__init__(parent)
        self._engine = engine
        self.levels: npt.NDArray[np.float32] = np.zeros((32, LEVEL_COLUMNS), dtype=np.float32)
        self.strips = 0
        self.position_ticks = 0
        self.position_seconds = 0.0
        self.rolling = False
        self.meters: MeterGeometry | None = None
        self.meter_upload: tuple[int, int, int] = (0, 0, 0)
        self._timer = QTimer(self)
        self._timer.setInterval(interval_ms)
        self._timer.timeout.connect(self.tick)

    def start(self) -> None:
        """Start ticking."""
        self._timer.start()

    def stop(self) -> None:
        """Stop ticking."""
        self._timer.stop()

    def tick(self) -> None:
        """One frame: exactly one engine call."""
        if self.meters is not None:
            ticks, rolling, strips, samples, address, count, revision = (
                self._engine.frame_with_meters(self.levels, self.meters)
            )
            self.meter_upload = (address, count, revision)
        else:
            ticks, rolling, strips, samples = self._engine.frame(self.levels)
        if strips > self.levels.shape[0]:
            # Too small: grow once (no engine call); the next frame fills every strip.
            self.levels = np.zeros((strips * 2, LEVEL_COLUMNS), dtype=np.float32)
        self.strips = min(strips, self.levels.shape[0])
        self.position_ticks = ticks
        self.position_seconds = samples / self._engine.sample_rate
        self.rolling = rolling
        self.frame.emit(ticks, rolling)


class ErrorChannel(QObject):
    """Engine errors as a Qt signal, so a panel never has to catch them itself."""

    error = Signal(str, str)

    def report(self, context: str, error: BaseException) -> None:
        """Turn ``error`` into an ``error(context, message)`` signal."""
        self.error.emit(context, str(error))


errors = ErrorChannel()


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
