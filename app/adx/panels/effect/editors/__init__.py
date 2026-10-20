"""Hand-built effect editors, only where the generic one is inadequate (phase_6.md 4.4):
the EQs' curve and spectrum, the dynamics' transfer curve and gain-reduction meter,
convolution's impulse response, GrossBeat's time map - and pictures of the delay's
repeats and the reverb's tail. Each is an overview tab beside the generic editor's full
grid; every other effect is the generic editor alone.
"""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtWidgets import QWidget

from adx.panels.editor_parts import TabbedEditor
from adx.panels.effect.editors.comp import DynamicsView
from adx.panels.effect.editors.convolution import ImpulseView
from adx.panels.effect.editors.eq import EqView
from adx.panels.effect.editors.grossbeat import TimeMap
from adx.panels.effect.editors.time_effects import DelayTaps, ReverbDecay
from adx.panels.effect.generic_editor import generic_for_slot
from adx.session import Session

View = Callable[[Session, int, int, str], QWidget]

#: The effect types with a hand-built overview.
VIEWS: dict[str, View] = {
    "ParametricEq": EqView,
    "EQ": EqView,
    "Compressor": DynamicsView,
    "MultibandComp": DynamicsView,
    "Limiter": DynamicsView,
    "Convolution": lambda s, i, slot, _e: ImpulseView(s, i, slot),
    "GrossBeat": lambda s, i, slot, _e: TimeMap(s, i, slot),
    "Delay": lambda s, i, slot, _e: DelayTaps(s, i, slot),
    "Reverb": lambda s, i, slot, _e: ReverbDecay(s, i, slot),
}


def editor_for(session: Session, insert: int, slot: int, effect: str) -> QWidget:
    """The editor for a slot: hand-built overview + generic grid, or the generic grid."""
    generic = generic_for_slot(session, insert, slot, effect)
    view = VIEWS.get(effect)
    if view is None:
        return generic
    return TabbedEditor(view(session, insert, slot, effect), generic)
