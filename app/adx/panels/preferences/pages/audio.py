"""Audio: the device the engine opens, the rate and buffer it opens with (applied at the
next start), and the latency readout - the buffer plus the graph's own plugin delay."""

from __future__ import annotations

from PySide6.QtWidgets import QCheckBox, QComboBox, QFormLayout, QLabel, QWidget

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
        self.record_input = QCheckBox("Open the default input, for recording")
        self.record_input.setChecked(prefs.record_input)
        form.addRow("Recording", self.record_input)
        self.exclusive = QCheckBox("Exclusive mode (lowest latency)")
        self.exclusive.setChecked(prefs.exclusive_audio)
        self.exclusive.setToolTip(
            "Plays the default output in WASAPI exclusive mode: a few milliseconds instead "
            "of the Windows mixer's tens. Other applications cannot play while adX runs, "
            "and there is no recording input. Use a buffer of 128 or less."
        )
        form.addRow("Low latency", self.exclusive)
        buffer_ms = prefs.buffer_frames / prefs.sample_rate * 1000.0
        form.addRow(
            "Latency", QLabel(f"{buffer_ms:.1f} ms buffer + {latency_ms:.1f} ms plugin delay")
        )
        form.addRow(
            QLabel("Rate, buffer, the input and exclusive mode apply the next time adX starts.")
        )

    def apply(self) -> None:
        """Write this page's fields."""
        self.prefs.sample_rate = int(self.rate.currentText())
        self.prefs.buffer_frames = int(self.buffer.currentText())
        self.prefs.record_input = self.record_input.isChecked()
        self.prefs.exclusive_audio = self.exclusive.isChecked()
