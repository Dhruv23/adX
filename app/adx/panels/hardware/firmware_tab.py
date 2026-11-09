"""The FM-1's firmware (phase_11.5.md 4.11): the catalog with each entry's licence and
risk notes, an install behind a typed confirmation, and the recovery guide.
"""

from __future__ import annotations

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QListWidget,
    QProgressBar,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from adx.hardware.fm1.firmware import FirmwareCatalog, FirmwarePackage
from adx.hardware.fm1.flasher import State
from adx.hardware.fm1.manager import Fm1Manager
from adx.hardware.fm1.recovery import guide

#: Where the catalog is fetched from; a manifest in adX's format (firmware.py).
DEFAULT_MANIFEST = ""


class FirmwareTab(QWidget):
    def __init__(self, manager: Fm1Manager, catalog: FirmwareCatalog | None = None) -> None:
        super().__init__()
        self.manager = manager
        self.catalog = catalog or FirmwareCatalog()
        self.data: bytes | None = None
        layout = QVBoxLayout(self)
        row = QHBoxLayout()
        self.url = QLineEdit(DEFAULT_MANIFEST)
        self.url.setObjectName("fm1.firmware.url")
        self.url.setPlaceholderText("Catalog manifest URL (empty: the cached catalog)")
        row.addWidget(self.url, 1)
        load = QPushButton("Load catalog")
        load.setObjectName("fm1.firmware.load")
        load.clicked.connect(self.load)
        row.addWidget(load)
        layout.addLayout(row)
        self.list = QListWidget()
        self.list.setObjectName("fm1.firmware.list")
        self.list.currentRowChanged.connect(self._chosen)
        layout.addWidget(self.list, 1)
        self.details = QLabel("")
        self.details.setObjectName("fm1.firmware.details")
        self.details.setWordWrap(True)
        layout.addWidget(self.details)
        self.install_button = QPushButton("Install…")
        self.install_button.setObjectName("fm1.firmware.install")
        self.install_button.clicked.connect(self.install_dialog)
        layout.addWidget(self.install_button)
        self.progress = QProgressBar()
        self.progress.setObjectName("fm1.firmware.progress")
        layout.addWidget(self.progress)
        self.guide = QLabel("")
        self.guide.setObjectName("fm1.firmware.guide")
        self.guide.setWordWrap(True)
        layout.addWidget(self.guide)
        self._timer = QTimer(self)
        self._timer.setInterval(200)
        self._timer.timeout.connect(self.follow)

    def load(self) -> int:
        entries = self.catalog.load(self.url.text().strip() or None)
        self.list.clear()
        for entry in entries:
            kind = "" if entry.installable else "  (link only)"
            self.list.addItem(f"{entry.name} {entry.version} - {entry.source}{kind}")
        if not entries:
            self.details.setText("No catalog yet: give a manifest URL and load it.")
        return len(entries)

    def chosen(self) -> FirmwarePackage | None:
        row = self.list.currentRow()
        return self.catalog.entries[row] if 0 <= row < len(self.catalog.entries) else None

    def _chosen(self, _row: int) -> None:
        p = self.chosen()
        if p is None:
            return
        self.details.setText(
            f"{p.name} {p.version} from {p.source}. Licence: {p.license or 'not stated'}. "
            f"{'Open source.' if p.open_source else 'Closed: a link only.'} {p.risk} "
            f"{p.page}"
        )
        self.install_button.setEnabled(p.installable)

    def prepare(self) -> bool:
        """Download, verify and gate the chosen package: True once it may be confirmed."""
        p = self.chosen()
        if p is None:
            return False
        data, problem = self.catalog.download(p)
        if data is None:
            self.details.setText(problem)
            return False
        flasher = self.manager.new_flasher()
        if not flasher.verify(p, data):
            self.details.setText(flasher.message)
            return False
        self.data = data
        return True

    def confirm_and_start(self, typed: str) -> bool:
        flasher = self.manager.flasher
        if flasher is None or not flasher.confirm(typed):
            self.details.setText(flasher.message if flasher else "")
            return False
        if not flasher.start(self.manager.port_out or self.manager.port_in, self.manager.identity):
            self.details.setText(flasher.message)
            return False
        self._timer.start()
        return True

    def install_dialog(self) -> None:
        if not self.prepare() or self.manager.flasher is None:
            return
        typed, ok = QInputDialog.getText(self, "Install firmware", self.manager.flasher.warning())
        if ok:
            self.confirm_and_start(typed)

    def follow(self) -> None:
        flasher = self.manager.flasher
        if flasher is None:
            return
        if flasher.server is not None:
            self.progress.setValue(int(flasher.server.progress * 100))
        self.details.setText(f"{flasher.state.value}: {flasher.message}")
        if flasher.state is State.DONE and self.data is not None and flasher.package is not None:
            self.catalog.remember_flashed(flasher.package, self.data)
            self._timer.stop()
        elif flasher.state is State.INTERRUPTED:
            self.guide.setText(flasher.message)
            self._timer.stop()
        elif flasher.state is State.REFUSED:
            self._timer.stop()

    def show_guide(self) -> None:
        self.guide.setText(guide(self.manager.state))
