"""Patterns by name: the id a command wants, and the length that holds some notes."""

from __future__ import annotations

from adx.engine_bridge import PPQ, NoteArray, Project


def pattern_id(project: Project, pattern: str) -> int:
    for info in project.patterns_info():
        if info["name"] == pattern:
            return int(info["id"])
    raise ValueError(f"no pattern {pattern!r}")


def fitted_length(project: Project, pattern: str, rows: list[NoteArray]) -> int | None:
    """The pattern length, in whole bars, that holds ``rows``; None if it already does."""
    end = max((int((r["start"] + r["length"]).max()) for r in rows if len(r)), default=0)
    if end <= project.pattern_length(pattern):
        return None
    numerator, denominator = project.meter_at(0)
    bar = PPQ * 4 * numerator // denominator
    return -(-end // bar) * bar
