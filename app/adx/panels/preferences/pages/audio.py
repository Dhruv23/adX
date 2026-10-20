"""Audio: the device the engine opens, the rate and buffer it opens with (applied at the
next start), and the latency readout - the buffer plus the graph's own plugin delay."""

from __future__ import annotations

from PySide6.QtWidgets import QComboBox, QFormLayout, QLabel, QWidget

from adx.preferences import Preferences

RATES = (44100, 48000, 88200, 96000)
BUFFERS = (64, 128, 256, 512, 1024, 2048)


def devices() -> list[str]:
    """The system's output devices, for display (the engine opens the default)."""
    try:
        from adx import engine_bridge

        return [str(d.get("name", "?")) for d in engine_bridge.enumerate_devices()]
    except (RuntimeError, AttributeError):
        return []


class AudioPage(QWidget):
    """Rate, buffer, device, latency."""

    title = "Audio"

    def __init__(
        self, prefs: Preferences, latency_ms: float, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.prefs = prefs
        form = QFormLayout(self)
        self.device = QComboBox()
        self.device.addItems(devices() or ["(default device)"])
        self.device.setToolTip("adX opens the system's default output")
        form.addRow("Device", self.device)
        self.rate = QComboBox()
        self.rate.addItems([str(r) for r in RATES])
        self.rate.setCurrentText(str(prefs.sample_rate))
        form.addRow("Sample rate", self.rate)
        self.buffer = QComboBox()
        self.buffer.addItems([str(b) for b in BUFFERS])
        self.buffer.setCurrentText(str(prefs.buffer_frames))
        form.addRow("Buffer (frames)", self.buffer)
        buffer_ms = prefs.buffer_frames / prefs.sample_rate * 1000.0
        form.addRow(
            "Latency", QLabel(f"{buffer_ms:.1f} ms buffer + {latency_ms:.1f} ms plugin delay")
        )
        form.addRow(QLabel("Rate and buffer apply the next time adX starts."))

    def apply(self) -> None:
        """Write this page's fields."""
        self.prefs.sample_rate = int(self.rate.currentText())
        self.prefs.buffer_frames = int(self.buffer.currentText())
