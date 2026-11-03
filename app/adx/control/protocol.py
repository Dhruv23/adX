"""The control channel's wire format (phase_10.5.md 4.2): one JSON object per line.

    client -> {"hello": TOKEN, "protocol": 1}
    server <- {"ok": true, "protocol": 1, "version": "...", "pid": N}
    client -> {"id": 7, "op": "click", "target": "mixer/insert:1/mute"}
    server <- {"id": 7, "ok": true, "result": ...}
           or {"id": 7, "ok": false, "error": "...", "code": "no-target"}

A response carries the request's id; requests on one connection are answered in order,
except ``wait_for``, which answers when its predicate holds or its timeout passes.
"""

from __future__ import annotations

import json
from typing import Any

PROTOCOL_VERSION = 1

#: A decoded JSON value: a request field, a probe reading, a script's result.
type Json = Any

#: Error codes a response may carry.
BAD_REQUEST = "bad-request"
BAD_TOKEN = "bad-token"
UNKNOWN_OP = "unknown-op"
NO_TARGET = "no-target"
AMBIGUOUS = "ambiguous"
TIMEOUT = "timeout"
FAILED = "failed"

#: Every request the server understands, for documentation and ``ops``.
OPS: tuple[str, ...] = (
    "ops",
    "tree",
    "find",
    "click",
    "double_click",
    "drag",
    "drop",
    "type",
    "key",
    "scroll",
    "choose",
    "trigger",
    "actions",
    "state",
    "wait_for",
    "screenshot",
    "script",
    "open",
    "capture_start",
    "capture_stop",
    "answer",
    "menu",
    "context_menu",
    "quit",
)


def encode(message: dict[str, Any]) -> bytes:
    return (json.dumps(message, separators=(",", ":"), default=str) + "\n").encode("utf-8")


def decode(line: bytes | str) -> dict[str, Any]:
    text = line.decode("utf-8") if isinstance(line, bytes) else line
    value = json.loads(text)
    if not isinstance(value, dict):
        raise ValueError("a message is a JSON object")
    return value


class ControlError(Exception):
    """A request the server refused, with its code."""

    def __init__(self, message: str, code: str = FAILED) -> None:
        super().__init__(message)
        self.code = code
