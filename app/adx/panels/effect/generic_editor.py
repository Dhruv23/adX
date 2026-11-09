"""The generic editor bound to one effect slot (phase_6.md 4.4).

Paths are ``insert.<n>.slot.<m>.<parameter>``; a parameter the slot does not hold yet
plays its default, and its first turn sets it with a command.
"""

from __future__ import annotations

import math
from typing import Any

from adx import engine_bridge
from adx.panels.automation.lane import automated_paths, create_lane
from adx.panels.params import GenericEditor, ParamTarget, arm_learn, slot_path
from adx.session import Session


def effect_descriptors(effect: str) -> list[dict[str, Any]]:
    """An effect type's descriptors (the engine's static table)."""
    entry = next(
        (t for t in engine_bridge.type_catalog() if t["kind"] == "effect" and t["name"] == effect),
        None,
    )
    return list(entry["params"]) if entry is not None else []


def plugin_descriptors(plugin: dict[str, Any] | None, format_name: str) -> list[dict[str, Any]]:
    """A hosted plugin's parameters as generic-editor descriptors, from the scan cache
    (the plugin is described, never instantiated, for this)."""
    if plugin is None:
        return []
    described = engine_bridge.Plugins.describe(format_name, str(plugin["uid"]), str(plugin["path"]))
    if described is None:
        return []
    out: list[dict[str, Any]] = []
    for param in described["params"]:
        if param["hidden"]:
            continue
        steps = int(param["steps"])
        low, high = float(param["min"]), float(param["max"])
        # Plugins report what they report: Airwindows Consolidated's unused slots give
        # uninitialised defaults like 3e32, which a knob cannot draw.
        default = float(param["default"])
        default = min(high, max(low, default)) if math.isfinite(default) else low
        out.append(
            {
                "name": str(param["key"]),
                "label": str(param["name"]),
                "min": low,
                "max": high,
                "default": default,
                "unit": "count" if steps else "normalized",
                "scale": "stepped" if steps else "linear",
                "rate": "sample",
                "curve_part": 0,
            }
        )
    return out


def slot_descriptors(info: dict[str, Any] | None) -> list[dict[str, Any]]:
    """A slot's descriptors: the catalog's for a native effect, the plugin's own for a
    hosted one."""
    if info is None:
        return []
    effect = str(info["type"])
    if effect in ("clap", "vst3"):
        return plugin_descriptors(info.get("plugin"), effect)
    return effect_descriptors(effect)


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

    def ensure(parameter: str) -> str:
        """The parameter's path, set to its default first if the slot lacks it."""
        path = slot_path(insert, slot, parameter)
        if not session.require().param_valid(path):
            info = slot_info(session, insert, slot)
            default = next(
                (float(d["default"]) for d in slot_descriptors(info) if d["name"] == parameter),
                0.0,
            )
            session.build_and_execute("SetSlotParam", slot, parameter, default)
        return path

    def automate(parameter: str) -> None:
        with session.group(f"Automate {parameter}"):
            create_lane(session, ensure(parameter))

    def learn(parameter: str) -> None:
        arm_learn(ensure(parameter))

    return ParamTarget(
        path_of=lambda parameter: slot_path(insert, slot, parameter),
        read_values=read_values,
        turn=turn,
        automate=automate,
        learn=learn,
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
    """The generic editor for a slot - a hosted plugin's too, from its own parameter
    list (phase_9.md 4.6: the fallback that is always there)."""
    descriptors = (
        slot_descriptors(slot_info(session, insert, slot))
        if effect in ("clap", "vst3")
        else effect_descriptors(effect)
    )
    return GenericEditor(descriptors, slot_target(session, insert, slot), type_name=effect)
