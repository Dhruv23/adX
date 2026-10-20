"""Drum synth: its tone and decay on a pad above the generic grid - the model, pitch drop
and the rest are a combo and knobs the generic editor already does well."""

from __future__ import annotations

from adx.panels.editor_parts import TabbedEditor
from adx.panels.instrument.editors.common import tabbed
from adx.panels.instrument.editors.va import FilterPad
from adx.panels.instrument.generic_editor import channel_target
from adx.session import Session


def drumsynth_editor(session: Session, channel: int) -> TabbedEditor:
    """Tone (across) and decay (up) on a pad, beside every parameter."""
    pad = FilterPad(channel_target(session, channel), "drumsynth", "tone", "decay")
    return tabbed(session, channel, "drumsynth", pad)
