"""The FM-1's parts (phase_11.5.md 4.6): V16's eight on channels 1-8 (one before V16),
each with a program, bend range and level; the connect snapshot; clock to the device;
the keys as pads; and the audio return's latency, measured by ping.
"""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtWidgets import (
    QCheckBox,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSpinBox,
    QTableWidget,
    QVBoxLayout,
    QWidget,
)

from adx.hardware.fm1.manager import Fm1Manager

COLUMNS = ("Channel", "Program", "Bend range", "Level")


class PartsTab(QWidget):
    def __init__(
        self,
        manager: Fm1Manager,
        set_clock: Callable[[bool], None] | None = None,
        set_pads: Callable[[bool], None] | None = None,
    ) -> None:
        super().__init__()
        self.manager = manager
        self.set_clock = set_clock
        self.set_pads = set_pads
        layout = QVBoxLayout(self)
        self.table = QTableWidget(0, len(COLUMNS))
        self.table.setObjectName("fm1.parts.table")
        self.table.setHorizontalHeaderLabels(list(COLUMNS))
        self.table.verticalHeader().setVisible(False)
        layout.addWidget(self.table, 1)
        row = QHBoxLayout()
        self.snapshot = QPushButton("Send everything")
        self.snapshot.setObjectName("fm1.parts.snapshot")
        self.snapshot.setToolTip("Programs, bend ranges, levels and every effect, in one go")
        self.snapshot.clicked.connect(self.send_snapshot)
        row.addWidget(self.snapshot)
        self.clock = QCheckBox("FM-1 follows adX (clock, start, stop)")
        self.clock.setObjectName("fm1.parts.clock")
        self.clock.toggled.connect(lambda on: self.set_clock(on) if self.set_clock else None)
        row.addWidget(self.clock)
        self.pads = QCheckBox("Use keys as pads")
        self.pads.setObjectName("fm1.parts.pads")
        self.pads.toggled.connect(lambda on: self.set_pads(on) if self.set_pads else None)
        row.addWidget(self.pads)
        layout.addLayout(row)
        row2 = QHBoxLayout()
        self.add_parts = QPushButton("Add part channels")
        self.add_parts.setObjectName("fm1.parts.add_channels")
        self.add_parts.setToolTip("A channel per part whose notes play on the FM-1")
        self.add_parts.clicked.connect(self.manager.add_part_channels)
        row2.addWidget(self.add_parts)
        self.ping_button = QPushButton("Ping")
        self.ping_button.setObjectName("fm1.parts.ping")
        self.ping_button.setToolTip("Time the FM-1's audio return (FM-1 Audio as the input)")
        self.ping_button.clicked.connect(self.ping)
        row2.addWidget(self.ping_button)
        layout.addLayout(row2)
        self.latency = QLabel("Audio return: not measured. Ping sends a note and times its onset.")
        self.latency.setObjectName("fm1.parts.latency")
        self.latency.setWordWrap(True)
        layout.addWidget(self.latency)
        self.spins: list[tuple[QSpinBox, QSpinBox, QSpinBox]] = []
        manager.changed.connect(self.refresh)
        self.refresh()

    def refresh(self) -> None:
        device = self.manager.device
        parts = device.parts if device is not None else []
        top = device.profile.bend_range_max if device is not None else 12
        self.table.setRowCount(len(parts))
        self.spins = []
        for index, part in enumerate(parts):
            label = QLabel(f"  {part.channel}")
            self.table.setCellWidget(index, 0, label)
            program = QSpinBox()
            program.setRange(0, 127)
            program.setSpecialValueText("—")
            program.setValue(part.program if part.program is not None else 0)
            program.valueChanged.connect(lambda v, i=index: self.set_program(i, v))
            bend = QSpinBox()
            bend.setRange(0, top)
            bend.setValue(part.bend_range)
            bend.valueChanged.connect(lambda v, i=index: self.set_bend(i, v))
            level = QSpinBox()
            level.setRange(0, 127)
            level.setValue(part.level)
            level.valueChanged.connect(lambda v, i=index: self.set_level(i, v))
            for column, widget in ((1, program), (2, bend), (3, level)):
                self.table.setCellWidget(index, column, widget)
            self.spins.append((program, bend, level))

    def set_program(self, part: int, slot: int) -> None:
        if self.manager.device is not None:
            self.manager.device.program(part, slot)

    def set_bend(self, part: int, semitones: int) -> None:
        if self.manager.device is not None:
            self.manager.device.bend_range(part, semitones)

    def set_level(self, part: int, level: int) -> None:
        device = self.manager.device
        if device is not None:
            device.parts[part].level = level
            device.part_cc(part, "VOLUME", level)

    def send_snapshot(self) -> int:
        return self.manager.device.send_snapshot() if self.manager.device is not None else 0

    def ping(self) -> None:
        try:
            samples = self.manager.ping()
        except RuntimeError as error:
            self.latency.setText(f"Audio return: {error}")
            return
        self.show_latency(samples, int(self.manager.engine.sample_rate))

    def show_latency(self, samples: int | None, rate: int) -> None:
        if samples is None:
            self.latency.setText("Audio return: nothing came back. Is FM-1 Audio the input?")
        else:
            self.latency.setText(
                f"Audio return: {samples} samples ({samples * 1000.0 / rate:.2f} ms) late; "
                "the part is shifted by that much."
            )
