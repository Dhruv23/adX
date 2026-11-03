"""The export dialog (phase_8.md 3.5): every axis of the matrix over one ExportSettings.

The dialog only builds settings; :class:`ExportRunner` runs them as an ExportJob off the
UI thread and polls it from a timer, so a long export never blocks the window. ``adx
render`` builds the same ExportSettings, which is what makes the two agree.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QObject, QTimer, Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLineEdit,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from adx.engine_bridge import ExportJob, Project
from adx.export_settings import DITHERS, FORMATS, STEMS, ExportSettings, format_for

FORMAT_LABELS = {
    "wav16": "WAV 16-bit",
    "wav24": "WAV 24-bit",
    "wav32f": "WAV 32-bit float",
    "mp3": "MP3",
    "flac": "FLAC",
    "ogg": "OGG Vorbis",
}
_EXTENSIONS = {"mp3": ".mp3", "flac": ".flac", "ogg": ".ogg"}
SOURCES = ("master", "master + stems", "stems only")
RANGE_LABELS = {
    "project": "Whole project",
    "loop": "Loop region",
    "markers": "Between markers",
    "selection": "Selection",
}
RATES = (0, 44100, 48000, 88200, 96000)
BLOCKS = (64, 128, 256, 512, 1024)


class ExportDialog(QDialog):
    """Reads as settings; :meth:`settings` is what OK means."""

    def __init__(
        self,
        output: str,
        markers: list[str],
        selection: tuple[int, int] | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("Export")
        self.setObjectName("export")
        self._selection = selection
        layout = QVBoxLayout(self)

        file_box = QGroupBox("File")
        form = QFormLayout(file_box)
        row = QHBoxLayout()
        self.output = QLineEdit(output)
        self.output.setObjectName("export.output")
        browse = QPushButton("Browse…")
        browse.clicked.connect(self._browse)
        row.addWidget(self.output, 1)
        row.addWidget(browse)
        form.addRow("Save as", row)
        self.format = QComboBox()
        self.format.setObjectName("export.format")
        for name in FORMATS:
            self.format.addItem(FORMAT_LABELS[name], name)
        self.format.setCurrentIndex(FORMATS.index(format_for(output)))
        self.format.currentIndexChanged.connect(self._format_changed)
        form.addRow("Format", self.format)
        self.bitrate = QComboBox()
        for kbps in (128, 192, 256, 320):
            self.bitrate.addItem(f"{kbps} kbit/s CBR", kbps)
        self.bitrate.setCurrentIndex(3)
        form.addRow("MP3 bitrate", self.bitrate)
        self.flac_level = QSpinBox()
        self.flac_level.setRange(0, 8)
        self.flac_level.setValue(5)
        form.addRow("FLAC level", self.flac_level)
        self.ogg_quality = QDoubleSpinBox()
        self.ogg_quality.setRange(-0.1, 1.0)
        self.ogg_quality.setSingleStep(0.1)
        self.ogg_quality.setValue(0.6)
        form.addRow("OGG quality", self.ogg_quality)
        self.rate = QComboBox()
        for rate in RATES:
            self.rate.addItem("Project rate" if rate == 0 else f"{rate} Hz", rate)
        form.addRow("Sample rate", self.rate)
        layout.addWidget(file_box)

        what_box = QGroupBox("What")
        form = QFormLayout(what_box)
        self.source = QComboBox()
        self.source.addItems(SOURCES)
        self.source.currentIndexChanged.connect(self._update_enabled)
        form.addRow("Source", self.source)
        self.stems = QComboBox()
        for name in STEMS:
            self.stems.addItem(f"per {name[:-1]}", name)
        form.addRow("Stems", self.stems)
        self.range = QComboBox()
        for key, label in RANGE_LABELS.items():
            if key == "selection" and selection is None:
                continue
            self.range.addItem(label, key)
        self.range.currentIndexChanged.connect(self._update_enabled)
        form.addRow("Range", self.range)
        self.from_marker = QComboBox()
        self.to_marker = QComboBox()
        self.from_marker.addItems(["(start)", *markers])
        self.to_marker.addItems(["(end)", *markers])
        markers_row = QHBoxLayout()
        markers_row.addWidget(self.from_marker)
        markers_row.addWidget(self.to_marker)
        form.addRow("Markers", markers_row)
        self.split = QCheckBox("One file per stretch between markers")
        self.split.setEnabled(bool(markers))
        form.addRow("", self.split)
        tail_row = QHBoxLayout()
        self.tail = QDoubleSpinBox()
        self.tail.setRange(0.0, 60.0)
        self.tail.setSuffix(" s")
        self.tail.setValue(2.0)
        self.until_silence = QCheckBox("until silence")
        self.until_silence.toggled.connect(self._update_enabled)
        tail_row.addWidget(self.tail)
        tail_row.addWidget(self.until_silence)
        form.addRow("Tail", tail_row)
        layout.addWidget(what_box)

        processing = QGroupBox("Processing")
        form = QFormLayout(processing)
        self.dither = QComboBox()
        for name in DITHERS:
            self.dither.addItem(
                {"tpdf": "TPDF", "shaped": "Noise-shaped", "off": "Off"}[name], name
            )
        self.dither.setToolTip("Applied last, and only when the format has fewer bits than 32f")
        form.addRow("Dither", self.dither)
        norm_row = QHBoxLayout()
        self.normalize = QComboBox()
        self.normalize.addItem("Off", "off")
        self.normalize.addItem("Peak (dBFS)", "peak")
        self.normalize.addItem("Loudness (LUFS)", "lufs")
        self.normalize.currentIndexChanged.connect(self._normalize_changed)
        self.target = QDoubleSpinBox()
        self.target.setRange(-40.0, 0.0)
        self.target.setSingleStep(0.5)
        self.target.setValue(-14.0)
        norm_row.addWidget(self.normalize)
        norm_row.addWidget(self.target)
        form.addRow("Normalize", norm_row)
        self.block = QComboBox()
        for frames in BLOCKS:
            self.block.addItem(str(frames), frames)
        self.block.setCurrentIndex(BLOCKS.index(256))
        self.block.setToolTip("Renders are bit-exact per block size; adx render --block matches")
        form.addRow("Block", self.block)
        self.midi = QCheckBox("Also write MIDI (.mid beside the file)")
        form.addRow("", self.midi)
        self.bundle = QCheckBox("Also collect a project bundle")
        form.addRow("", self.bundle)
        layout.addWidget(processing)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("Export")
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self._format_changed()
        self._normalize_changed()

    def _browse(self) -> None:
        path, _filter = QFileDialog.getSaveFileName(
            self, "Export", self.output.text(), "Audio (*.wav *.mp3 *.flac *.ogg)"
        )
        if path:
            self.output.setText(path)
            self.format.setCurrentIndex(FORMATS.index(format_for(path)))

    def _format_changed(self) -> None:
        name = str(self.format.currentData())
        self.bitrate.setEnabled(name == "mp3")
        self.flac_level.setEnabled(name == "flac")
        self.ogg_quality.setEnabled(name == "ogg")
        self.dither.setEnabled(name != "wav32f")
        # Keep the file's extension in step with the format.
        path = Path(self.output.text())
        if path.name:
            wanted = _EXTENSIONS.get(name, ".wav")
            if path.suffix.lower() != wanted:
                self.output.setText(str(path.with_suffix(wanted)))
        self._update_enabled()

    def _normalize_changed(self) -> None:
        kind = str(self.normalize.currentData())
        self.target.setEnabled(kind != "off")
        self.target.setValue(-14.0 if kind == "lufs" else -1.0)

    def _update_enabled(self) -> None:
        self.stems.setEnabled(self.source.currentIndex() > 0)
        between = self.range.currentData() == "markers"
        self.from_marker.setEnabled(between)
        self.to_marker.setEnabled(between)
        self.tail.setEnabled(not self.until_silence.isChecked())

    def settings(self) -> ExportSettings:
        """What the dialog says, as the settings ``adx render`` would build."""
        source = self.source.currentIndex()
        settings = ExportSettings(output=self.output.text(), format=str(self.format.currentData()))
        settings.master = source < 2
        settings.stems = str(self.stems.currentData()) if source > 0 else ""
        settings.range = str(self.range.currentData())
        if settings.range == "markers":
            settings.from_marker = (
                self.from_marker.currentText() if self.from_marker.currentIndex() > 0 else ""
            )
            settings.to_marker = (
                self.to_marker.currentText() if self.to_marker.currentIndex() > 0 else ""
            )
        if settings.range == "selection" and self._selection is not None:
            settings.from_ticks, settings.to_ticks = self._selection
        settings.split_at_markers = self.split.isChecked()
        settings.tail_until_silence = self.until_silence.isChecked()
        settings.tail_seconds = 0.0 if settings.tail_until_silence else self.tail.value()
        settings.dither = str(self.dither.currentData())
        settings.normalize = str(self.normalize.currentData())
        settings.normalize_target = self.target.value()
        settings.sample_rate = int(self.rate.currentData())
        settings.block = int(self.block.currentData())
        settings.mp3_bitrate = int(self.bitrate.currentData())
        settings.flac_level = self.flac_level.value()
        settings.ogg_quality = self.ogg_quality.value()
        settings.midi = self.midi.isChecked()
        return settings


class ExportRunner(QObject):
    """One ExportJob, polled from a timer: ``progress(fraction)``, then ``finished(result)``."""

    progress = Signal(float)
    finished = Signal(dict)

    def __init__(
        self, project: Project, settings: ExportSettings, parent: QObject | None = None
    ) -> None:
        super().__init__(parent)
        self.settings = settings
        self.job = ExportJob(project, settings.request())
        self._timer = QTimer(self)
        self._timer.setInterval(100)
        self._timer.timeout.connect(self._poll)
        self._timer.start()

    def cancel(self) -> None:
        """Ask the job to stop; ``finished`` still comes, with cancelled set."""
        self.job.cancel()

    def _poll(self) -> None:
        if not self.job.done:
            self.progress.emit(self.job.progress)
            return
        self._timer.stop()
        self.finished.emit(self.job.result())
