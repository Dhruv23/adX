"""The ``adx`` scripting module (phase_7.md 4.7; docs/scripting.md).

In the console and in scripts it is simply ``adx``::

    p = adx.project()
    with p.undo_group("build a riff"):
        lead = p.add_channel("Lead", instrument="va")
        verse = p.add_pattern("Verse", bars=8)
        verse.notes(lead).add_mini("0 3 5 7 | scale=a_minor | fast 2")
        p.playlist.track(1).place(verse, at="0:0:0")
        p.mixer.insert(1).add_effect("Reverb", mix=0.3)
    adx.transport.play()
    adx.on_bar(lambda bar: print("bar", bar))

The rules (phase_7.md 4.7), and how they are kept:

1. **The API issues commands.** Every mutation below is a command builder the panels
   use (``engine_bridge.commands``) or one of the few in ``engine/script/ScriptApi.h``,
   which are commands too. There is no privileged path into the model.
2. **Main thread only.** ``on_bar`` callbacks run from a Qt timer on the main thread,
   reading the transport's position - never from the audio callback.
3. **Every action is undoable**; ``undo_group`` makes a block of them one step.
4. Inside the app, edits go through the app's :class:`~adx.session.Session`, so every
   panel refreshes and the audio thread hears them, exactly as if made by hand.

Positions and durations are written as the file writes them: ``"bar:beat:tick"`` from
``"0:0:0"``, and ``"2:0:0"`` is two bars long.
"""

from __future__ import annotations

import contextlib
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from typing import Any

import numpy as np
from PySide6.QtCore import QObject, QTimer, SignalInstance

from adx import engine_bridge
from adx.engine_bridge import PPQ, Engine, Native, Project
from adx.session import Session

__all__ = [
    "ChannelRef",
    "Insert",
    "Mixer",
    "Notes",
    "PatternRef",
    "Playlist",
    "ProjectApi",
    "Track",
    "attach",
    "new",
    "off_bar",
    "on_bar",
    "open",
    "project",
    "transport",
]


class ScriptError(RuntimeError):
    """A script asked for something that is not there."""


class _Host:
    """Where commands go: the app's session, or a project of the script's own."""

    def __init__(
        self,
        project: Project | None = None,
        session: Session | None = None,
        engine: Engine | None = None,
    ) -> None:
        self._project = project
        self.session = session
        self.engine = engine

    @property
    def project(self) -> Project:
        if self.session is not None:
            current: Project = self.session.require()
            return current
        if self._project is None:
            raise ScriptError("no project is open")
        return self._project

    def execute(self, command: Native) -> None:
        if self.session is not None:
            if not self.session.execute(command):
                raise ScriptError("the engine refused the command (see the status bar)")
            return
        self.project.execute(command)

    def changed(self) -> None:
        """A ScriptApi function executed on the project directly: tell everyone."""
        if self.session is not None:
            self.session.notify()

    @contextmanager
    def group(self, label: str) -> Iterator[None]:
        if self.session is not None:
            with self.session.group(label):
                yield
            return
        project = self.project
        project.begin_group(label)
        try:
            yield
        finally:
            project.end_group()


_host: _Host | None = None


def _require() -> _Host:
    if _host is None:
        raise ScriptError("adx is not attached to a project: use adx.new() or adx.open()")
    return _host


def attach(session: Session, engine: Engine | None = None) -> None:
    """Bind the module to the app's session (the app does this at start-up)."""
    global _host
    _host = _Host(session=session, engine=engine)


def _own_engine() -> Engine | None:
    """Outside the app, a script keeps the engine it made for itself - never the app's,
    and it stops following the app's frame clock (the window may be gone)."""
    if _bars is not None:
        _bars.unfollow()
    if _host is not None and _host.session is None:
        return _host.engine
    return None


def new(text: str | None = None) -> ProjectApi:
    """A project of the script's own - for running outside the app (and in the tests).
    ``text`` is .adx text; the default is one master insert and nothing else."""
    global _host
    source = text or '[PROJECT]\nADX_VERSION=2\n\n[MIXER]\nINSERT 1 name="Master"\n'
    loaded, diagnostics = Project.loads(source)
    errors = [d for d in diagnostics if str(d["severity"]) == "error"]
    if errors:
        raise ScriptError(f"line {errors[0]['line']}: {errors[0]['message']}")
    _host = _Host(project=loaded, engine=_own_engine())
    return ProjectApi(_host)


def open(path: str) -> ProjectApi:  # noqa: A001 - adx.open reads as it should
    """Open a .adx file as the script's project (outside the app)."""
    global _host
    loaded, _diagnostics = Project.load(path)
    _host = _Host(project=loaded, engine=_own_engine())
    return ProjectApi(_host)


def project() -> ProjectApi:
    """The open project."""
    return ProjectApi(_require())


# --- the project --------------------------------------------------------------------


class ProjectApi:
    """The open project, as a script sees it."""

    def __init__(self, host: _Host) -> None:
        self._host = host
        self.playlist = Playlist(host)
        self.mixer = Mixer(host)

    @property
    def native(self) -> Project:
        """The bridge's Project, for anything this module does not wrap."""
        return self._host.project

    def text(self) -> str:
        """The project's canonical .adx text."""
        return self.native.dumps()

    @contextmanager
    def undo_group(self, label: str) -> Iterator[None]:
        """Everything inside is one undo step."""
        with self._host.group(label):
            yield

    def undo(self) -> bool:
        """Undo one step."""
        if self._host.session is not None:
            return bool(self._host.session.undo())
        return self.native.undo()

    def redo(self) -> bool:
        """Redo one step."""
        if self._host.session is not None:
            return bool(self._host.session.redo())
        return self.native.redo()

    def position(self, text: str) -> int:
        """Ticks of a position in the file's spelling."""
        return engine_bridge.script_position(self.native, text)

    def duration(self, text: str) -> int:
        """Ticks of a duration in the file's spelling."""
        return engine_bridge.script_duration(self.native, text)

    # --- channels and patterns ---

    def channels(self) -> list[str]:
        """Channel names."""
        return self.native.channels()

    def channel(self, name: str) -> ChannelRef:
        """An existing channel."""
        if name not in self.native.channels():
            raise ScriptError(f"no channel named {name!r}")
        return ChannelRef(self._host, name)

    def add_channel(
        self,
        name: str,
        instrument: str = "additive",
        preset: str | None = None,
        output: int = 0,
    ) -> ChannelRef:
        """A new channel (``output`` 0: the master). ``preset`` is a preset file's path."""
        with self._host.group(f"Add channel {name}"):
            self._host.execute(
                engine_bridge.commands.build("AddChannelNew", name, instrument, output)
            )
            ref = ChannelRef(self._host, name)
            if preset is not None:
                self.native.apply_preset(ref.id, preset)
                self._host.changed()
        return ref

    def patterns(self) -> list[str]:
        """Pattern names."""
        return self.native.patterns()

    def pattern(self, name: str) -> PatternRef:
        """An existing pattern."""
        if name not in self.native.patterns():
            raise ScriptError(f"no pattern named {name!r}")
        return PatternRef(self._host, name)

    def add_pattern(self, name: str, bars: int = 4, length: str | None = None) -> PatternRef:
        """A new pattern ``bars`` bars long (of the meter at tick 0), or ``length`` long."""
        ticks = (
            self.duration(length)
            if length is not None
            else bars * engine_bridge.mini_cycle_length(self.native)
        )
        self._host.execute(engine_bridge.commands.build("AddPatternNew", name, ticks, ""))
        return PatternRef(self._host, name)

    def set_tempo(self, bpm: float, at: str = "0:0:0") -> None:
        """A tempo change."""
        self._host.execute(engine_bridge.commands.set_tempo(bpm, self.position(at)))

    def add_marker(self, name: str, at: str = "0:0:0") -> None:
        """A marker on the ruler."""
        self._host.execute(engine_bridge.commands.add_marker(self.position(at), name))


class ChannelRef:
    """One channel, by name."""

    def __init__(self, host: _Host, name: str) -> None:
        self._host = host
        self.name = name

    def _info(self) -> dict[str, Any]:
        for info in self._host.project.channels_info():
            if info["name"] == self.name:
                return info
        raise ScriptError(f"no channel named {self.name!r}")

    @property
    def id(self) -> int:
        """The channel's id."""
        return int(self._info()["id"])

    def info(self) -> dict[str, Any]:
        """Everything the inspector shows about it."""
        return self._info()

    def set(self, **fields: float) -> None:
        """Strip values: ``volume``, ``pan``, ``pitch`` (cents), ``mute``, ``solo``,
        ``polyphony``. One undo step."""
        with self._host.group(f"Set {self.name}"):
            for field, value in fields.items():
                self._host.execute(
                    engine_bridge.commands.build("SetChannelField", self.id, field, float(value))
                )

    def param(self, name: str, value: float) -> None:
        """An instrument parameter: ``ch.param("filter.cutoff", 1200)``."""
        self._host.execute(
            engine_bridge.commands.build("SetChannelParam", self.id, name, float(value))
        )

    def instrument(self, kind: str) -> None:
        """Change the instrument (its voices release; new notes play the new one)."""
        self._host.execute(engine_bridge.commands.build("SetInstrumentType", self.id, kind))

    def route(self, insert: int) -> None:
        """Send this channel to a mixer insert."""
        self._host.execute(engine_bridge.commands.build("SetChannelOutput", self.id, insert))


def _channel_name(channel: ChannelRef | str) -> str:
    return channel.name if isinstance(channel, ChannelRef) else channel


class PatternRef:
    """One pattern, by name."""

    def __init__(self, host: _Host, name: str) -> None:
        self._host = host
        self.name = name

    @property
    def id(self) -> int:
        """The pattern's id."""
        for info in self._host.project.patterns_info():
            if info["name"] == self.name:
                return int(info["id"])
        raise ScriptError(f"no pattern named {self.name!r}")

    @property
    def length(self) -> int:
        """Length in ticks."""
        return self._host.project.pattern_length(self.name)

    def notes(self, channel: ChannelRef | str) -> Notes:
        """The notes ``channel`` plays in this pattern."""
        return Notes(self._host, self.name, _channel_name(channel))

    def mini(self, channel: ChannelRef | str, text: str) -> None:
        """A live mini-notation block for ``channel``: it plays the song's cycles and
        follows every edit (empty text removes it)."""
        engine_bridge.script_set_mini(self._host.project, self.name, _channel_name(channel), text)
        self._host.changed()


class Notes:
    """One channel's notes in one pattern."""

    def __init__(self, host: _Host, pattern: str, channel: str) -> None:
        self._host = host
        self.pattern = pattern
        self.channel = channel

    def count(self) -> int:
        """How many there are."""
        return len(self._host.project.notes(self.pattern, self.channel))

    def add(
        self, pitch: int, at: str | int, length: str | int = "0:1:0", velocity: int = 100
    ) -> None:
        """One note: ``notes.add(60, at="0:0:0", length="0:1:0")``. A position or length
        may also be a number of ticks."""
        project = self._host.project
        row = engine_bridge.empty_notes(1)
        row["start"] = at if isinstance(at, int) else engine_bridge.script_position(project, at)
        row["length"] = (
            length if isinstance(length, int) else engine_bridge.script_duration(project, length)
        )
        row["pitch"] = pitch
        row["velocity"] = velocity
        self._host.execute(
            engine_bridge.commands.edit_notes(
                self.pattern,
                self.channel,
                np.zeros(0, dtype=np.uint32),
                engine_bridge.empty_notes(0),
                row,
                "Add note",
            )
        )

    def add_mini(self, text: str) -> int:
        """Compile mini-notation into ordinary notes across the pattern - its cycles
        counted from the pattern's start, a bar each. Returns how many it added."""
        added = engine_bridge.script_add_mini_notes(
            self._host.project, self.pattern, self.channel, text
        )
        self._host.changed()
        return added

    def clear(self) -> None:
        """Remove them all."""
        notes = self._host.project.notes(self.pattern, self.channel)
        if len(notes) == 0:
            return
        self._host.execute(
            engine_bridge.commands.edit_notes(
                self.pattern,
                self.channel,
                notes["id"].astype(np.uint32),
                engine_bridge.empty_notes(0),
                engine_bridge.empty_notes(0),
                "Clear notes",
            )
        )


# --- the playlist and the mixer -------------------------------------------------------


class Playlist:
    """The arrangement."""

    def __init__(self, host: _Host) -> None:
        self._host = host

    def track(self, number: int) -> Track:
        """Track ``number`` (1-based, as the file numbers them), created if missing."""
        tracks = self._host.project.playlist_info()["tracks"]
        ids = [int(t["id"]) for t in tracks]
        while number not in ids:
            self._host.execute(engine_bridge.commands.build("AddPlaylistTrackNew", ""))
            before = len(ids)
            ids = [int(t["id"]) for t in self._host.project.playlist_info()["tracks"]]
            # A refused command adds nothing, and would loop here forever.
            if len(ids) <= before or len(ids) > number + 64:
                raise ScriptError(f"could not create track {number}")
        return Track(self._host, number)


class Track:
    """One playlist track."""

    def __init__(self, host: _Host, track_id: int) -> None:
        self._host = host
        self.id = track_id

    def place(
        self, pattern: PatternRef | str, at: str = "0:0:0", length: str | None = None
    ) -> None:
        """Place a pattern on this track."""
        ref = pattern if isinstance(pattern, PatternRef) else PatternRef(self._host, pattern)
        project = self._host.project
        ticks = engine_bridge.script_duration(project, length) if length else 0
        self._host.execute(
            engine_bridge.commands.build(
                "AddPatternItem", self.id, ref.id, engine_bridge.script_position(project, at), ticks
            )
        )


class Mixer:
    """The mixer."""

    def __init__(self, host: _Host) -> None:
        self._host = host

    def insert(self, number: int) -> Insert:
        """An existing insert, by the number the file gives it."""
        inserts = self._host.project.mixer_info()["inserts"]
        if all(int(i["id"]) != number for i in inserts):
            raise ScriptError(f"no insert {number}")
        return Insert(self._host, number)

    def add_insert(self, name: str, to: int = 0) -> Insert:
        """A new insert routed to insert ``to`` (0: the master). One undo step."""
        project = self._host.project
        before = {int(i["id"]) for i in project.mixer_info()["inserts"]}
        with self._host.group(f"Add insert {name}"):
            self._host.execute(engine_bridge.commands.build("AddInsertNew", name))
            info = project.mixer_info()
            created = max(int(i["id"]) for i in info["inserts"] if int(i["id"]) not in before)
            target = to or int(info["master"])
            self._host.execute(engine_bridge.commands.build("AddRoute", created, target))
        return Insert(self._host, created)


class Insert:
    """One mixer insert."""

    def __init__(self, host: _Host, insert_id: int) -> None:
        self._host = host
        self.id = insert_id

    def set(self, **fields: float) -> None:
        """Strip values: ``gain``, ``pan``, ``width``, ``mute``, ``solo``, ``invert``."""
        with self._host.group(f"Set insert {self.id}"):
            for field, value in fields.items():
                self._host.execute(
                    engine_bridge.commands.build("SetInsertField", self.id, field, float(value))
                )

    def add_effect(self, effect: str, mix: float = 1.0, **params: float) -> int:
        """An effect at the end of the chain, its parameters set: one undo step.
        Returns the slot id."""
        slot = engine_bridge.script_add_effect(
            self._host.project, self.id, effect, {k: float(v) for k, v in params.items()}, mix
        )
        self._host.changed()
        return slot


# --- the transport and bar callbacks --------------------------------------------------


class Transport:
    """Play, stop, seek - the app's transport, or an engine of the script's own."""

    def _engine(self) -> Engine:
        host = _require()
        if host.engine is None:
            host.engine = Engine(null_backend=True)
            host.engine.start()
            host.engine.set_project(host.project)
        return host.engine

    def play(self) -> None:
        """Start playing from the playhead."""
        engine = self._engine()
        engine.commit(_require().project)
        engine.play()

    def stop(self) -> None:
        """Stop where the playhead is."""
        self._engine().stop_playback()

    def seek(self, at: str) -> None:
        """Move the playhead."""
        self._engine().seek(engine_bridge.script_position(_require().project, at))

    def position(self) -> int:
        """The playhead, in ticks."""
        return self._engine().position_ticks()


transport = Transport()


class _BarClock(QObject):
    """Calls the registered callbacks when the playhead enters a new bar.

    Main thread only: a Qt timer reads the transport's position (the app's 60 Hz frame
    clock when there is one, so a bar callback costs no engine call of its own) and
    compares bars. Never the audio callback (phase_7.md 4.7, rule 2).
    """

    def __init__(self) -> None:
        super().__init__()
        self.callbacks: list[Callable[[int], object]] = []
        self._bar = -1
        self._bar_ticks = PPQ * 4
        self._timer = QTimer(self)
        self._timer.setInterval(16)
        self._timer.timeout.connect(self._poll)
        self._frames: Any = None

    def follow(self, frame_signal: SignalInstance) -> None:
        """Use the app's frame clock instead of a timer of our own."""
        self.unfollow()
        self._frames = frame_signal
        frame_signal.connect(self.at)
        self._timer.stop()

    def unfollow(self) -> None:
        """Back to a timer of our own."""
        if self._frames is not None:
            with contextlib.suppress(RuntimeError, TypeError):
                self._frames.disconnect(self.at)
            self._frames = None

    def start(self) -> None:
        host = _require()
        self._bar_ticks = engine_bridge.mini_cycle_length(host.project)
        if self._frames is None and not self._timer.isActive():
            self._timer.start()

    def _poll(self) -> None:
        if self.callbacks:
            self.at(transport.position(), True)

    def at(self, ticks: int, rolling: bool) -> None:
        """The playhead is at ``ticks``: fire on a new bar."""
        if not rolling or not self.callbacks:
            return
        bar = int(ticks) // max(1, self._bar_ticks)
        if bar == self._bar:
            return
        self._bar = bar
        for callback in list(self.callbacks):
            callback(bar)


_bars: _BarClock | None = None


def _bar_clock() -> _BarClock:
    global _bars
    if _bars is None:
        _bars = _BarClock()
    return _bars


def follow_frames(frame_signal: SignalInstance) -> None:
    """The app's 60 Hz frame signal drives bar callbacks (called by the main window)."""
    _bar_clock().follow(frame_signal)


def on_bar(callback: Callable[[int], object]) -> Callable[[int], object]:
    """Call ``callback(bar)`` each time playback enters a bar (0-based), on the main
    thread. Returns the callback, so it can be used as a decorator."""
    clock = _bar_clock()
    clock.callbacks.append(callback)
    clock.start()
    return callback


def off_bar(callback: Callable[[int], object]) -> None:
    """Stop calling ``callback``."""
    clock = _bar_clock()
    if callback in clock.callbacks:
        clock.callbacks.remove(callback)
