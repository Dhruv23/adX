"""What an agent reads back from the running app (phase_10.5.md 4.2): the state a user
would look at - the transport, the project and its undo history, the status line and
notices, which panels are open, plugin instances and MIDI ports, the stream's xruns.

``state(window)`` is one dict; ``wait_for`` predicates name a path into it with dots
("transport.playing", "project.undo_depth").
"""

from __future__ import annotations

import hashlib
from typing import TYPE_CHECKING, Any

from PySide6.QtWidgets import QApplication

from adx import engine_bridge
from adx.control.protocol import Json
from adx.panels.analyzers.host import on_screen

if TYPE_CHECKING:
    from adx.mainwindow import MainWindow


def _plugins(window: MainWindow) -> dict[str, Any]:
    scanning = bool(getattr(getattr(window, "plugins", None), "scanning", False))
    runtime = getattr(window, "plugin_runtime", None)
    notices = list(getattr(runtime, "history", []))
    try:
        return {
            "instances": engine_bridge.Plugins.instances(),
            "known": len(engine_bridge.Plugins.listing()),
            "scanning": scanning,
            "notices": notices,
        }
    except (RuntimeError, AttributeError):
        return {"instances": [], "known": 0, "scanning": scanning, "notices": notices}


def state(window: MainWindow) -> dict[str, Any]:
    project = window.session.project
    modal = QApplication.activeModalWidget()
    popup = QApplication.activePopupWidget()
    focus = QApplication.focusWidget()
    text = project.dumps() if project is not None else ""
    engine = window.engine
    clock = window.clock
    out: dict[str, Any] = {
        "transport": {
            "playing": bool(clock.rolling),
            "position_ticks": int(clock.position_ticks),
            "position_seconds": float(clock.position_seconds),
            "bpm": float(project.bpm_at(0)) if project is not None else 0.0,
        },
        "project": {
            "open": project is not None,
            "path": window.path,
            "dirty": bool(project is not None and project.revision() != window.saved_revision),
            "revision": int(project.revision()) if project is not None else 0,
            "undo_depth": int(project.undo_depth()) if project is not None else 0,
            "history": project.history() if project is not None else [],
            "text_sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
            "channels": project.channels() if project is not None else [],
            "patterns": project.patterns() if project is not None else [],
            "notes": sum(int(p["notes"]) for p in project.patterns_info())
            if project is not None
            else 0,
        },
        "status": window.statusBar().currentMessage(),
        "title": window.windowTitle(),
        "panels_open": sorted(p.objectName() for p in window.panels if on_screen(p)),
        "focus": focus.objectName() if focus is not None else "",
        "modal": (
            {
                "class": type(modal).__name__,
                "name": modal.objectName(),
                "title": modal.windowTitle(),
            }
            if modal is not None
            else None
        ),
        "popup": (
            {
                "class": type(popup).__name__,
                "items": [a.text() for a in popup.actions() if a.text()],
            }
            if popup is not None
            else None
        ),
        "plugins": _plugins(window),
        "midi": {
            "inputs": engine.midi_inputs() if not engine.null_backend else [],
            "opened": engine.midi_opened,
        },
        "audio": {
            "null": bool(engine.null_backend),
            "xruns": engine.xruns(),
            "capturing": False,
            "worst_callback_ms": float(engine.worst_callback_ms()),
        },
        "analyzers": sorted(window.analyzer_host.open),
        "control": {"errors": list(getattr(window.control_server, "async_errors", []) or [])},
    }
    return out


def lookup(snapshot: dict[str, Any], path: str) -> Json:
    """``snapshot["a"]["b"]`` for "a.b"; a missing key is None."""
    node: Any = snapshot
    for part in path.split("."):
        if isinstance(node, dict):
            node = node.get(part)
        elif isinstance(node, list) and part.isdigit():
            index = int(part)
            node = node[index] if index < len(node) else None
        else:
            return None
    return node


def holds(value: Json, op: str, want: Json) -> bool:
    if op == "equals":
        return bool(value == want)
    if op == "not_equals":
        return bool(value != want)
    if op == "gte":
        return value is not None and value >= want
    if op == "lte":
        return value is not None and value <= want
    if op == "gt":
        return value is not None and value > want
    if op == "contains":
        return value is not None and want in value
    if op == "truthy":
        return bool(value)
    if op == "falsy":
        return not value
    raise ValueError(f"unknown predicate {op!r}")
