"""The effect panel: one mixer slot's effect, edited (phase_6.md 4.4).

Twenty-seven effects, one generic editor; hand-built overviews only where a grid of knobs
genuinely fails (``editors/``). The header holds the slot's own fields - its effect type,
wet/dry and bypass - and loads an effect preset onto it (one undo step).
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QLabel,
    QPushButton,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from adx import engine_bridge
from adx.panels.base import DockPanel
from adx.panels.effect.editors import editor_for
from adx.panels.effect.generic_editor import slot_info
from adx.selection import Kind, Selection
from adx.session import Session
from adx.theme.tokens import Theme
from adx.widgets.knob import Knob
from adx.widgets.step_button import StepButton


class EffectPanel(DockPanel):
    """One slot's effect."""

    panel_id = "effect"
    title = "Effect"
    default_area = Qt.DockWidgetArea.RightDockWidgetArea

    def __init__(
        self, session: Session, selection: Selection, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.selection = selection
        self.insert = 0
        self.slot = 0
        self._shown: tuple[int, str] = (0, "")
        body = QWidget()
        self.layout_ = QVBoxLayout(body)
        self.layout_.setContentsMargins(0, 0, 0, 0)
        bar = QToolBar()
        self.name = QLabel("No effect")
        bar.addWidget(self.name)
        self.type_box = QComboBox()
        self.type_box.addItems(
            [str(t["name"]) for t in engine_bridge.type_catalog() if t["kind"] == "effect"]
        )
        self.type_box.activated.connect(self._type_chosen)
        bar.addWidget(self.type_box)
        self.mix = Knob(0.0, 1.0, 1.0, 1.0, "mix")
        self.mix.setFixedSize(40, 44)
        self.mix.value_changed.connect(
            lambda v: self.session.set_param(f"insert.{self.insert}.slot.{self.slot}.mix", v)
        )
        bar.addWidget(self.mix)
        self.bypass = StepButton("B", "Bypass")
        self.bypass.toggled_by_user.connect(
            lambda on: self.session.build_and_execute("SetSlotBypass", self.slot, on)
        )
        bar.addWidget(self.bypass)
        preset = QPushButton("Load preset…")
        preset.clicked.connect(self.load_preset_dialog)
        bar.addWidget(preset)
        self.layout_.addWidget(bar)
        self.editor: QWidget | None = None
        self.setWidget(body)
        session.changed.connect(self.refresh)
        selection.changed.connect(self._selection_changed)

    def _selection_changed(self) -> None:
        slots = self.selection.ids(Kind.SLOTS)
        if len(slots) == 1 and slots[0] != self.slot:
            self.show_slot(self.selection.current.context.insert, slots[0])

    def show_slot(self, insert: int, slot: int) -> None:
        """Edit slot ``slot`` of insert ``insert``."""
        self.insert, self.slot = insert, slot
        self.refresh()

    def refresh(self) -> None:
        """Re-read; rebuild the editor only when the slot or its effect changed."""
        if self.session.project is None or not self.slot:
            return
        info = slot_info(self.session, self.insert, self.slot)
        if info is None:
            if self.editor is not None:
                self.editor.setParent(None)
                self.editor.deleteLater()
                self.editor = None
            self.name.setText("No effect")
            self._shown = (0, "")
            return
        effect = str(info["type"])
        self.name.setText(f"  insert {self.insert} · slot {self.slot}  ")
        self.type_box.setCurrentText(effect)
        self.mix.set_value(float(info["mix"]))
        self.bypass.set_checked(bool(info["bypass"]))
        if self._shown != (self.slot, effect) or self.editor is None:
            if self.editor is not None:
                self.editor.setParent(None)
                self.editor.deleteLater()
            self.editor = editor_for(self.session, self.insert, self.slot, effect)
            self.layout_.addWidget(self.editor, 1)
            self._shown = (self.slot, effect)
        else:
            refresh = getattr(self.editor, "refresh", None)
            if refresh is not None:
                refresh()

    def _type_chosen(self, _index: int) -> None:
        if self.slot:
            self.session.call(
                self.session.require().set_slot_effect, self.slot, self.type_box.currentText()
            )

    def load_preset_dialog(self) -> None:
        """Choose an effect preset and apply it to this slot."""
        path, _filter = QFileDialog.getOpenFileName(
            self, "Load preset", "", "adX presets (*.adxpreset)"
        )
        if path and self.slot:
            self.session.call(
                self.session.require().apply_effect_preset, self.insert, self.slot, path
            )

    def apply_theme(self, theme: Theme) -> None:
        """Nothing themed beyond the stylesheet."""
