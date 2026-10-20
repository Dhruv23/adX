"""Granular: where in the source the grains come from (position across) and how far they
scatter (spray up), on a pad above the envelope and the generic grid."""

from __future__ import annotations

from adx.panels.editor_parts import TabbedEditor
from adx.panels.instrument.editors.common import tabbed
from adx.panels.instrument.editors.va import FilterPad
from adx.panels.instrument.generic_editor import channel_target
from adx.session import Session


def granular_editor(session: Session, channel: int) -> TabbedEditor:
    """Position and spray on a pad, beside every parameter."""
    pad = FilterPad(channel_target(session, channel), "granular", "position", "spray")
    return tabbed(session, channel, "granular", pad)
