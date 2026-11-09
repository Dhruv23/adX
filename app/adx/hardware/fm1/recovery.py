"""Which state the FM-1 is in, and what to do about it (phase_11.5.md 4.11).

adX never writes in the mask ROM's UBOOT1.00 mode, and never retries an interrupted
transfer on its own; this module only says where the device is and how to get it back.
"""

from __future__ import annotations

import re
import subprocess
import sys
from enum import Enum

from adx.hardware.fm1 import identity


class DeviceState(Enum):
    ABSENT = "not found"
    NORMAL = "connected"
    #: In OTA mode with a transfer running.
    OTA = "updating"
    #: In OTA mode with no transfer: a write that stopped, or upgrade mode entered by hand.
    OTA_STRANDED = "OTA mode, no transfer"
    UBOOT = "ROM recovery (UBOOT1.00)"


GUIDES = {
    DeviceState.OTA_STRANDED: (
        "The FM-1 is in update mode with nothing being sent. Unplug it, hold no buttons and "
        "plug it back in: a device whose firmware is intact starts normally. If it comes "
        "back in update mode again, install the official firmware with M-VAVE's own "
        "updater, which speaks the same protocol. adX does not retry a stopped write."
    ),
    DeviceState.UBOOT: (
        "The FM-1 is in its ROM's recovery mode (UBOOT1.00, a USB drive with VID 4C4A, "
        "PID 8057): its firmware is missing or damaged. adX does not write in this mode. "
        "Recover it with a community tool made for it - fm1_rescue.py, "
        "MvaveFM1Unbricker or M-Flasher - and the official image from M-VAVE, following "
        "that tool's own instructions. There is no verified recovery path: read them first."
    ),
    DeviceState.ABSENT: (
        "No FM-1 found. Connect it by USB-C (a data cable, not a charge-only one). "
        "Bluetooth MIDI works for notes but not for firmware."
    ),
}


def classify(ports: list[str], usb: list[tuple[int, int]], transferring: bool) -> DeviceState:
    """The state from the MIDI port names and the USB ids present."""
    if identity.USB_UBOOT in usb:
        return DeviceState.UBOOT
    if any(identity.is_ota_port(p) for p in ports) or identity.USB_OTA in usb:
        return DeviceState.OTA if transferring else DeviceState.OTA_STRANDED
    if any(identity.is_fm1_port(p) for p in ports) or identity.USB_NORMAL in usb:
        return DeviceState.NORMAL
    return DeviceState.ABSENT


def guide(state: DeviceState) -> str:
    return GUIDES.get(state, "")


_IDS = re.compile(r"VID_([0-9A-F]{4})&PID_([0-9A-F]{4})", re.IGNORECASE)


def usb_ids() -> list[tuple[int, int]]:
    """The USB vendor and product ids present (Windows; empty elsewhere or on failure).
    Asks PnP, which takes a second or two: call it on demand, not per frame."""
    if sys.platform != "win32":
        return []
    try:
        out = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                "Get-PnpDevice -PresentOnly | Select-Object -ExpandProperty InstanceId",
            ],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    return sorted({(int(v, 16), int(p, 16)) for v, p in _IDS.findall(out)})
