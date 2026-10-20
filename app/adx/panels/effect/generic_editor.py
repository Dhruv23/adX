"""The generic editor bound to one effect slot (phase_6.md 4.4).

Paths are ``insert.<n>.slot.<m>.<parameter>``; a parameter the slot does not hold yet
plays its default, and its first turn sets it with a command.
"""

from __future__ import annotations

from typing import Any

from adx import engine_bridge
from adx.panels.automation.lane import automated_paths, create_lane
from adx.panels.params import GenericEditor, ParamTarget, slot_path
from adx.session import Session


def effect_descriptors(effect: str) -> list[dict[str, Any]]:
    """An effect type's descriptors (the engine's static table)."""
    entry = next(
        (t for t in engine_bridge.type_catalog() if t["kind"] == "effect" and t["name"] == effect),
        None,
    )
    return list(entry["params"]) if entry is not None else []


def slot_info(session: Session, insert: int, slot: int) -> dict[str, Any] | None:
    """A slot's settings, read now."""
    for entry in session.require().mixer_info()["inserts"]:
        if int(entry["id"]) == insert:
            return next((s for s in entry["slots"] if int(s["id"]) == slot), None)
    return None


def slot_target(session: Session, insert: int, slot: int) -> ParamTarget:
    """Where a slot's effect parameters live."""

    def read_values() -> dict[str, float]:
        info = slot_info(session, insert, slot)
        if info is None:
            return {}
        return {str(name): float(value) for name, value in info["params"]}

    def turn(parameter: str, value: float) -> None:
        path = slot_path(insert, slot, parameter)
        if session.require().param_valid(path):
            session.set_param(path, value)
        else:
            session.build_and_execute("SetSlotParam", slot, parameter, value)

    def automate(parameter: str) -> None:
        path = slot_path(insert, slot, parameter)
        with session.group(f"Automate {parameter}"):
            if not session.require().param_valid(path):
                info = slot_info(session, insert, slot)
                effect = "" if info is None else str(info["type"])
                default = next(
                    (
                        float(d["default"])
                        for d in effect_descriptors(effect)
                        if d["name"] == parameter
                    ),
                    0.0,
                )
                session.build_and_execute("SetSlotParam", slot, parameter, default)
            create_lane(session, path)

    return ParamTarget(
        path_of=lambda parameter: slot_path(insert, slot, parameter),
        read_values=read_values,
        turn=turn,
        automate=automate,
        automated_paths=lambda: automated_paths(session),
    )


def params_in_order(session: Session, insert: int, slot: int, effect: str) -> list[float]:
    """The slot's values in descriptor order (defaults where unset): what the effect's
    node reads, and what the EQ curve is computed from."""
    values = slot_target(session, insert, slot).read_values()
    return [
        values.get(str(d["name"]), float(d["default"]))
        for d in effect_descriptors(effect)
        if int(d["curve_part"]) == 0
    ]


def generic_for_slot(session: Session, insert: int, slot: int, effect: str) -> GenericEditor:
    """The generic editor for a slot."""
    return GenericEditor(effect_descriptors(effect), slot_target(session, insert, slot))
