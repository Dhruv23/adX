"""Check that adX.cmd, the double-click launcher in the repository root, opens the app.

Run at the end of every phase, after the phase's last ``pip install -e .[dev]``
(plans/STATE.md, "When you finish a phase"), so the launcher the user opens always runs
the latest build:

    py -3.12 tools/check_launcher.py

It starts adX.cmd exactly as a double-click does, with ADX_SMOKE set: the application
opens its window, writes a short report once it has run for a moment, and quits
(adx/application.py). Exit status 0 means the whole application came up - every panel,
the engine, the Qt Quick plugin - from the launcher.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "adX.cmd"
TIMEOUT_S = 90.0


def main() -> int:
    if not LAUNCHER.exists():
        print(f"check_launcher: {LAUNCHER} is missing", file=sys.stderr)
        return 1
    report = Path(tempfile.mkdtemp()) / "adx_smoke.json"
    env = dict(os.environ, ADX_SMOKE=str(report))
    started = subprocess.run(
        ["cmd", "/c", str(LAUNCHER)], env=env, cwd=ROOT, capture_output=True, text=True
    )
    if started.returncode != 0:
        print(f"check_launcher: adX.cmd failed:\n{started.stdout}{started.stderr}", file=sys.stderr)
        return 1
    deadline = time.monotonic() + TIMEOUT_S
    while not report.exists() and time.monotonic() < deadline:
        time.sleep(0.5)
    if not report.exists():
        print(f"check_launcher: no report within {TIMEOUT_S:.0f} s", file=sys.stderr)
        return 1
    time.sleep(0.2)  # the report is written in one call; let it close
    facts = json.loads(report.read_text(encoding="utf-8"))
    problems = []
    if not facts.get("visible"):
        problems.append("the window was not visible")
    if len(facts.get("panels", [])) < 11:
        problems.append(f"only {len(facts.get('panels', []))} panels")
    if problems:
        print("check_launcher: " + "; ".join(problems), file=sys.stderr)
        return 1
    print(
        f"check_launcher: adX.cmd opened adX {facts['version']} - "
        f"{len(facts['panels'])} panels, {facts['status']}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
