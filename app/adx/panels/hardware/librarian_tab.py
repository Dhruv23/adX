"""The FM-1 librarian (phase_11.5.md 4.4): banks and voices from a `.syx` file, a live
dump or the V16 Bank Export; audition, rename, open as a dx7 channel, send back, export.
"""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

from PySide6.QtWidgets import (
    QFileDialog,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QListWidget,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from adx.hardware import dx7
from adx.hardware.fm1.manager import Fm1Manager


class LibrarianTab(QWidget):
    """One list of voices (the bank last received or opened)."""

    def __init__(self, manager: Fm1Manager, audition: Callable[[str], None] | None = None) -> None:
        super().__init__()
        self.manager = manager
        self.audition_hook = audition
        self.voices: list[list[int]] = []
        self.source = ""
        layout = QVBoxLayout(self)
        row = QHBoxLayout()
        for label, slot, name in (
            ("Import .syx…", self.import_dialog, "import"),
            ("Receive from FM-1", self.receive, "receive"),
            ("Export .syx…", self.export_dialog, "export"),
        ):
            button = QPushButton(label)
            button.setObjectName(f"fm1.librarian.{name}")
            button.clicked.connect(slot)
            row.addWidget(button)
        layout.addLayout(row)
        self.status = QLabel("No voices yet: import a .syx, or receive from the FM-1.")
        self.status.setObjectName("fm1.librarian.status")
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        self.list = QListWidget()
        self.list.setObjectName("fm1.librarian.voices")
        self.list.itemDoubleClicked.connect(lambda _item: self.open_selected())
        layout.addWidget(self.list, 1)
        actions = QHBoxLayout()
        buttons: tuple[tuple[str, Callable[[], object], str], ...] = (
            ("Open as dx7 channel", self.open_selected, "open"),
            ("Audition", self.audition, "audition"),
            ("Rename…", self.rename_dialog, "rename"),
            ("Send voice", self.send_voice, "send_voice"),
            ("Send bank", self.send_bank, "send_bank"),
        )
        for label, handler, name in buttons:
            button = QPushButton(label)
            button.setObjectName(f"fm1.librarian.{name}")
            button.clicked.connect(handler)
            actions.addWidget(button)
        layout.addLayout(actions)
        manager.received.connect(self._received)
        manager.changed.connect(self._state)

    # --- getting voices ---------------------------------------------------------------

    def show_voices(self, source: str, voices: list[list[int]], bad_checksums: int = 0) -> None:
        self.voices = [list(v) for v in voices]
        self.source = source
        self.list.clear()
        for index, voice in enumerate(self.voices):
            self.list.addItem(f"{index + 1:3d}  {dx7.name_of(voice)}")
        if self.voices:
            self.list.setCurrentRow(0)
        warning = f" ({bad_checksums} with a bad checksum)" if bad_checksums else ""
        self.status.setText(f"{len(self.voices)} voices from {source}{warning}.")

    def import_file(self, path: str | Path) -> int:
        read = dx7.read_file(path)
        voices = read["voices"]
        bad = int(read["bad_checksums"])
        if bad and not self._open_anyway(bad):
            return 0
        self.show_voices(Path(path).name, voices, bad)
        if int(read["clamped"]):
            self.status.setText(self.status.text() + f" {read['clamped']} values were clamped.")
        return len(voices)

    def _open_anyway(self, bad: int) -> bool:
        answer = QMessageBox.question(
            self,
            "Bad checksum",
            f"{bad} message(s) in this file fail their checksum. Open the voices anyway?",
        )
        return answer == QMessageBox.StandardButton.Yes

    def import_dialog(self) -> None:
        path, _filter = QFileDialog.getOpenFileName(
            self, "Import DX7 voices", "", "SysEx (*.syx);;All files (*)"
        )
        if path:
            self.import_file(path)

    def receive(self) -> None:
        if not self.manager.port_out:
            self.status.setText("No FM-1 connected.")
            return
        self.manager.request_bank()
        self.status.setText("Asked the FM-1 for its bank…")

    def _received(self, source: str, _count: int) -> None:
        name, voices = self.manager.inbox.banks[-1]
        self.show_voices(name or source, voices, self.manager.inbox.bad_checksums)

    def _state(self) -> None:
        if self.manager.waiting_for_export:
            self.status.setText(
                "The FM-1 does not answer dump requests. Press Bank Export on the FM-1 "
                "(V16): adX is listening."
            )

    # --- one voice --------------------------------------------------------------------

    def selected(self) -> list[int] | None:
        row = self.list.currentRow()
        return self.voices[row] if 0 <= row < len(self.voices) else None

    def open_selected(self) -> str:
        voice = self.selected()
        if voice is None:
            return ""
        name = dx7.open_as_channel(self.manager.session, voice)
        self.status.setText(f"Opened {dx7.name_of(voice).strip()} as the dx7 channel {name}.")
        return name

    def audition(self) -> None:
        voice = self.selected()
        if voice is not None and self.audition_hook is not None:
            self.audition_hook(self.open_selected())

    def rename(self, name: str) -> None:
        row = self.list.currentRow()
        if 0 <= row < len(self.voices):
            self.voices[row] = dx7.with_name(self.voices[row], name)
            self.list.item(row).setText(f"{row + 1:3d}  {dx7.name_of(self.voices[row])}")

    def rename_dialog(self) -> None:
        voice = self.selected()
        if voice is None:
            return
        name, ok = QInputDialog.getText(
            self, "Rename voice", "Name (10 characters)", text=dx7.name_of(voice).strip()
        )
        if ok:
            self.rename(name[:10])

    def send_voice(self) -> None:
        voice = self.selected()
        if voice is not None and self.manager.device is not None:
            self.manager.device.send_voice(voice, dx7.name_of(voice))

    def send_bank(self) -> None:
        if self.voices and self.manager.device is not None:
            bank = self.voices[:32]
            self.manager.device.send_bank(bank, [dx7.name_of(v) for v in bank])

    # --- out --------------------------------------------------------------------------

    def export(self, path: str | Path, only_selected: bool = False) -> Path:
        voice = self.selected()
        if only_selected and voice is not None:
            return dx7.write_voice(path, voice)
        return dx7.write_bank(path, self.voices)

    def export_dialog(self) -> None:
        if not self.voices:
            return
        path, _filter = QFileDialog.getSaveFileName(self, "Export DX7 bank", "", "SysEx (*.syx)")
        if path:
            self.export(path)
