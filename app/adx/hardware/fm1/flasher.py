"""Writing firmware to the FM-1 (phase_11.5.md 4.11): verify, confirm, enter upgrade,
serve, finish - and stop, never retry, when anything goes wrong.

A state machine driven from outside: :meth:`Flasher.on_sysex` for each message from the
device, :meth:`Flasher.poll` on a timer. It talks through a :class:`Link`, so the tests
and the emulator drive exactly the code the panel does.

The gates, in order, each one a hard stop:

1. the package's bytes match the catalog's SHA-256 and look like an FM-1 image
   (firmware.image_problem); a package that fails is never offered;
2. the user types the firmware's name, exactly - a bare OK is not enough;
3. the protocol has been verified on hardware (ota.PROTOCOL_VERIFIED), or the device is
   the emulator - until the final review, a real FM-1 is never written.
"""

from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass, field
from enum import Enum
from typing import Protocol

from adx.hardware.fm1 import identity, ota
from adx.hardware.fm1.firmware import FirmwarePackage, image_problem
from adx.hardware.fm1.recovery import DeviceState, guide

#: How long the device gets to show up in OTA mode, to ask for its next block, and to
#: come back as itself after the last one.
ENTER_TIMEOUT = 10.0
IDLE_TIMEOUT = 5.0
REBOOT_TIMEOUT = 30.0


class Link(Protocol):
    def ports(self) -> list[str]:
        """The MIDI port names present now."""
        ...

    def open(self, port: str) -> bool:
        """Open ``port`` both ways, SysEx kept."""
        ...

    def send(self, port: str, message: bytes) -> None: ...


class State(Enum):
    IDLE = "idle"
    VERIFIED = "verified"
    CONFIRMED = "confirmed"
    ENTERING = "waiting for update mode"
    SERVING = "sending"
    REBOOTING = "waiting for the device to restart"
    DONE = "done"
    INTERRUPTED = "interrupted"
    REFUSED = "refused"


@dataclass
class Flasher:
    link: Link
    #: The FM-1 is the emulator (tools/virtual_fm1.py), not hardware.
    emulated: bool = False
    clock: Callable[[], float] = time.monotonic
    state: State = State.IDLE
    package: FirmwarePackage | None = None
    message: str = ""
    before: identity.Identity | None = None
    after: identity.Identity | None = None
    server: ota.ImageServer | None = None
    _data: bytes = b""
    _since: float = 0.0
    _port: str = ""
    _asked: float = -1.0
    log: list[str] = field(default_factory=list)

    # --- the gates -----------------------------------------------------------------

    def verify(self, package: FirmwarePackage, data: bytes) -> bool:
        problem = image_problem(data, package)
        if problem:
            return self._refuse(problem)
        self.package, self._data = package, data
        self.state = State.VERIFIED
        self.message = f"{package.name} {package.version}: hash and image check passed"
        return True

    def warning(self) -> str:
        """What the confirmation dialog says before the user types the name."""
        p = self.package
        assert p is not None
        return (
            f"Install {p.name} {p.version} from {p.source} ({p.license or 'licence unknown'}) "
            "on the FM-1. The device has one flash bank and no verified recovery path: if the "
            "write fails, it needs the community UBOOT recovery tools, which may not work. "
            f"{p.risk} Type the firmware's name, {p.name}, to go ahead."
        )

    def confirm(self, typed: str) -> bool:
        if self.state is not State.VERIFIED or self.package is None:
            return self._refuse("verify a package first")
        if typed != self.package.name:
            self.message = "the name typed does not match; nothing was sent"
            return False
        self.state = State.CONFIRMED
        return True

    def start(self, port: str, before: identity.Identity | None) -> bool:
        """Send enter-upgrade to ``port``: only after both gates, and only to the emulator
        until the protocol is verified on hardware."""
        if self.state is not State.CONFIRMED:
            return self._refuse("confirm the install first")
        if not (ota.PROTOCOL_VERIFIED or self.emulated):
            return self._refuse(
                "writing to a real FM-1 is off until adX's update protocol has been checked "
                "on hardware (the final review); nothing was sent"
            )
        if before is None or not before.known:
            return self._refuse("the device did not identify itself as a known FM-1")
        self.before = before
        self.link.send(port, ota.ENTER_UPGRADE)
        self._port = port
        self.state = State.ENTERING
        self._since = self.clock()
        self.log.append("enter-upgrade sent")
        return True

    # --- driven from outside -------------------------------------------------------

    def on_sysex(self, port: str, message: bytes) -> None:
        if self.state in (State.ENTERING, State.SERVING):
            request = ota.parse_request(message)
            if request is None:
                return
            if self.state is State.ENTERING:
                self.server = ota.ImageServer(self._data)
                self.state = State.SERVING
                self.log.append(f"device requesting on {port}")
            self._port = port
            self._since = self.clock()
            assert self.server is not None
            reply = self.server.answer(request)
            if reply is not None:
                self.link.send(port, reply)
            elif request.complete:
                self.state = State.REBOOTING
                self._since = self.clock()
                self._asked = -1.0
                self.log.append("device has the image")
        elif self.state is State.REBOOTING:
            answered = identity.parse_reply(message)
            if answered is not None:
                self.after = answered
                self.state = State.DONE
                changed = self.before is None or answered.model != self.before.model
                self.message = (
                    f"installed: the device now reports {answered.model}"
                    if changed
                    else f"installed; the device still reports {answered.model}"
                )

    def poll(self) -> None:
        now = self.clock()
        ports = self.link.ports()
        if self.state is State.ENTERING:
            for p in ports:
                if identity.is_ota_port(p) and p != self._port and self.link.open(p):
                    self._port = p
                    self.log.append(f"{p} appeared")
            if now - self._since > ENTER_TIMEOUT:
                self._interrupt("the device did not enter update mode")
        elif self.state is State.SERVING:
            gone = not any(p == self._port for p in ports)
            if gone:
                self._interrupt("the device disappeared during the transfer")
            elif now - self._since > IDLE_TIMEOUT:
                self._interrupt("the device stopped asking for data")
        elif self.state is State.REBOOTING:
            normal = [p for p in ports if identity.is_fm1_port(p)]
            if normal and now - self._asked >= 1.0 and self.link.open(normal[0]):
                self._asked = now
                self.link.send(normal[0], identity.IDENTITY_QUERY)
            if now - self._since > REBOOT_TIMEOUT:
                self._interrupt("the device did not come back after the update")

    # --- outcomes ------------------------------------------------------------------

    def _refuse(self, why: str) -> bool:
        self.state = State.REFUSED
        self.message = why
        self.log.append(f"refused: {why}")
        return False

    def _interrupt(self, why: str) -> None:
        """Stop. No retry: the recovery guide is what comes next."""
        self.state = State.INTERRUPTED
        self.message = f"{why}. Nothing more will be sent. {guide(DeviceState.OTA_STRANDED)}"
        self.log.append(f"interrupted: {why}")

    @property
    def recovery_shown(self) -> bool:
        return self.state is State.INTERRUPTED
