"""The FM-1's effects in adX (phase_11.5.md 4.7): its named FX state as the native
Fm1Effects, whose parameters are the profile's FX names in lower case with the same
0..127 values, so "Match in adX" is a copy, value for value.
"""

from __future__ import annotations

from collections.abc import Callable

from adx import engine_bridge
from adx.hardware.fm1.device import FX_DEFAULTS as DEFAULTS  # noqa: F401 - re-exported
from adx.selection import Kind, Selection
from adx.session import Session

EFFECT = "Fm1Effects"


def param_name(fx: str) -> str:
    """A profile FX name as the Fm1Effects parameter: FILTER_CUTOFF -> filter_cutoff."""
    return fx.lower()


def as_params(fx: dict[str, int]) -> dict[str, float]:
    return {param_name(name): float(value) for name, value in fx.items()}


def target_insert(session: Session, selection: Selection) -> int | None:
    """The insert of the selected channel, else of the first dx7 channel."""
    project = session.require()
    channels = project.channels_info()
    chosen = set(selection.ids(Kind.CHANNELS))
    pick = next((c for c in channels if int(c["id"]) in chosen), None)
    if pick is None:
        pick = next((c for c in channels if c["instrument"] == "dx7"), None)
    if pick is None:
        return None
    output = int(pick["output"])
    return output if output > 0 else None


def match_in_adx(
    session: Session,
    selection: Selection,
    fx: dict[str, int],
    report: Callable[[str], None] = lambda _m: None,
) -> bool:
    """An Fm1Effects with ``fx``'s values at the end of the chosen channel's insert, as
    one undo step."""
    insert = target_insert(session, selection)
    if insert is None:
        report("Match in adX: select a channel that feeds a mixer insert first")
        return False
    slot = session.call(
        engine_bridge.script_add_effect, session.require(), insert, EFFECT, as_params(fx), 1.0
    )
    if slot is None:
        return False
    report(f"FM-1 effects matched on insert {insert}")
    return True
