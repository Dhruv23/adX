"""Check that adX.cmd, the double-click launcher in the repository root, opens the app.

Run at the end of every phase, after the phase's last ``pip install -e .[dev]``
(plans/STATE.md, "When you finish a phase"), so the launcher the user opens always runs
the latest build:

    py -3.12 tools/check_launcher.py

It starts adX.cmd exactly as a double-click does, with ADX_SMOKE set: the application
opens its window, writes a short report once it has run for a moment, and quits
(adx/application.py). Exit status 0 means the whole application came up - every panel,
the engine, the Qt Quick plugin - from the launcher.

Then it launches adX.cmd again with the control channel (ADX_CONTROL=1) and drives it as
an agent would (phase_10.5.md 4.3): open a project, play, capture a second of the master,
and quit through the channel, waiting for the process to end.
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
    env.setdefault("ADX_SCREEN", "secondary")
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
    return drive()


def drive() -> int:
    """The launcher, driven: open, play, capture, quit."""
    sys.path.insert(0, str(ROOT / "app"))
    from adx.testing.driver import Driver, DriverError, _alive
    from adx.testing.wav import read_wav

    project = ROOT / "tests" / "golden" / "single_tone.adx"
    capture = Path(tempfile.mkdtemp()) / "take.wav"
    try:
        app = Driver.launch(project, via="adX.cmd", null_audio=True, timeout=TIMEOUT_S)
        with app:
            app.wait_for("project.open", timeout=30)
            app.trigger("transport.play")
            app.wait_for("transport.playing", timeout=10)
            app.capture(1.0, capture)
            xruns = int(app.state()["audio"]["xruns"])
        pid = app.pid
    except DriverError as error:
        print(f"check_launcher: driving adX.cmd failed: {error}", file=sys.stderr)
        return 1
    if _alive(pid):
        print("check_launcher: the driven app did not quit", file=sys.stderr)
        return 1
    audio, _ = read_wav(capture)
    peak = float(abs(audio).max()) if len(audio) else 0.0
    if peak <= 0.0:
        print("check_launcher: the driven capture was silent", file=sys.stderr)
        return 1
    print(
        f"check_launcher: drove adX.cmd - played {project.name}, captured {len(audio)} frames "
        f"(peak {peak:.3f}), {xruns} xrun(s), quit cleanly"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
