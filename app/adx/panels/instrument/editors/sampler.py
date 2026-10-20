"""Sampler, pool: the zone editor (phase_6.md 4.4's table: "needs a waveform with zone
and loop-point editing").

The zone list on the left; the selected zone's sample on the right, from the engine's
peak cache, with its start, end and loop points as markers you drag. Every edit replaces
the zone map in one SetChannelZones; a sample dropped here is one more zone (one step).
"""

from __future__ import annotations

from typing import Any

from PySide6.QtGui import QDragEnterEvent, QDropEvent
from PySide6.QtWidgets import QHBoxLayout, QListWidget, QSpinBox, QVBoxLayout, QWidget

from adx import dnd
from adx.engine_bridge import PeakCache
from adx.panels.editor_parts import TabbedEditor, WaveView
from adx.panels.instrument.editors.common import tabbed
from adx.session import Session

NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")


def note_name(key: int) -> str:
    """60 -> C5 (adX's octave numbering)."""
    return f"{NOTE_NAMES[key % 12]}{key // 12}"


class ZoneEditor(QWidget):
    """A sampler channel's zones: list, waveform, key range and root."""

    def __init__(self, session: Session, channel: int, cache: PeakCache) -> None:
        super().__init__()
        self.session = session
        self.channel = channel
        self.setAcceptDrops(True)
        layout = QHBoxLayout(self)
        left = QVBoxLayout()
        self.zone_list = QListWidget()
        self.zone_list.currentRowChanged.connect(lambda _r: self.refresh())
        left.addWidget(self.zone_list, 1)
        self.key_low = self._spin(left, "Low key", "key_low")
        self.key_high = self._spin(left, "High key", "key_high")
        self.root = self._spin(left, "Root", "root")
        layout.addLayout(left, 1)
        self.wave = WaveView(cache)
        self.wave.marker_moved.connect(self._marker_moved)
        layout.addWidget(self.wave, 3)
        self._shown_sample = ""
        self.refresh()

    def _spin(self, layout: QVBoxLayout, tip: str, field: str) -> QSpinBox:
        spin = QSpinBox()
        spin.setRange(0, 127)
        spin.setToolTip(tip)
        spin.setPrefix(f"{tip}: ")
        spin.setKeyboardTracking(False)
        spin.valueChanged.connect(lambda v: self._set_field(field, int(v)))
        layout.addWidget(spin)
        return spin

    def zones(self) -> list[dict[str, Any]]:
        """The channel's zones, read now."""
        return self.session.require().zones(self.channel)

    def refresh(self) -> None:
        """Re-read the zones and show the selected one."""
        if self.session.project is None:
            return
        zones = self.zones()
        labels = [
            f"{z['sample']}  {note_name(int(z['key_low']))}-{note_name(int(z['key_high']))}"
            for z in zones
        ]
        row = self.zone_list.currentRow()
        if [self.zone_list.item(i).text() for i in range(self.zone_list.count())] != labels:
            self.zone_list.blockSignals(True)
            self.zone_list.clear()
            self.zone_list.addItems(labels)
            self.zone_list.blockSignals(False)
            self.zone_list.setCurrentRow(min(max(row, 0), len(labels) - 1))
        row = self.zone_list.currentRow()
        if not 0 <= row < len(zones):
            return
        zone = zones[row]
        for spin, field in (
            (self.key_low, "key_low"),
            (self.key_high, "key_high"),
            (self.root, "root"),
        ):
            spin.blockSignals(True)
            spin.setValue(int(zone[field]))
            spin.blockSignals(False)
        samples = {path: full for _i, path, full in self.session.require().samples()}
        full = samples.get(str(zone["sample"]), "")
        if full != self._shown_sample:
            self._shown_sample = full
            self.wave.open(full)
        self.wave.markers = {"start": float(zone["start"])}
        if int(zone["end"]):
            self.wave.markers["end"] = float(zone["end"])
        if int(zone["loop"]):
            self.wave.markers["loop start"] = float(zone["loop_start"])
            if int(zone["loop_end"]):
                self.wave.markers["loop end"] = float(zone["loop_end"])
        self.wave.update()

    def _set_field(self, field: str, value: int) -> None:
        zones = self.zones()
        row = self.zone_list.currentRow()
        if 0 <= row < len(zones) and int(zones[row][field]) != value:
            zones[row][field] = value
            self.session.build_and_execute("SetChannelZones", self.channel, zones)

    def _marker_moved(self, name: str, frame: float) -> None:
        field = {
            "start": "start",
            "end": "end",
            "loop start": "loop_start",
            "loop end": "loop_end",
        }[name]
        self._set_field(field, round(frame))

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:  # noqa: N802 - Qt's name
        payload = dnd.decode(event.mimeData())
        if payload is not None and dnd.accepts(payload, dnd.ZoneTarget(self.channel)):
            event.acceptProposedAction()

    def dropEvent(self, event: QDropEvent) -> None:  # noqa: N802 - Qt's name
        payload = dnd.decode(event.mimeData())
        if payload is not None:
            dnd.perform(self.session, payload, dnd.ZoneTarget(self.channel))
            event.acceptProposedAction()


_CACHE: list[PeakCache] = []


def shared_cache() -> PeakCache:
    """One peak cache for every zone editor (it owns a worker thread)."""
    if not _CACHE:
        _CACHE.append(PeakCache())
    return _CACHE[0]


def sampler_editor(session: Session, channel: int) -> TabbedEditor:
    """Zones and envelopes, beside every parameter."""
    instrument = next(
        (
            str(c["instrument"])
            for c in session.require().channels_info()
            if int(c["id"]) == channel
        ),
        "sampler",
    )
    zones = ZoneEditor(session, channel, shared_cache())
    zones.setMinimumHeight(160)
    return tabbed(session, channel, instrument, zones)
