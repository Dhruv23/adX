"""Who is on the other end (phase_11.5.md 2.4, 4.8): the vendor identity query and its
reply, the firmware generation, and the USB names and ids that mark an FM-1, its OTA
mode and its ROM recovery mode.

The device answers only parameter 0x40 of the query under M-VAVE's manufacturer id
00 32, with a model string such as ``FM-1_015``. The reply's framing past the header is
read leniently: the model string is found wherever it sits in the message.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

#: F0 00 32 45 00 00 00 40 7F F7.
IDENTITY_QUERY = bytes([0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7])
MANUFACTURER = bytes([0x00, 0x32])

#: The composite USB device (MIDI + audio) and its two other states.
USB_NORMAL = (0x4C4A, 0xC755)
USB_OTA = (0x4D4A, 0x4155)
USB_UBOOT = (0x4C4A, 0x8057)
PORT_NAME = "FM-1 Midi"
OTA_PORT_NAME = "ota-FM-1"
AUDIO_NAME = "FM-1 Audio"

_MODEL = re.compile(rb"FM-1_[0-9A-Z]{2,8}")


#: What tools/virtual_fm1.py appends to its identity reply. Only a device that says this
#: is treated as the emulator - the one thing a firmware write is allowed to go to before
#: the protocol is verified on hardware (ota.PROTOCOL_VERIFIED).
EMULATOR_MARK = b" adX-EMU"


@dataclass(frozen=True)
class Identity:
    model: str
    #: 14, 15, 16..., or None for a model adX does not know (community firmware).
    generation: int | None
    emulated: bool = False

    @property
    def known(self) -> bool:
        return self.generation is not None or self.model == "FM-1_OPEN"


def is_vendor(message: bytes) -> bool:
    """A SysEx message under M-VAVE's id (00 32)."""
    return len(message) >= 4 and message[0] == 0xF0 and message[1:3] == MANUFACTURER


def is_query(message: bytes) -> bool:
    return bytes(message) == IDENTITY_QUERY


def parse_reply(message: bytes) -> Identity | None:
    """The identity in a reply; None when the message is not one (the query itself, which
    a loopback hands back, included)."""
    if not is_vendor(message) or is_query(message) or len(message) < 5 or message[3] != 0x45:
        return None
    found = _MODEL.search(bytes(message))
    if found is None:
        return None
    model = found.group(0).decode("ascii")
    suffix = model.split("_", 1)[1]
    generation = int(suffix) if suffix.isdigit() and 10 <= int(suffix) <= 99 else None
    return Identity(model=model, generation=generation, emulated=EMULATOR_MARK in bytes(message))


def reply_for(model: str, emulated: bool = False) -> bytes:
    """The reply a device with ``model`` sends (the emulator's, and the tests')."""
    mark = EMULATOR_MARK if emulated else b""
    return bytes(
        [0xF0, 0x00, 0x32, 0x45, 0x01, 0x00, 0x00, 0x40, *model.encode("ascii"), *mark, 0xF7]
    )


def is_fm1_port(name: str) -> bool:
    """A MIDI port that is an FM-1 in normal mode (Windows may append a number)."""
    return name.startswith(PORT_NAME)


def is_ota_port(name: str) -> bool:
    return name.startswith(OTA_PORT_NAME)


def fallback_generation(answered: Identity | None) -> int:
    """What to assume with no (or an unknown) reply: V15, which the user can change."""
    if answered is not None and answered.generation is not None:
        return answered.generation
    return 15
