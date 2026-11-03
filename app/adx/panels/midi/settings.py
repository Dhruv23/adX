"""The project's MIDI input routes (phase_9.md 4.1): which keys of which input play which
channel, transposed how. With no routes at all, every input plays the selected channel,
which is what a keyboard plugged in expects; add routes to split one keyboard across
channels or to keep a pad controller on the drums.

Devices are opened in Preferences > MIDI; this is what they play.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtWidgets import (
    QComboBox,
    QHBoxLayout,
    QPushButton,
    QSpinBox,
    QTableWidget,
    QVBoxLayout,
    QWidget,
)

from adx.session import Session

COLUMNS = ["Channel", "Low key", "High key", "Transpose"]


class RoutesEditor(QWidget):
    """A table of routes, written back as one command whenever a cell changes."""

    def __init__(self, session: Session, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.session = session
        self._loading = False
        layout = QVBoxLayout(self)
        self.table = QTableWidget(0, len(COLUMNS))
        self.table.setHorizontalHeaderLabels(COLUMNS)
        layout.addWidget(self.table, 1)
        buttons = QHBoxLayout()
        add = QPushButton("Add route")
        add.clicked.connect(self.add_route)
        remove = QPushButton("Remove")
        remove.clicked.connect(self.remove_selected)
        buttons.addWidget(add)
        buttons.addWidget(remove)
        layout.addLayout(buttons)

    def _channels(self) -> list[tuple[int, str]]:
        project = self.session.project
        return (
            []
            if project is None
            else [(int(c["id"]), str(c["name"])) for c in project.channels_info()]
        )

    def refresh(self) -> None:
        project = self.session.project
        self._loading = True
        self.table.setRowCount(0)
        if project is not None:
            for route in project.midi_routes():
                self._append(route)
        self._loading = False

    def _append(self, route: dict[str, Any]) -> None:
        row = self.table.rowCount()
        self.table.insertRow(row)
        channel = QComboBox()
        for channel_id, name in self._channels():
            channel.addItem(name, channel_id)
        index = channel.findData(int(route.get("channel", 0)))
        channel.setCurrentIndex(max(index, 0))
        channel.currentIndexChanged.connect(self.write)
        self.table.setCellWidget(row, 0, channel)
        for column, key, low, high, default in (
            (1, "key_low", 0, 127, 0),
            (2, "key_high", 0, 127, 127),
            (3, "transpose", -48, 48, 0),
        ):
            spin = QSpinBox()
            spin.setRange(low, high)
            spin.setValue(int(route.get(key, default)))
            spin.valueChanged.connect(self.write)
            self.table.setCellWidget(row, column, spin)

    def routes(self) -> list[dict[str, Any]]:
        """The table as the project's route list."""
        out: list[dict[str, Any]] = []
        for row in range(self.table.rowCount()):
            channel = self.table.cellWidget(row, 0)
            low, high, transpose = (self.table.cellWidget(row, c) for c in (1, 2, 3))
            if not isinstance(channel, QComboBox) or channel.currentData() is None:
                continue
            if not (
                isinstance(low, QSpinBox)
                and isinstance(high, QSpinBox)
                and isinstance(transpose, QSpinBox)
            ):
                continue
            out.append(
                {
                    "channel": int(channel.currentData()),
                    "key_low": min(low.value(), high.value()),
                    "key_high": max(low.value(), high.value()),
                    "transpose": transpose.value(),
                }
            )
        return out

    def write(self) -> None:
        if self._loading or self.session.project is None:
            return
        self.session.call(self.session.project.set_midi_routes, self.routes())

    def add_route(self) -> None:
        if not self._channels():
            return
        self._append({"channel": self._channels()[0][0]})
        self.write()

    def remove_selected(self) -> None:
        row = self.table.currentRow()
        if row >= 0:
            self.table.removeRow(row)
            self.write()
