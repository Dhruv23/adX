"""Hand-built instrument editors, only where the generic one is inadequate
(phase_6.md 4.4's table): additive's harmonics, sampler and slicer zones, wavetable's
table, FM's algorithm - and every instrument's envelopes. Each is an overview tab beside
the generic editor's full grid, so no parameter is ever hidden by hand-building.
"""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtWidgets import QWidget

from adx.panels.instrument.editors.additive import additive_editor
from adx.panels.instrument.editors.drumsynth import drumsynth_editor
from adx.panels.instrument.editors.fm import fm_editor
from adx.panels.instrument.editors.granular import granular_editor
from adx.panels.instrument.editors.sampler import sampler_editor
from adx.panels.instrument.editors.slicer import slicer_editor
from adx.panels.instrument.editors.va import va_editor
from adx.panels.instrument.editors.wavetable import wavetable_editor
from adx.panels.instrument.generic_editor import generic_for
from adx.session import Session

Factory = Callable[[Session, int], QWidget]

#: The types with a hand-built overview.
EDITORS: dict[str, Factory] = {
    "additive": additive_editor,
    "sampler": sampler_editor,
    "pool": sampler_editor,
    "slicer": slicer_editor,
    "fm": fm_editor,
    "wavetable": wavetable_editor,
    "va": va_editor,
    "drumsynth": drumsynth_editor,
    "granular": granular_editor,
}


def editor_for(session: Session, channel: int, instrument: str) -> QWidget:
    """The editor for a channel's instrument: hand-built where one exists, generic
    otherwise."""
    factory = EDITORS.get(instrument)
    return (
        factory(session, channel)
        if factory is not None
        else generic_for(session, channel, instrument)
    )
