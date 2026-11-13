"""File > Get sound libraries (phase_12.md 4.8): what the library store holds, and a
button to fetch or verify each library.

The work is tools/packs/fetch_libraries.py's - a pinned shallow clone and a SHA-256
check of every file - run as a child process so the window stays live while gigabytes
come down. Its output streams into the log. The dialog is not modal: a fetch can take
an hour, and the rest of adX stays usable meanwhile.
"""

from __future__ import annotations

import sys
from pathlib import Path

from PySide6.QtCore import QProcess, Qt
from PySide6.QtGui import QCloseEvent
from PySide6.QtWidgets import (
    QDialog,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QPlainTextEdit,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

REPO = Path(__file__).resolve().parents[4]
TOOL = REPO / "tools" / "packs" / "fetch_libraries.py"


def libraries() -> list[dict[str, object]]:
    """Every library the store knows: id, title, size, licence, whether it is fetched."""
    sys.path.insert(0, str(TOOL.parent))
    try:
        import fetch_libraries as fl  # type: ignore[import-not-found]
    finally:
        sys.path.pop(0)
    root = fl.store_root()
    return [
        {
            "id": lib.id,
            "title": lib.title,
            "gigabytes": lib.gigabytes,
            "licence": lib.licence,
            "redistributable": lib.redistributable,
            "fetched": (root / lib.folder).is_dir(),
            "folder": str(root / lib.folder),
        }
        for lib in fl.LIBRARIES.values()
    ]


class LibraryDialog(QDialog):
    """The store's libraries, one row each, and a log of the last fetch or verify."""

    COLUMNS = ("Library", "Size", "Licence", "State", "")

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Get sound libraries")
        self.setModal(False)
        self.resize(760, 460)
        self.process: QProcess | None = None
        self.last_exit: int | None = None
        self.available = TOOL.is_file()

        intro = QLabel(
            "Sample libraries are fetched once into your library store, pinned to one "
            "version and checked file by file. Presets name them as lib://, so a project "
            "opens on any machine that has them."
        )
        intro.setWordWrap(True)
        self.table = QTableWidget(0, len(self.COLUMNS))
        self.table.setHorizontalHeaderLabels(list(self.COLUMNS))
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.table.verticalHeader().setVisible(False)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setPlaceholderText("Fetch or verify a library; its progress shows here.")
        self.status = QLabel("")
        close = QPushButton("Close")
        close.clicked.connect(self.close)
        foot = QHBoxLayout()
        foot.addWidget(self.status, 1)
        foot.addWidget(close)

        layout = QVBoxLayout(self)
        layout.addWidget(intro)
        layout.addWidget(self.table, 2)
        layout.addWidget(self.log, 1)
        layout.addLayout(foot)
        self.refresh()
        if not self.available:
            self.status.setText(
                "This copy of adX has no fetch tool (tools/packs/fetch_libraries.py)."
            )

    # --- rows --------------------------------------------------------------------------

    def refresh(self) -> None:
        """Re-read the store and rebuild the rows."""
        rows = libraries()
        self.table.setRowCount(len(rows))
        for r, lib in enumerate(rows):
            licence = str(lib["licence"]) + ("" if lib["redistributable"] else " (fetched only)")
            cells = (
                str(lib["title"]),
                f"{float(str(lib['gigabytes'])):.1f} GB",
                licence,
                "fetched" if lib["fetched"] else "not fetched",
            )
            for c, text in enumerate(cells):
                item = QTableWidgetItem(text)
                item.setData(Qt.ItemDataRole.UserRole, lib["id"])
                self.table.setItem(r, c, item)
            buttons = QWidget()
            row = QHBoxLayout(buttons)
            row.setContentsMargins(2, 0, 2, 0)
            fetch = QPushButton("Verify" if lib["fetched"] else "Fetch")
            fetch.setObjectName(f"run-{lib['id']}")
            command = "verify" if lib["fetched"] else "fetch"
            fetch.clicked.connect(lambda _=False, i=str(lib["id"]), c=command: self.run(c, i))
            fetch.setEnabled(self.available and self.process is None)
            row.addWidget(fetch)
            self.table.setCellWidget(r, len(self.COLUMNS) - 1, buttons)

    def state_of(self, library: str) -> str:
        """The State cell of `library`'s row, as shown."""
        for r in range(self.table.rowCount()):
            item = self.table.item(r, 3)
            if item is not None and item.data(Qt.ItemDataRole.UserRole) == library:
                return item.text()
        return ""

    # --- running the tool ----------------------------------------------------------------

    def run(self, command: str, library: str) -> None:
        """Start `fetch_libraries.py command library`, streaming its output."""
        if self.process is not None or not self.available:
            return
        self.log.appendPlainText(f"$ fetch_libraries.py {command} {library}")
        process = QProcess(self)
        process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        process.readyReadStandardOutput.connect(self._output)
        process.finished.connect(self._finished)
        self.process = process
        self.last_exit = None
        self.status.setText(f"{command} {library}…")
        process.start(sys.executable, [str(TOOL), command, library])
        self.refresh()

    def _output(self) -> None:
        if self.process is not None:
            text = bytes(self.process.readAllStandardOutput().data()).decode("utf-8", "replace")
            self.log.appendPlainText(text.rstrip("\n"))

    def _finished(self, code: int, _status: QProcess.ExitStatus) -> None:
        self._output()
        self.last_exit = code
        self.process = None
        self.status.setText("done" if code == 0 else f"failed (exit {code}); see the log")
        self.refresh()

    def closeEvent(self, event: QCloseEvent) -> None:  # noqa: N802 - Qt's name
        """A running fetch keeps going in its own process; the dialog only stops
        listening to it."""
        if self.process is not None:
            self.process.readyReadStandardOutput.disconnect()
            self.process.finished.disconnect()
            self.process = None
        super().closeEvent(event)
