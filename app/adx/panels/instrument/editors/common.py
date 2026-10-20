"""What every hand-built instrument editor is made of: the generic editor for the channel,
the envelope section, and the overview tab around a type-specific view."""

from __future__ import annotations

from PySide6.QtWidgets import QVBoxLayout, QWidget

from adx.panels.editor_parts import EnvelopeSection, TabbedEditor
from adx.panels.instrument.generic_editor import (
    channel_target,
    descriptors_of,
    generic_for,
    set_curve_callback,
)
from adx.session import Session


class Overview(QWidget):
    """A type-specific view above the envelopes. Refreshes both."""

    def __init__(
        self, session: Session, channel: int, instrument: str, specific: QWidget | None
    ) -> None:
        super().__init__()
        self.specific = specific
        layout = QVBoxLayout(self)
        if specific is not None:
            layout.addWidget(specific, 1)
        descriptors = {
            str(d["name"]): d for d in descriptors_of(instrument) if int(d["curve_part"]) == 0
        }
        self.envelopes = EnvelopeSection(
            descriptors, channel_target(session, channel), set_curve_callback(session, channel)
        )
        layout.addWidget(self.envelopes)

    def refresh(self) -> None:
        """Both parts show the model's values."""
        refresh = getattr(self.specific, "refresh", None)
        if refresh is not None:
            refresh()
        self.envelopes.refresh()


def tabbed(
    session: Session, channel: int, instrument: str, specific: QWidget | None
) -> TabbedEditor:
    """The overview (specific view + envelopes) and the generic editor."""
    return TabbedEditor(
        Overview(session, channel, instrument, specific), generic_for(session, channel, instrument)
    )
