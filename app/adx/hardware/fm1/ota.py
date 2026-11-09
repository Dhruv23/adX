"""The FM-1's OTA update protocol, adX's side (phase_11.5.md 2.4, 4.11).

After ENTER_UPGRADE the device re-enumerates as ``ota-FM-1`` and pulls the image: it
sends read requests, and adX answers each with the addressed slice. adX never pushes; the
device sets the pace. A request for COMPLETE_ADDRESS means it has all it wants.

The byte layout below is adX's reading of the published reverse-engineering notes:

    request  F0 00 32 41 41 <flash type> <address: 5 x 7 bits, LSB first>
             <length: 2 x 7 bits, LSB first> F7
    reply    F0 00 32 41 42 <flash type> <address> <length> <pack7(data)> <checksum> F7

with ``checksum = ~(flash type + sum(data) + sum(address bytes) + sum(length bytes))``,
seven bits, and pack7 an LSB-first 7-bit bitstream. The emulator (tools/virtual_fm1.py)
speaks exactly this. **It has not been checked against a device**, which is why
:data:`PROTOCOL_VERIFIED` is False and Flasher refuses a real device until the final
review checks it on hardware (phase_11.5.md 9).
"""

from __future__ import annotations

from dataclasses import dataclass

ENTER_UPGRADE = bytes([0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7])
COMPLETE_ADDRESS = 0xF0000000
_HEADER = bytes([0xF0, 0x00, 0x32, 0x41])
_READ = 0x41
_DATA = 0x42

#: Flipped once a real flash cycle has been observed (final review). Until then a real
#: device is never written: only the emulator.
PROTOCOL_VERIFIED = False


def pack7(data: bytes) -> bytes:
    """8-bit bytes as a 7-bit stream, least significant bit first."""
    out = bytearray()
    acc = 0
    bits = 0
    for byte in data:
        acc |= byte << bits
        bits += 8
        while bits >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            bits -= 7
    if bits:
        out.append(acc & 0x7F)
    return bytes(out)


def unpack7(data: bytes, length: int) -> bytes:
    """The inverse of :func:`pack7`, for ``length`` bytes."""
    out = bytearray()
    acc = 0
    bits = 0
    for septet in data:
        acc |= (septet & 0x7F) << bits
        bits += 7
        while bits >= 8 and len(out) < length:
            out.append(acc & 0xFF)
            acc >>= 8
            bits -= 8
    return bytes(out)


def _septets(value: int, count: int) -> bytes:
    return bytes((value >> (7 * i)) & 0x7F for i in range(count))


def _from_septets(data: bytes) -> int:
    return sum((b & 0x7F) << (7 * i) for i, b in enumerate(data))


def checksum(flash_type: int, address: bytes, length: bytes, data: bytes) -> int:
    return ~(flash_type + sum(data) + sum(address) + sum(length)) & 0x7F


@dataclass(frozen=True)
class ReadRequest:
    flash_type: int
    address: int
    length: int

    @property
    def complete(self) -> bool:
        return self.address == COMPLETE_ADDRESS


def encode_request(request: ReadRequest) -> bytes:
    """The device's side (the emulator and the tests)."""
    return bytes(
        [
            *_HEADER,
            _READ,
            request.flash_type & 0x7F,
            *_septets(request.address, 5),
            *_septets(request.length, 2),
            0xF7,
        ]
    )


def parse_request(message: bytes) -> ReadRequest | None:
    m = bytes(message)
    if len(m) != 14 or m[:4] != _HEADER or m[4] != _READ or m[-1] != 0xF7:
        return None
    return ReadRequest(m[5], _from_septets(m[6:11]), _from_septets(m[11:13]))


def encode_reply(request: ReadRequest, data: bytes) -> bytes:
    address = _septets(request.address, 5)
    length = _septets(len(data), 2)
    return bytes(
        [
            *_HEADER,
            _DATA,
            request.flash_type & 0x7F,
            *address,
            *length,
            *pack7(data),
            checksum(request.flash_type, address, length, data),
            0xF7,
        ]
    )


def parse_reply(message: bytes) -> tuple[ReadRequest, bytes, bool] | None:
    """(what it answers, the data, whether the checksum holds) - the device's side."""
    m = bytes(message)
    if len(m) < 16 or m[:4] != _HEADER or m[4] != _DATA or m[-1] != 0xF7:
        return None
    flash_type = m[5]
    address, length = m[6:11], m[11:13]
    count = _from_septets(length)
    data = unpack7(m[13:-2], count)
    ok = len(data) == count and checksum(flash_type, address, length, data) == m[-2]
    return ReadRequest(flash_type, _from_septets(address), count), data, ok


class ImageServer:
    """Answers read requests from one image. Refuses anything outside it; progress is the
    highest address served."""

    def __init__(self, image: bytes) -> None:
        self.image = image
        self.highest = 0
        self.refused = 0
        self.served = 0
        self.done = False

    @property
    def progress(self) -> float:
        return min(1.0, self.highest / max(1, len(self.image)))

    def answer(self, request: ReadRequest) -> bytes | None:
        """The reply, or None for a completion marker or a request it refuses."""
        if request.complete:
            self.done = True
            return None
        end = request.address + request.length
        if request.length <= 0 or request.address < 0 or end > len(self.image):
            self.refused += 1
            return None
        self.served += 1
        self.highest = max(self.highest, end)
        return encode_reply(request, self.image[request.address : end])
