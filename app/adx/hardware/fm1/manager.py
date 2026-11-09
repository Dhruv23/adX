"""The FM-1 in the running app (phase_11.5.md 4.4, 4.6, 4.8): finds it, identifies it,
owns its ports, drains SysEx at frame rate and hands each message to whoever it is for.

- **Finding it.** Once a second the MIDI port lists are read; a port named "FM-1 Midi"
  is opened both ways with SysEx kept, and the identity query goes out. Its reply picks
  the profile; no reply within two seconds falls back to V15 (the panel can change it).
- **SysEx.** DX7 voices and banks go to the librarian's inbox, OTA read requests to the
  flasher, identity replies here, everything else to the monitor (last 100).
- **Getting patches off it**, in order: a dump request (waits two seconds, the answer
  cached per firmware), then "press Bank Export", then a `.syx` file.
- **Live edit.** While on, the edited dx7 channel's voice is diffed every tick and each
  changed byte is sent as a DX7 parameter change (LiveEdit's rate limit); choosing a
  different channel sends its whole voice.
"""

from __future__ import annotations

import os
import time
from collections.abc import Callable
from dataclasses import dataclass, field

from PySide6.QtCore import QObject, QTimer, Signal

from adx import engine_bridge
from adx.engine_bridge import Engine
from adx.hardware import dx7
from adx.hardware.fm1 import identity, ota, profile
from adx.hardware.fm1.device import Fm1Device, LiveEdit, VelocityWatch
from adx.hardware.fm1.flasher import Flasher
from adx.hardware.fm1.recovery import DeviceState, classify
from adx.live_midi import LiveMidi
from adx.session import Session

#: How long a dump request is given before the librarian asks for Bank Export.
DUMP_WAIT = 2.0
IDENTIFY_WAIT = 2.0


@dataclass
class Probe:
    """What the device turned out to do (4.8); None until probed."""

    answers_dump: bool | None = None
    sends_velocity: bool | None = None
    usb_playback: bool | None = None


@dataclass
class Inbox:
    """Voices received from the device or a file, newest last."""

    banks: list[tuple[str, list[list[int]]]] = field(default_factory=list)
    bad_checksums: int = 0


class Fm1Manager(QObject):
    #: Connection state changed (connected / identified / gone).
    changed = Signal()
    #: Voices arrived: (where from, how many).
    received = Signal(str, int)

    def __init__(
        self,
        engine: Engine,
        live: LiveMidi,
        session: Session,
        report: Callable[[str], None] = lambda _m: None,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent)
        self.engine = engine
        self.live = live
        self.session = session
        self.report = report
        self.port_in = ""
        self.port_out = ""
        self.identity: identity.Identity | None = None
        self.profile: profile.DeviceProfile | None = None
        self.device: Fm1Device | None = None
        self.state = DeviceState.ABSENT
        self.probe = Probe()
        self.inbox = Inbox()
        self.monitor: list[tuple[str, bytes]] = []
        self.flasher: Flasher | None = None
        self.waiting_for_export = False
        self.live_channel: int | None = None
        self.live_edit: LiveEdit | None = None
        self.velocity = VelocityWatch()
        #: The audio return's measured latency, by sample rate.
        self.latency: dict[int, int] = {}
        self.snapshot_on_connect = True
        self._identify_at = 0.0
        self._dump_at = 0.0
        self._last_voice: list[int] | None = None
        self._scan = QTimer(self)
        self._scan.setInterval(1000)
        self._scan.timeout.connect(self.scan)
        self._tick = QTimer(self)
        self._tick.setInterval(33)
        self._tick.timeout.connect(self.tick)

    def start(self) -> None:
        """Watch for the device. With no audio device (tests, ``--null-audio``) only when
        ADX_FM1=1 asks: MIDI works without audio, and a scenario wants it."""
        if self.engine.null_backend and os.environ.get("ADX_FM1") != "1":
            return
        self._scan.start()
        self._tick.start()

    def stop(self) -> None:
        self._scan.stop()
        self._tick.stop()

    # --- ports -----------------------------------------------------------------------

    def ports(self) -> list[str]:
        try:
            return sorted(set(self.engine.midi_inputs()) | set(self.engine.midi_outputs()))
        except RuntimeError:
            return []

    def scan(self) -> None:
        """Find (or lose) the device; identify a new one."""
        try:
            inputs, outputs = self.engine.midi_inputs(), self.engine.midi_outputs()
        except RuntimeError:
            return
        # A port that vanished (the FM-1 re-enumerating for an update, or unplugged) is
        # closed first: a handle left open on a vanished virtual or USB port keeps the
        # next device to appear from hearing what adX sends it.
        for gone in sorted(self.live.owned - set(inputs)):
            self.live.release(gone)
        for gone in sorted(self.live.owned_outputs - set(outputs)):
            self.live.release(gone)
        fm1_in = next((p for p in inputs if identity.is_fm1_port(p)), "")
        fm1_out = next((p for p in outputs if identity.is_fm1_port(p)), "")
        transferring = self.flasher is not None and self.flasher.state.name == "SERVING"
        state = classify(inputs + outputs, [], transferring)
        if fm1_in and fm1_out and (fm1_in, fm1_out) != (self.port_in, self.port_out):
            self.connect_ports(fm1_in, fm1_out)
        elif not fm1_in and self.port_in:
            self.live.release(self.port_in)
            self.live.release(self.port_out)
            self.port_in = self.port_out = ""
            self.device = None
        if state is not self.state:
            self.state = state
            self.changed.emit()
        if self.flasher is not None:
            self.flasher.poll()

    def connect_ports(self, port_in: str, port_out: str) -> bool:
        if not (self.live.own(port_in) and self.live.own_output(port_out)):
            return False
        self.port_in, self.port_out = port_in, port_out
        self.identity = None
        self.send_sysex(identity.IDENTITY_QUERY)
        self._identify_at = time.monotonic()
        self.report(f"FM-1 found on {port_in}")
        self.changed.emit()
        return True

    def send(self, message: bytes) -> None:
        if self.port_out:
            self.live.send(self.port_out, message)

    def send_sysex(self, message: bytes, gap_ms: int = 0) -> None:
        if not self.port_out:
            return
        try:
            self.engine.send_sysex(self.port_out, message, gap_ms)
        except RuntimeError as error:
            self.report(f"FM-1: {error}")

    def use_profile(self, chosen: profile.DeviceProfile) -> None:
        self.profile = chosen
        self.device = Fm1Device(chosen, self.send, self.send_sysex)
        if self.snapshot_on_connect:
            self.device.send_snapshot()
        self.point_part_channels()
        self.changed.emit()

    def check_take(self, pattern: str, channel: int) -> bool | None:
        """After a MIDI take while the FM-1 is connected: did its notes carry real
        velocity? A run of FIXED_VELOCITY_RUN at one value marks the device as sending a
        fixed one (4.6); the probe line shows it. None when there was nothing to judge."""
        project = self.session.project
        if project is None or not self.port_in:
            return None
        name = next(
            (str(c["name"]) for c in project.channels_info() if int(c["id"]) == channel), ""
        )
        if not name:
            return None
        notes = project.notes(pattern, name)
        if len(notes) == 0:
            return None
        watch = VelocityWatch()
        for velocity in notes["velocity"][notes["start"].argsort()]:
            watch.note(int(velocity))
        self.probe.sends_velocity = not watch.fixed
        self.changed.emit()
        return self.probe.sends_velocity

    # --- parts as channels (4.6) ---------------------------------------------------------

    PART_PREFIX = "FM-1 part "

    def port_slot(self) -> int:
        try:
            return self.engine.midi_output_slot(self.port_out) if self.port_out else -1
        except RuntimeError:
            return -1

    def part_channels(self) -> dict[int, int]:
        """Part number (1-based) -> the midiout channel playing it, by name."""
        project = self.session.project
        if project is None:
            return {}
        out = {}
        for c in project.channels_info():
            name = str(c["name"])
            if c["instrument"] == "midiout" and name.startswith(self.PART_PREFIX):
                tail = name[len(self.PART_PREFIX) :]
                if tail.isdigit():
                    out[int(tail)] = int(c["id"])
        return out

    def add_part_channels(self) -> list[str]:
        """A midiout channel per part the device has ("FM-1 part 1".."8"), each on its
        part's MIDI channel, playing into the FM-1's port: one undo step."""
        if self.device is None or self.session.project is None:
            return []
        have = self.part_channels()
        made = []
        with self.session.group("Add FM-1 part channels"):
            for part in self.device.parts:
                if part.channel in have:
                    continue
                name = f"{self.PART_PREFIX}{part.channel}"
                self.session.build_and_execute("AddChannelNew", name, "midiout", 0, "")
                made.append(name)
            self.point_part_channels(grouped=False)
        return made

    def point_part_channels(self, grouped: bool = True) -> None:
        """Every part channel plays into the FM-1's port as it is now (a slot can change
        when the device reconnects), on its part's channel; one undo step, or none when
        they already do."""
        project = self.session.project
        channels = self.part_channels()
        if project is None or not channels:
            return
        slot = float(self.port_slot())
        held = {
            int(c["id"]): {str(p[0]): float(p[1]) for p in c["params"]}
            for c in project.channels_info()
        }
        changes = [
            (cid, name, value)
            for part, cid in channels.items()
            for name, value in (("port", slot), ("channel", float(part)))
            if held.get(cid, {}).get(name) != value
        ]
        if not changes:
            return
        if not grouped:
            for cid, name, value in changes:
                self.session.build_and_execute("SetChannelParam", cid, name, value)
            return
        with self.session.group("Point FM-1 part channels"):
            for cid, name, value in changes:
                self.session.build_and_execute("SetChannelParam", cid, name, value)

    def ping(self, engine: Engine | None = None, frames: int = 48000) -> int | None:
        """The audio return's latency in samples: a ping out, its onset timed in the input
        (the recording-offset measurement, Calibration). ``engine`` is a loopback engine in
        a test; the app's own engine needs the FM-1's audio as its input. Stored per rate."""
        target = engine or self.engine
        project = self.session.project
        if project is None:
            return None
        calibration = engine_bridge.Calibration(target)
        calibration.begin(project, 4800)
        if engine is not None:
            for _ in range(max(1, frames // 256)):
                engine.render_offline(256)
        else:
            time.sleep(frames / max(1, target.sample_rate))
        measured = calibration.finish()
        if measured is not None:
            self.latency[int(target.sample_rate)] = measured
        return measured

    # --- each tick ---------------------------------------------------------------------

    def tick(self) -> None:
        try:
            messages = self.engine.drain_sysex()
        except RuntimeError:
            messages = []
        for port, message in messages:
            self.dispatch(port, message)
        now = time.monotonic()
        if self.port_in and self.profile is None and now - self._identify_at > IDENTIFY_WAIT:
            generation = identity.fallback_generation(self.identity)
            fallback = profile.for_model(f"FM-1_0{generation}")
            if fallback is not None:
                self.report(f"FM-1 did not identify itself; assuming V{generation}")
                self.use_profile(fallback)
        if self._dump_at and now - self._dump_at > DUMP_WAIT:
            self._dump_at = 0.0
            self.probe.answers_dump = False
            self.waiting_for_export = True
            self.changed.emit()
        self._follow_edits()

    def dispatch(self, port: str, message: bytes) -> None:
        if self.flasher is not None and ota.parse_request(message) is not None:
            self.flasher.on_sysex(port, message)
            return
        answered = identity.parse_reply(message)
        if answered is not None:
            if self.flasher is not None:
                self.flasher.on_sysex(port, message)
            self.identity = answered
            chosen = profile.for_model(answered.model)
            if chosen is not None and (self.profile is None or self.profile is not chosen):
                self.use_profile(chosen)
            self.changed.emit()
            return
        if len(message) > 3 and message[1] == 0x43:
            read = engine_bridge.dx7_read(message)
            if read["voices"]:
                self.take_voices("FM-1", read["voices"], int(read["bad_checksums"]))
                return
        self.monitor.append((port, message))
        del self.monitor[:-100]

    def take_voices(self, source: str, voices: list[list[int]], bad: int = 0) -> None:
        if self._dump_at:
            self._dump_at = 0.0
            self.probe.answers_dump = True
        self.waiting_for_export = False
        self.inbox.banks.append((source, voices))
        self.inbox.bad_checksums += bad
        self.received.emit(source, len(voices))

    # --- the librarian's requests -------------------------------------------------------

    def request_bank(self) -> None:
        """A dump request; when it goes unanswered the panel asks for Bank Export."""
        if self.probe.answers_dump is False:
            self.waiting_for_export = True
            self.changed.emit()
            return
        self.send_sysex(engine_bridge.dx7_dump_request(True))
        self._dump_at = time.monotonic()

    # --- live editing ---------------------------------------------------------------------

    def set_live_edit(self, channel: int | None) -> None:
        """Follow the dx7 channel ``channel`` (None: stop)."""
        self.live_channel = channel
        self.live_edit = LiveEdit(self.send_sysex) if channel is not None else None
        self._last_voice = None

    def _follow_edits(self) -> None:
        if self.live_edit is None or self.live_channel is None or self.device is None:
            return
        try:
            voice = dx7.channel_voice(self.session, self.live_channel)
        except (StopIteration, RuntimeError):
            return
        if self._last_voice is None:
            self.device.send_voice(voice, dx7.name_of(voice))
        else:
            for param, (old, new) in enumerate(zip(self._last_voice, voice, strict=True)):
                if old != new:
                    self.live_edit.change(param, new)
        self.live_edit.flush()
        self._last_voice = voice

    # --- firmware -------------------------------------------------------------------------

    def new_flasher(self) -> Flasher:
        # Only a device whose identity carries the emulator's mark counts as one.
        emulated = self.identity is not None and self.identity.emulated
        self.flasher = Flasher(_ManagerLink(self), emulated=emulated)
        return self.flasher


class _ManagerLink:
    """The flasher's view of the ports, through the manager."""

    def __init__(self, manager: Fm1Manager) -> None:
        self.manager = manager

    def ports(self) -> list[str]:
        return self.manager.ports()

    def open(self, port: str) -> bool:
        m = self.manager
        try:
            outputs = m.engine.midi_outputs()
        except RuntimeError:
            return False
        base = port.rsplit(" ", 1)[0]
        out = next((o for o in outputs if o.startswith(base)), "")
        if not out:
            return False
        ok = m.live.own(port) if port in m.engine.midi_inputs() else True
        ok = ok and m.live.own_output(out)
        if ok and identity.is_fm1_port(port):
            m.port_in, m.port_out = port, out
        return ok

    def send(self, port: str, message: bytes) -> None:
        m = self.manager
        base = port.rsplit(" ", 1)[0]
        try:
            out = next((o for o in m.engine.midi_outputs() if o.startswith(base)), "")
            if out:
                if out not in m.engine.midi_outputs_opened:
                    m.live.own_output(out)
                m.engine.send_sysex(out, message, 0)
        except RuntimeError as error:
            m.report(f"FM-1 update: {error}")
