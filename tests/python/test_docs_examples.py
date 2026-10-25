"""The examples in docs/mininotation.md and docs/scripting.md, executed (phase_7.md 7:
"their examples are executed by CI").

mininotation.md's tables say what a pattern plays, as ``note@beat``; each row is compiled
by the engine and compared. scripting.md's Python blocks run in order, in one namespace,
on a fresh project.
"""

from __future__ import annotations

import pathlib
import re

import pytest

from adx import engine_bridge
from adx.engine_bridge import PPQ

NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
_ROW = re.compile(
    r"^\|\s*`(?P<pattern>(?:[^`]|\\`)*)`\s*\|\s*(?P<cycle>\d+)\s*\|\s*`(?P<plays>[^`]*)`\s*\|\s*$"
)


def note_name(pitch: int) -> str:
    """MIDI to a note name, C4 = 60, sharps."""
    return f"{NAMES[pitch % 12]}{pitch // 12 - 1}"


def beats(ticks: int) -> str:
    """Ticks as beats, to four decimals, trailing zeros dropped."""
    text = f"{ticks / PPQ:.4f}".rstrip("0").rstrip(".")
    return text or "0"


def plays(pattern: str, cycle: int) -> str:
    """What the engine compiles ``pattern`` to, spelled as the tables spell it."""
    result = engine_bridge.mini_compile(pattern, 0, PPQ * 4, cycle)
    assert result["ok"], result["problems"]
    return " ".join(f"{note_name(n[2])}@{beats(n[0])}" for n in result["notes"])


def mini_rows(repo_root: pathlib.Path) -> list[tuple[str, int, str]]:
    rows = []
    for line in (repo_root / "docs" / "mininotation.md").read_text(encoding="utf-8").splitlines():
        match = _ROW.match(line)
        if match:
            pattern = match.group("pattern").replace("\\|", "|")
            rows.append((pattern, int(match.group("cycle")), match.group("plays")))
    return rows


def test_mininotation_tables_are_true(repo_root: pathlib.Path) -> None:
    rows = mini_rows(repo_root)
    assert len(rows) >= 25, "the tables were not found"
    wrong = [
        f"{pattern!r} cycle {cycle}: documented {expected!r}, plays {plays(pattern, cycle)!r}"
        for pattern, cycle, expected in rows
        if plays(pattern, cycle) != expected
    ]
    assert not wrong, "\n".join(wrong)


def python_blocks(path: pathlib.Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    return re.findall(r"```python\n(.*?)```", text, flags=re.S)


@pytest.mark.usefixtures("qapp")
def test_scripting_doc_blocks_run(repo_root: pathlib.Path) -> None:
    from adx.scripting import api

    blocks = python_blocks(repo_root / "docs" / "scripting.md")
    assert len(blocks) >= 8
    api.new()
    namespace: dict[str, object] = {"adx": api}
    for number, block in enumerate(blocks, start=1):
        try:
            exec(compile(block, f"docs/scripting.md block {number}", "exec"), namespace)
        except Exception as error:
            pytest.fail(f"docs/scripting.md block {number} failed: {error!r}\n{block}")
