"""scenario_performance_walkthrough (phase_11.md 5, 6): docs/performance.md followed step
by step through the driver, against the emulated controller, with the state each step
says the reader should see asserted. Learn mode included: the virtual pad presses each
pad the dialog asks for.

The hardware half (a physical controller) is in the final review.
"""

from __future__ import annotations

import os
import pathlib
import sys
import time

import pytest
from wannacry import ROOT, STAND_IN, VIA

from adx.testing.driver import Driver

pytestmark = [
    pytest.mark.skipif(sys.platform != "win32", reason="loopMIDI and adX.cmd are Windows"),
    pytest.mark.skipif(bool(os.environ.get("CI")), reason="needs a loopMIDI port (not on CI)"),
]

CELL_W, CELL_H = 96, 28
PROFILE = ROOT / "app" / "adx" / "pads" / "profiles" / "adx_virtual_pad.adxpad"

SESSION = """
info = window.session.project.session_info()
e = window.engine
result = {"tracks": [t["channel"] for t in info["tracks"]],
          "scenes": [s["name"] for s in info["scenes"]],
          "cells": [[(s or {}).get("pattern") for s in t["slots"]] for t in info["tracks"]],
          "states": [[e.clip_state(t, r) for r in range(len(info["scenes"]))]
                     for t in range(len(info["tracks"]))],
          "rate": e.session_rate,
          "pads": sorted(window.pads.attached)}
"""


def session(app: Driver) -> dict[str, object]:
    return dict(app.script(SESSION))


def wait(app: Driver, check: object, timeout: float = 10.0) -> dict[str, object]:
    deadline = time.monotonic() + timeout
    state = session(app)
    while time.monotonic() < deadline:
        state = session(app)
        if check(state):  # type: ignore[operator]
            return state
        time.sleep(0.1)
    raise AssertionError(f"not reached: {state}")


def test_performance_walkthrough(evidence: pathlib.Path) -> None:
    from adx.testing.midi import ports

    if not any("loopmidi" in n.lower() for n in ports()["inputs"]):
        pytest.skip("needs loopMIDI running with a port")
    sys.path.insert(0, str(ROOT / "tools"))
    from virtual_pad import VirtualPad

    pad = VirtualPad(PROFILE.read_text(encoding="utf-8"), "loopMIDI")
    try:
        with Driver.launch(via=VIA, settings_dir=evidence / "settings", timeout=180) as app:
            app.open(STAND_IN)
            patterns = app.state()["project"]["patterns"]
            channels = app.state()["project"]["channels"]

            # 1. Open the session; add a track, two scenes, and a pattern in a cell.
            app.trigger("view.session")
            app.wait_for("panels_open", predicate="contains", value="session", timeout=10)
            app.click("session-add-track", wait=False)
            app.wait_for("modal", timeout=10)
            app.script(
                "from PySide6.QtWidgets import QApplication\n"
                "d = QApplication.activeModalWidget()\n"
                f"d.setTextValue({channels[0]!r})\nd.accept()\nresult = True"
            )
            wait(app, lambda s: s["tracks"] == [channels[0]])
            app.click("session-add-scene")
            app.click("session-add-scene")
            wait(app, lambda s: s["scenes"] == ["Scene 1", "Scene 2"])
            app.context_menu("session-grid", x=CELL_W / 2, y=CELL_H / 2)
            app.menu("Put pattern")
            app.menu(patterns[0])
            wait(app, lambda s: s["cells"] == [[patterns[0], None]])

            # 2. Launch from the screen: click the cell; it plays (nothing else rolls, so
            # the session's clock starts with it). Click the empty cell: the track stops.
            app.click("session-grid", x=CELL_W / 2, y=CELL_H / 2)
            wait(app, lambda s: s["states"][0][0] == "playing")
            app.click("session-grid", x=CELL_W / 2, y=CELL_H * 1.5)
            wait(app, lambda s: s["states"][0][0] == "stopped", timeout=15)

            # 3. Connect the controller: Pads..., the input, Use as pads.
            app.script(
                "names = [n for n in window.engine.midi_inputs() if 'loopMIDI' in n]\n"
                "window.engine.open_midi(names[0])\nresult = names[0]"
            )
            app.click("session-pads", wait=False)
            app.wait_for("modal.name", predicate="equals", value="pad-setup", timeout=10)
            app.choose("pad-profile", "adX Virtual Pad")
            app.click("pad-attach")
            status = app.script(
                "from PySide6.QtWidgets import QApplication\n"
                "result = QApplication.activeModalWidget().status.text()"
            )
            assert "pads (adX Virtual Pad)" in str(status)
            # A pad launches the cell, and its light follows.
            pad.launch(0, 0, 100)
            wait(app, lambda s: s["states"][0][0] == "playing")
            deadline = time.monotonic() + 3
            while pad.light(0, 0) != "playing" and time.monotonic() < deadline:
                time.sleep(0.05)
            assert pad.light(0, 0) == "playing"
            assert pad.light(0, 1) == "empty"
            pad.stop(0)
            wait(app, lambda s: s["states"][0][0] == "stopped", timeout=15)

            # Learn mode: a 2 x 2 grid of the virtual pad's notes, scenes skipped, then
            # stop all and tap.
            app.script(
                "from PySide6.QtWidgets import QApplication\n"
                "d = QApplication.activeModalWidget()\n"
                "d.columns.setValue(2)\nd.rows.setValue(2)\nresult = True"
            )
            app.click("pad-learn")
            prompt = (
                "from PySide6.QtWidgets import QApplication\n"
                "result = QApplication.activeModalWidget().prompt.text()"
            )
            for note in (0, 1, 8, 9):
                assert "Press pad" in str(app.script(prompt))
                pad._press(bytes([0x90, note, 100]))
                time.sleep(0.25)
            assert "scene 1" in str(app.script(prompt))
            app.click("pad-learn-skip")
            pad._press(bytes([0xB0, 100, 127]))
            time.sleep(0.25)
            pad._press(bytes([0xB0, 101, 127]))
            time.sleep(0.25)
            assert "Learned: saved as" in str(app.script(prompt))
            learned = list((evidence / "settings" / "pads").glob("*.adxpad"))
            assert learned, "no profile saved"
            text = learned[0].read_text(encoding="utf-8")
            assert "GRID=2x2" in text
            assert "NOTE_BASE=0" in text
            assert "ROW_STRIDE=8" in text
            app.key("Escape")
            app.wait_for("modal", predicate="falsy", timeout=10)

            # 4. Tempo: four taps half a second apart; a held nudge returns exactly.
            base = float(session(app)["rate"])
            for _ in range(4):
                app.click("transport-tap")
                time.sleep(0.5)
            tapped = wait(app, lambda s: abs(float(s["rate"]) - base) > 1e-6)
            app.script("window.nudge(1)\nresult = True")
            nudged = wait(app, lambda s: abs(float(s["rate"]) - float(tapped["rate"])) > 1e-6)
            assert abs(float(nudged["rate"]) / float(tapped["rate"]) - 1.04) < 1e-6
            app.script("window.nudge(0)\nresult = True")
            wait(app, lambda s: s["rate"] == tapped["rate"])

            # 5. Record: Perform on, a clip, Perform off; the status says what was written.
            app.click("transport-perform")
            app.click("session-grid", x=CELL_W / 2, y=CELL_H / 2)
            wait(app, lambda s: s["states"][0][0] == "playing")
            time.sleep(3.0)
            app.click("transport-perform")
            app.wait_for("status", predicate="contains", value="Performance recorded", timeout=10)
            assert app.state()["project"]["undo_depth"] > 0
    finally:
        pad.close()
