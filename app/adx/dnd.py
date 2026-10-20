"""Drag-and-drop: payload types, declared once, and what each target does with them
(phase_6.md 4.6).

| Payload     | Valid drop targets                                                  |
|-------------|---------------------------------------------------------------------|
| sample file | playlist (an audio clip), channel rack (a sampler channel),          |
|             | sampler zone editor (one more zone)                                 |
| preset      | channel (replaces its instrument), effect slot                      |
| pattern     | playlist                                                            |
| effect type | mixer slot                                                          |

Every drop is one command - or one group, which is one undo step - through the session.
:func:`perform` returns the name of what it did, which is what ``browser_drag_creates_
command`` asserts for every payload x target pair.
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path

from PySide6.QtCore import QByteArray, QMimeData

from adx.session import Session

#: The MIME type every adX payload travels under.
MIME = "application/x-adx-payload"


@dataclass(frozen=True, slots=True)
class SampleFile:
    """An audio file."""

    path: str


@dataclass(frozen=True, slots=True)
class Preset:
    """A preset file; ``kind`` is "instrument" or "effect"."""

    path: str
    kind: str


@dataclass(frozen=True, slots=True)
class PatternRef:
    """A pattern of the open project."""

    pattern: int


@dataclass(frozen=True, slots=True)
class EffectType:
    """An effect, by catalog name."""

    name: str


Payload = SampleFile | Preset | PatternRef | EffectType

_TYPES: dict[str, type[SampleFile] | type[Preset] | type[PatternRef] | type[EffectType]] = {
    "sample": SampleFile,
    "preset": Preset,
    "pattern": PatternRef,
    "effect": EffectType,
}


@dataclass(frozen=True, slots=True)
class PlaylistTarget:
    """A tick on a playlist track."""

    track: int
    tick: int


@dataclass(frozen=True, slots=True)
class RackTarget:
    """The channel rack's empty space: a new channel."""


@dataclass(frozen=True, slots=True)
class ChannelTarget:
    """An existing channel."""

    channel: int


@dataclass(frozen=True, slots=True)
class ZoneTarget:
    """A sampler channel's zone editor."""

    channel: int


@dataclass(frozen=True, slots=True)
class SlotTarget:
    """A mixer slot; ``slot`` 0 is the empty slot at the end of the insert's chain."""

    insert: int
    slot: int


Target = PlaylistTarget | RackTarget | ChannelTarget | ZoneTarget | SlotTarget


def encode(payload: Payload) -> QMimeData:
    """A payload as MIME data (plus a URL for a file, so other apps understand it)."""
    name = next(key for key, kind in _TYPES.items() if isinstance(payload, kind))
    data = QMimeData()
    data.setData(MIME, QByteArray(json.dumps({"type": name, **asdict(payload)}).encode()))
    if isinstance(payload, SampleFile | Preset):
        from PySide6.QtCore import QUrl

        data.setUrls([QUrl.fromLocalFile(payload.path)])
    return data


def decode(data: QMimeData) -> Payload | None:
    """The payload in MIME data: ours, or a dropped audio/preset file from outside."""
    if data.hasFormat(MIME):
        fields = json.loads(bytes(data.data(MIME).data()).decode())
        kind = _TYPES.get(str(fields.pop("type", "")))
        return kind(**fields) if kind is not None else None
    if data.hasUrls():
        from adx.panels.browser.model import AUDIO_SUFFIXES

        for url in data.urls():
            path = url.toLocalFile()
            suffix = Path(path).suffix.lower()
            if suffix in AUDIO_SUFFIXES:
                return SampleFile(path)
            if suffix == ".adxpreset":
                return Preset(path, "instrument")
    return None


def accepts(payload: Payload, target: Target) -> bool:
    """Whether ``target`` takes ``payload`` (the table above)."""
    if isinstance(payload, SampleFile):
        return isinstance(target, PlaylistTarget | RackTarget | ZoneTarget)
    if isinstance(payload, Preset):
        if payload.kind == "effect":
            return isinstance(target, SlotTarget)
        return isinstance(target, ChannelTarget | RackTarget)
    if isinstance(payload, PatternRef):
        return isinstance(target, PlaylistTarget)
    return isinstance(target, SlotTarget)


def perform(session: Session, payload: Payload, target: Target) -> str:
    """Drop ``payload`` on ``target``: one undo step. Returns what was done, or "" when
    the target does not take it."""
    if not accepts(payload, target):
        return ""
    project = session.require()
    if isinstance(payload, SampleFile):
        if isinstance(target, PlaylistTarget):
            session.call(project.add_audio_item, target.track, payload.path, target.tick)
            return "add_audio_item"
        if isinstance(target, ZoneTarget):
            session.call(project.add_zone, target.channel, payload.path)
            return "add_zone"
        session.call(
            project.add_sampler_channel, _channel_name(session, payload.path), payload.path
        )
        return "add_sampler_channel"
    if isinstance(payload, Preset):
        if isinstance(target, SlotTarget):
            session.call(project.apply_effect_preset, target.insert, target.slot, payload.path)
            return "apply_effect_preset"
        if isinstance(target, RackTarget):
            name = _channel_name(session, Path(payload.path).stem)
            with session.group(f"Add {name}"):
                session.build_and_execute("AddChannelNew", name, "additive", 0, "")
                channel = int(project.channels_info()[-1]["id"])
                session.call(project.apply_preset, channel, payload.path)
            return "apply_preset"
        assert isinstance(target, ChannelTarget)
        session.call(project.apply_preset, target.channel, payload.path)
        return "apply_preset"
    if isinstance(payload, PatternRef):
        assert isinstance(target, PlaylistTarget)
        session.build_and_execute("AddPatternItem", target.track, payload.pattern, target.tick, 0)
        return "AddPatternItem"
    assert isinstance(target, SlotTarget)
    if target.slot:
        session.call(project.set_slot_effect, target.slot, payload.name)
        return "set_slot_effect"
    session.build_and_execute("AddSlot", target.insert, payload.name)
    return "AddSlot"


def _channel_name(session: Session, source: str) -> str:
    """A unique channel name from a file name: letters, digits and underscores."""
    stem = "".join(c if c.isalnum() else "_" for c in Path(source).stem).strip("_") or "Sample"
    if not stem[0].isalpha():
        stem = "S_" + stem
    taken = {str(c["name"]) for c in session.require().channels_info()}
    name, n = stem, 2
    while name in taken:
        name, n = f"{stem}_{n}", n + 1
    return name
