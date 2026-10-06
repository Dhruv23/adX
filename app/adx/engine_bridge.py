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


class FrameClock(QObject):
    """The 60 Hz UI frame: one :meth:`Engine.frame` call per tick (phase_5.md 4.8).

    Emits ``frame(position_ticks, rolling)`` and keeps the latest meter rows in
    :attr:`levels`, a buffer allocated once and refilled in place.
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
