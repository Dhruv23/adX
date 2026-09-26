"""The `.adx` format, seen from Python.

A thin, typed layer over ``adx_engine``: the engine parses, formats, validates and
diffs; this module turns what crosses the bridge into dataclasses so nothing
downstream handles an untyped dict (phase_2.md 4.14).
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass

import adx_engine

__all__ = [
    "Change",
    "Diagnostic",
    "diagnostic_codes",
    "diff_text",
    "format_text",
    "validate_text",
]


@dataclass(frozen=True, slots=True)
class Diagnostic:
    """One problem, addressable by an editor: line, column and length."""

    severity: str
    code: str
    line: int
    column: int
    length: int
    message: str
    hint: str

    @property
    def is_error(self) -> bool:
        """True for an Error; warnings and migration notes never fail a command."""
        return self.severity == "error"

    def render(self, path: str) -> str:
        """``path:line:col: severity ADXnnnn: message``, the shape editors parse.

        A diagnostic from the invariant checker has no position (line 0), because a
        project need not have come from a file; it is printed without one.
        """
        where = f"{path}:{self.line}:{self.column}: " if self.line > 0 else f"{path}: "
        text = f"{where}{self.severity} {self.code}: {self.message}"
        if self.hint:
            text += f" (hint: {self.hint})"
        return text


@dataclass(frozen=True, slots=True)
class Change:
    """One semantic difference between two projects."""

    kind: str
    subject: str
    detail: str

    def render(self) -> str:
        """``+ channel Lead`` / ``~ pattern Verse: Lead: +3 notes``."""
        return f"{self.kind} {self.subject}" + (f": {self.detail}" if self.detail else "")


def _diagnostics(raw: Iterable[Mapping[str, str | int]]) -> list[Diagnostic]:
    return [
        Diagnostic(
            severity=str(item["severity"]),
            code=str(item["code"]),
            line=int(item["line"]),
            column=int(item["column"]),
            length=int(item["length"]),
            message=str(item["message"]),
            hint=str(item["hint"]),
        )
        for item in raw
    ]


def format_text(text: str) -> tuple[str, list[Diagnostic], int]:
    """Return ``(canonical text, diagnostics, source version)``."""
    formatted, raw, version = adx_engine.format_text(text)
    return str(formatted), _diagnostics(raw), int(version)


def validate_text(
    text: str, base_dir: str = "", check_sample_files: bool = False
) -> list[Diagnostic]:
    """Parse and validate, returning every diagnostic."""
    return _diagnostics(adx_engine.validate_text(text, base_dir, check_sample_files))


def diff_text(before: str, after: str) -> list[Change]:
    """Semantic differences between two files' contents."""
    return [
        Change(kind=str(k), subject=str(s), detail=str(d))
        for k, s, d in adx_engine.diff_text(before, after)
    ]


def diagnostic_codes() -> list[tuple[str, str, str]]:
    """Every code this build can emit, as ``(code, severity, summary)``."""
    return [(str(c), str(s), str(m)) for c, s, m in adx_engine.diagnostic_codes()]
