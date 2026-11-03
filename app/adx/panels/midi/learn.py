"""Learned MIDI controls (phase_9.md 3 step 2): the list, and learn mode's state.

A binding is made from any parameter's context menu ("MIDI learn": the next control
that moves is bound to it). This panel shows what is bound, says when learn is waiting,
and forgets a binding. Bindings are the project's - the [MIDI] section - so they save,
diff and undo like everything else.
"""

from __future__ import annotations

from PySide6.QtCore import Qt
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

from adx.panels.base import DockPanel
from adx.panels.midi.settings import RoutesEditor
from adx.plugin_runtime import PluginRuntime, control_text
from adx.session import Session


class MidiPanel(DockPanel):
    """Learned controls and input routes."""

    panel_id = "midi"
    scrolls = True
    title = "MIDI"
    default_area = Qt.DockWidgetArea.LeftDockWidgetArea

    def __init__(
        self, session: Session, runtime: PluginRuntime | None, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.runtime = runtime
        body = QWidget()
        layout = QVBoxLayout(body)
        self.status = QLabel("Right-click a parameter and choose MIDI learn.")
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        tabs = QTabWidget()
        learn = QWidget()
        learn_layout = QVBoxLayout(learn)
        self.bindings = QTreeWidget()
        self.bindings.setHeaderLabels(["Control", "Parameter", "Range"])
        learn_layout.addWidget(self.bindings, 1)
        buttons = QHBoxLayout()
        self.cancel = QPushButton("Cancel learn")
        self.cancel.setEnabled(False)
        self.cancel.clicked.connect(self._cancel)
        self.forget = QPushButton("Forget")
        self.forget.clicked.connect(self.forget_selected)
        buttons.addWidget(self.cancel)
        buttons.addWidget(self.forget)
        learn_layout.addLayout(buttons)
        tabs.addTab(learn, "Learned")
        self.routes = RoutesEditor(session)
        tabs.addTab(self.routes, "Routes")
        layout.addWidget(tabs, 1)
        self.setWidget(body)
        session.changed.connect(self.refresh)
        if runtime is not None:
            runtime.armed.connect(self._armed)
            runtime.learned.connect(self._learned)
        self.refresh()

    def refresh(self) -> None:
        project = self.session.project
        self.bindings.clear()
        if project is None:
            return
        for index, binding in enumerate(project.midi_bindings()):
            control = control_text(
                {
                    "kind": binding["kind"],
                    "number": binding["number"],
                    "channel": binding["channel"],
                }
            )
            if binding["device"]:
                control += f" ({binding['device']})"
            if binding["toggle"]:
                control += ", toggle"
            span = f"{binding['min']:g} .. {binding['max']:g}"
            item = QTreeWidgetItem([control, str(binding["path"]), span])
            item.setData(0, Qt.ItemDataRole.UserRole, index)
            if not binding["valid"]:
                item.setToolTip(1, "This parameter no longer exists; the binding is kept")
            self.bindings.addTopLevelItem(item)
        self.routes.refresh()

    def forget_selected(self) -> bool:
        item = self.bindings.currentItem()
        if item is None or self.session.project is None:
            return False
        index = int(item.data(0, Qt.ItemDataRole.UserRole))
        self.session.call(self.session.project.remove_midi_binding, index)
        return True

    def _armed(self, path: str) -> None:
        self.cancel.setEnabled(bool(path))
        self.status.setText(f"Waiting: move a control for {path}" if path else "Learn finished.")

    def _learned(self, path: str, control: str) -> None:
        self.status.setText(f"{control} now moves {path}")

    def _cancel(self) -> None:
        if self.runtime is not None:
            self.runtime.disarm()
