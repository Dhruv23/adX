"""Slicer: its slices are zones (one per key), so it shares the sampler's zone editor -
the waveform with each slice's start and end to drag (phase_6.md 4.4's table)."""

from __future__ import annotations

from adx.panels.editor_parts import TabbedEditor
from adx.panels.instrument.editors.common import tabbed
from adx.panels.instrument.editors.sampler import ZoneEditor, shared_cache
from adx.session import Session


def slicer_editor(session: Session, channel: int) -> TabbedEditor:
    """Slices (zones) and envelopes, beside every parameter."""
    return tabbed(session, channel, "slicer", ZoneEditor(session, channel, shared_cache()))
