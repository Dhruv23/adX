"""The plugin browser (phase_9.md 4.2): what a scan found, what failed and why, and the
way to put a plugin to use.

Scanning runs out of process - a plugin that crashes or hangs while being loaded takes
its scanner child with it, never adX - and on a worker thread, so the window keeps
drawing while it goes. A file that failed is listed with its reason and stays skipped
until it changes or is rescanned here: the blacklist is visible and editable, because a
plugin that failed once may work after the user updates it.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import QObject, QRunnable, Qt, QThreadPool, Signal
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QPushButton,
    QTabWidget,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from adx.engine_bridge import Plugins
from adx.panels.base import DockPanel
from adx.selection import Kind, Selection
from adx.session import Session

PLUGIN_ROLE = int(Qt.ItemDataRole.UserRole)


class _ScanSignals(QObject):
    finished = Signal(int)


class _ScanJob(QRunnable):
    """One scan, off the UI thread (the engine releases the GIL while it waits)."""

    def __init__(self, force: bool) -> None:
        super().__init__()
        self.force = force
        self.signals = _ScanSignals()

    def run(self) -> None:
        self.signals.finished.emit(Plugins.scan(None, self.force))


def plugin_summary(plugin: dict[str, Any]) -> str:
    """One line about a plugin, for the details label."""
    kind = "instrument" if plugin["instrument"] else "effect"
    editor = "own editor" if plugin["editor"] else "generic editor only"
    latency = f", {plugin['latency']} samples latency" if int(plugin["latency"]) else ""
    vendor = f" by {plugin['vendor']}" if plugin["vendor"] else ""
    return (
        f"{plugin['name']} {plugin['version']}{vendor} - {plugin['format'].upper()} {kind}, "
        f"{plugin['param_count']} parameters, {editor}{latency}\n{plugin['path']}"
    )


class PluginBrowserPanel(DockPanel):
    """Scan status, the plugin list, the blacklist, and use-this-plugin actions."""

    panel_id = "plugins"
    scrolls = True
    title = "Plugins"
    default_area = Qt.DockWidgetArea.LeftDockWidgetArea

    def __init__(
        self, session: Session, selection: Selection, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.selection = selection
        self.scanning = False
        body = QWidget()
        layout = QVBoxLayout(body)
        buttons = QHBoxLayout()
        self.scan_button = QPushButton("Scan")
        self.scan_button.setObjectName("plugins.scan")
        self.scan_button.setToolTip("Scan the CLAP and VST3 folders for new or changed plugins")
        self.scan_button.clicked.connect(lambda: self.scan(False))
        self.rescan_button = QPushButton("Rescan all")
        self.rescan_button.setObjectName("plugins.rescan_all")
        self.rescan_button.clicked.connect(lambda: self.scan(True))
        buttons.addWidget(self.scan_button)
        buttons.addWidget(self.rescan_button)
        layout.addLayout(buttons)
        self.status = QLabel()
        self.status.setObjectName("plugins.status")
        self.status.setWordWrap(True)
        layout.addWidget(self.status)

        self.tabs = QTabWidget()
        self.plugins = QTreeWidget()
        self.plugins.setObjectName("plugins.list")
        self.plugins.setHeaderLabels(["Plugin", "Vendor", "Format"])
        self.plugins.currentItemChanged.connect(self._show_details)
        self.plugins.itemDoubleClicked.connect(lambda _item, _column: self.use_selected())
        self.tabs.addTab(self.plugins, "Installed")
        self.failed = QTreeWidget()
        self.failed.setObjectName("plugins.failed")
        self.failed.setHeaderLabels(["File", "Why it failed"])
        self.tabs.addTab(self.failed, "Failed")
        layout.addWidget(self.tabs, 1)

        self.details = QLabel()
        self.details.setWordWrap(True)
        self.details.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        layout.addWidget(self.details)
        actions = QHBoxLayout()
        self.use_button = QPushButton("Add to insert")
        self.use_button.setObjectName("plugins.use")
        self.use_button.setToolTip(
            "An effect: onto the selected insert. An instrument: as the selected channel's."
        )
        self.use_button.clicked.connect(self.use_selected)
        self.retry_button = QPushButton("Rescan file")
        self.retry_button.clicked.connect(self.retry_selected)
        self.forget_button = QPushButton("Forget")
        self.forget_button.setToolTip("Drop it from the list; the next scan tries it again")
        self.forget_button.clicked.connect(self.forget_selected)
        actions.addWidget(self.use_button)
        actions.addWidget(self.retry_button)
        actions.addWidget(self.forget_button)
        layout.addLayout(actions)
        self.setWidget(body)
        Plugins.load_cache()
        self.refresh()

    # --- scanning -------------------------------------------------------------------

    def scan(self, force: bool) -> None:
        if self.scanning:
            return
        if not Plugins.available():
            self.status.setText(f"The plugin host is missing: {Plugins.host_path()}")
            return
        self.scanning = True
        self.scan_button.setEnabled(False)
        self.rescan_button.setEnabled(False)
        self.status.setText("Scanning (each plugin in a process of its own)...")
        job = _ScanJob(force)
        job.signals.finished.connect(self._scanned)
        QThreadPool.globalInstance().start(job)

    def _scanned(self, count: int) -> None:
        self.scanning = False
        self.scan_button.setEnabled(True)
        self.rescan_button.setEnabled(True)
        self.refresh()
        self.status.setText(f"Scanned {count} file(s). " + self.status.text())

    # --- the lists ------------------------------------------------------------------

    def refresh(self) -> None:
        self.plugins.clear()
        listing = sorted(Plugins.listing(), key=lambda p: (str(p["name"]).lower(), p["format"]))
        groups: dict[str, QTreeWidgetItem] = {}
        for plugin in listing:
            group = "Instruments" if plugin["instrument"] else "Effects"
            if group not in groups:
                groups[group] = QTreeWidgetItem([group])
                self.plugins.addTopLevelItem(groups[group])
            item = QTreeWidgetItem(
                [str(plugin["name"]), str(plugin["vendor"]), str(plugin["format"]).upper()]
            )
            item.setData(0, PLUGIN_ROLE, plugin)
            groups[group].addChild(item)
        self.plugins.expandAll()
        self.failed.clear()
        failures = [r for r in Plugins.scan_results() if not r["ok"]]
        for result in failures:
            item = QTreeWidgetItem([str(result["path"]), str(result["error"])])
            item.setData(0, PLUGIN_ROLE, result)
            self.failed.addTopLevelItem(item)
        self.tabs.setTabText(1, f"Failed ({len(failures)})" if failures else "Failed")
        self.status.setText(f"{len(listing)} plugin(s), {len(failures)} failed.")

    def selected_plugin(self) -> dict[str, Any] | None:
        item = self.plugins.currentItem()
        data = None if item is None else item.data(0, PLUGIN_ROLE)
        return data if isinstance(data, dict) else None

    def _show_details(self, current: QTreeWidgetItem | None, _previous: object) -> None:
        plugin = None if current is None else current.data(0, PLUGIN_ROLE)
        self.details.setText(plugin_summary(plugin) if isinstance(plugin, dict) else "")
        if isinstance(plugin, dict):
            self.use_button.setText(
                "Use as instrument" if plugin["instrument"] else "Add to insert"
            )

    # --- actions --------------------------------------------------------------------

    def use_selected(self) -> bool:
        """The selected plugin onto the selected insert, or as the selected channel's
        instrument."""
        plugin = self.selected_plugin()
        if plugin is None or self.session.project is None:
            return False
        args = (str(plugin["format"]), str(plugin["uid"]), str(plugin["path"]), str(plugin["name"]))
        if plugin["instrument"]:
            channels = self.selection.ids(Kind.CHANNELS)
            if not channels:
                self.status.setText("Select a channel to play the instrument")
                return False
            return self.session.build_and_execute("SetPluginInstrument", int(channels[0]), *args)
        inserts = self.selection.ids(Kind.INSERTS)
        insert = int(inserts[0]) if inserts else int(self.session.require().mixer_info()["master"])
        return self.session.build_and_execute("AddPluginSlot", insert, *args)

    def retry_selected(self) -> None:
        item = (
            self.failed.currentItem()
            if self.tabs.currentIndex() == 1
            else self.plugins.currentItem()
        )
        data = None if item is None else item.data(0, PLUGIN_ROLE)
        if isinstance(data, dict):
            result = Plugins.rescan(str(data["format"]), str(data["path"]))
            self.status.setText(
                f"{result['path']}: " + ("scanned" if result["ok"] else str(result["error"]))
            )
            self.refresh()

    def forget_selected(self) -> None:
        item = (
            self.failed.currentItem()
            if self.tabs.currentIndex() == 1
            else self.plugins.currentItem()
        )
        data = None if item is None else item.data(0, PLUGIN_ROLE)
        if isinstance(data, dict) and Plugins.forget(str(data["path"])):
            self.refresh()
