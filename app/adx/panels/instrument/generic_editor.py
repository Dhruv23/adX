"""The generic editor bound to a channel's instrument (phase_6.md 4.4).

Paths are ``channel.<name>.<parameter>``; values are read in one call per refresh; a turn
is :meth:`Session.set_param`; "Create automation" makes a lane on that path.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from adx import engine_bridge
from adx.panels.automation.lane import automated_paths, create_lane
from adx.panels.params import GenericEditor, ParamTarget, channel_path
from adx.session import Session


def descriptors_of(instrument: str) -> list[dict[str, Any]]:
    """An instrument type's descriptors (the engine's static table)."""
    entry = next(
        (
            t
            for t in engine_bridge.type_catalog()
            if t["kind"] == "instrument" and t["name"] == instrument
        ),
        None,
    )
    return list(entry["params"]) if entry is not None else []


def channel_info(session: Session, channel: int) -> dict[str, Any] | None:
    """A channel's settings, read now."""
    return next((c for c in session.require().channels_info() if int(c["id"]) == channel), None)


def channel_target(session: Session, channel: int) -> ParamTarget:
    """Where a channel's instrument parameters live."""

    def name() -> str:
        info = channel_info(session, channel)
        return "" if info is None else str(info["name"])

    def read_values() -> dict[str, float]:
        info = channel_info(session, channel)
        if info is None:
            return {}
        return {str(p[0]): float(p[1]) for p in info["params"]}

    def turn(parameter: str, value: float) -> None:
        path = channel_path(name(), parameter)
        if session.require().param_valid(path):
            session.set_param(path, value)
        else:
            # A parameter the model does not hold yet (it plays its default): set it, as
            # a command, after which it is a path like any other.
            session.build_and_execute("SetChannelParam", channel, parameter, value)

    def automate(parameter: str) -> None:
        path = channel_path(name(), parameter)
        with session.group(f"Automate {parameter}"):
            if not session.require().param_valid(path):
                default = next(
                    (
                        float(d["default"])
                        for d in descriptors_of(instrument_of(session, channel))
                        if d["name"] == parameter and int(d["curve_part"]) == 0
                    ),
                    0.0,
                )
                session.build_and_execute("SetChannelParam", channel, parameter, default)
            create_lane(session, path)

    return ParamTarget(
        path_of=lambda parameter: channel_path(name(), parameter),
        read_values=read_values,
        turn=turn,
        automate=automate,
        automated_paths=lambda: automated_paths(session),
    )


def instrument_of(session: Session, channel: int) -> str:
    """A channel's instrument type, read now."""
    info = channel_info(session, channel)
    return "" if info is None else str(info["instrument"])


SetCurve = Callable[[str, float, int, float], None]


def set_curve_callback(session: Session, channel: int) -> SetCurve:
    """An envelope stage's shape: a command (curves are not automatable)."""

    def set_curve(parameter: str, value: float, kind: int, tension: float) -> None:
        session.build_and_execute("SetChannelParamCurve", channel, parameter, value, kind, tension)

    return set_curve


def generic_for(session: Session, channel: int, instrument: str) -> GenericEditor:
    """The generic editor for a channel's instrument."""
    return GenericEditor(descriptors_of(instrument), channel_target(session, channel))
