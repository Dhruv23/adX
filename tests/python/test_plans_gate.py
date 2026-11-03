"""plans_gate_fires (phase_10.5.md 4.7): the gate fails on a planted untagged box and
passes on the tree."""

from __future__ import annotations

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
LINT = ROOT / "tools" / "lint.py"

PLANTED = """# Phase 99

## 6. Definition of done

- [x] The user checked this long ago (history, left alone).
- [ ] Every test passes.
- [ ] The user confirms the mixer works
      on the user's machine.
- [ ] A human listens to the set. (judgement)
- [ ] The pads light on a real controller, which needs the user. (hardware half)

## 7. Out of scope

- [ ] the user does this elsewhere (not a done box)
"""


def lint_plans(root: pathlib.Path | None = None) -> subprocess.CompletedProcess[str]:
    extra = ["--root", str(root)] if root else []
    return subprocess.run(
        [sys.executable, str(LINT), "plans", *extra],
        capture_output=True,
        text=True,
        check=False,
        cwd=ROOT,
    )


def test_plans_gate_fires(tmp_path: pathlib.Path) -> None:
    (tmp_path / "phase_99.md").write_text(PLANTED, encoding="utf-8")
    run = lint_plans(tmp_path)
    assert run.returncode == 1
    flagged = [line for line in run.stderr.splitlines() if line.startswith("phase_99.md:")]
    assert flagged == ["phase_99.md:7: asks 'the user' with no (hardware half) or (judgement) tag"]


def test_plans_gate_clean_on_the_tree() -> None:
    run = lint_plans()
    assert run.returncode == 0, run.stderr
