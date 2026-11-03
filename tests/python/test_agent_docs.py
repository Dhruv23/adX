"""docs/agent-testing.md's examples run (phase_10.5.md 6): each Python block is executed
from the repository root, and every ``adx`` command in a console block is one the CLI
has."""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import sys

import pytest

from adx.cli import build_parser

ROOT = pathlib.Path(__file__).resolve().parents[2]
DOC = ROOT / "docs" / "agent-testing.md"


def blocks(language: str) -> list[str]:
    text = DOC.read_text(encoding="utf-8")
    return re.findall(rf"```{language}\n(.*?)```", text, flags=re.DOTALL)


@pytest.mark.parametrize("number", range(len(blocks("python"))))
def test_python_examples_run(number: int, tmp_path: pathlib.Path) -> None:
    # The page reads top to bottom: later examples use the first one's import.
    code = "from adx.testing.driver import Driver\n" + blocks("python")[number]
    (ROOT / "build").mkdir(exist_ok=True)
    environment = {k: v for k, v in os.environ.items() if k != "QT_QPA_PLATFORM"} | {
        "QT_QPA_PLATFORM": "offscreen"
    }
    run = subprocess.run(
        [sys.executable, "-c", code],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=240,
        check=False,
        env=environment,
    )
    assert run.returncode == 0, run.stderr[-3000:]


def test_console_commands_exist() -> None:
    sub = next(a for a in build_parser()._actions if a.dest == "command")
    known = set(sub.choices)  # type: ignore[arg-type]
    used = []
    for block in blocks("console"):
        for line in block.splitlines():
            match = re.match(r"\$ adx (\S+)", line)
            if match:
                used.append(match.group(1))
    assert used
    assert [u for u in used if u not in known] == []
