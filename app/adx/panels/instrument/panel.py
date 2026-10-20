"""The instrument panel: the selected channel's instrument, edited (phase_6.md 4.4).

Every instrument gets the generic editor; the ones a grid of knobs cannot do justice
(additive, sampler, slicer, FM, wavetable, and the envelopes anywhere) add a hand-built
overview beside it (``editors/``). The header switches the instrument type, loads a
preset (its samples included, one undo step) and auditions a note.
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
from adx.panels.instrument.editors import editor_for
from adx.panels.instrument.generic_editor import channel_info
from adx.selection import Kind, Selection
from adx.session import Session
from adx.theme.tokens import Theme


class InstrumentPanel(DockPanel):
    """One channel's instrument."""

    panel_id = "instrument"
    title = "Instrument"
    default_area = Qt.DockWidgetArea.RightDockWidgetArea

    def __init__(
        self, session: Session, selection: Selection, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.selection = selection
        self.channel = 0
        self._shown: tuple[int, str] = (0, "")
        body = QWidget()
        self.layout_ = QVBoxLayout(body)
        self.layout_.setContentsMargins(0, 0, 0, 0)
        bar = QToolBar()
        self.name = QLabel("No channel")
        bar.addWidget(self.name)
        self.type_box = QComboBox()
        self.type_box.addItems(
            [str(t["name"]) for t in engine_bridge.type_catalog() if t["kind"] == "instrument"]
        )
        self.type_box.setToolTip("Instrument type")
        self.type_box.activated.connect(self._type_chosen)
        bar.addWidget(self.type_box)
        preset = QPushButton("Load preset…")
        preset.clicked.connect(self.load_preset_dialog)
        bar.addWidget(preset)
        audition = QPushButton("▶ C5")
        audition.setToolTip("Audition the instrument")
        audition.clicked.connect(lambda: self.session.engine.audition(self.channel, 60, 100, 0.5))
        bar.addWidget(audition)
        self.layout_.addWidget(bar)
        self.editor: QWidget | None = None
        self.setWidget(body)
        session.changed.connect(self.refresh)
        selection.changed.connect(self._selection_changed)

    def _selection_changed(self) -> None:
        channels = self.selection.ids(Kind.CHANNELS)
        if len(channels) == 1 and channels[0] != self.channel:
            self.show_channel(channels[0])

    def show_channel(self, channel: int) -> None:
        """Edit ``channel``'s instrument."""
        self.channel = channel
        self.refresh()

    def refresh(self) -> None:
        """Re-read; rebuild the editor only when the channel or its type changed."""
        if self.session.project is None:
            return
        info = channel_info(self.session, self.channel) if self.channel else None
        if info is None:
            channels = self.session.project.channels_info()
            if not channels:
                return
            self.channel = int(channels[0]["id"])
            info = channels[0]
        self.name.setText(f"  {info['name']}  ")
        instrument = str(info["instrument"])
        self.type_box.setCurrentText(instrument)
        if self._shown != (self.channel, instrument) or self.editor is None:
            if self.editor is not None:
                self.editor.setParent(None)
                self.editor.deleteLater()
            self.editor = editor_for(self.session, self.channel, instrument)
            self.layout_.addWidget(self.editor, 1)
            self._shown = (self.channel, instrument)
        else:
            refresh = getattr(self.editor, "refresh", None)
            if refresh is not None:
                refresh()

    def _type_chosen(self, _index: int) -> None:
        if self.channel:
            self.session.build_and_execute(
                "SetInstrumentType", self.channel, self.type_box.currentText()
            )

    def load_preset_dialog(self) -> None:
        """Choose an instrument preset and apply it (its samples included)."""
        path, _filter = QFileDialog.getOpenFileName(
            self, "Load preset", "", "adX presets (*.adxpreset)"
        )
        if path and self.channel:
            self.session.call(self.session.require().apply_preset, self.channel, path)

    def apply_theme(self, theme: Theme) -> None:
        """Nothing themed beyond the stylesheet."""
