"""launcher_driven (phase_10.5.md 5): adX.cmd with ADX_CONTROL=1, as a double-click starts
it; the driver connects, opens a project, plays, and quits cleanly."""

from __future__ import annotations

import pathlib
import sys

import pytest
from wannacry import MATERIAL, STAND_IN, project_for

from adx.testing.driver import Driver, _alive
from adx.testing.glitch import find_glitches
from adx.testing.wav import read_wav

pytestmark = pytest.mark.skipif(sys.platform != "win32", reason="adX.cmd is the Windows launcher")


@pytest.mark.parametrize("material", MATERIAL)
def test_launcher_driven(material: str, evidence: pathlib.Path) -> None:
    project = project_for(material)
    with Driver.launch(via="adX.cmd", settings_dir=evidence / "settings") as app:
        assert app.state()["project"]["path"] in (None, "")
        opened = app.open(project)
        assert opened["path"] and pathlib.Path(opened["path"]) == project
        before = int(app.state()["audio"]["xruns"])
        app.trigger("transport.play")
        app.wait_for("transport.playing", timeout=10)
        app.wait_for("transport.position_seconds", predicate="gt", value=0.5, timeout=10)
        take = app.capture(3.0, evidence / "take.wav")
        app.trigger("transport.stop")
        app.wait_for("transport.playing", predicate="falsy", timeout=10)
        assert int(app.state()["audio"]["xruns"]) == before
        pid = app.pid
    assert not _alive(pid), "adX did not quit"
    audio, rate = read_wav(take["path"])
    assert len(audio) >= rate * 2.5
    assert float(abs(audio).max()) > 0.01, "the capture is silent"
    assert find_glitches(audio, rate) == []
    assert project == STAND_IN or material == "wannacry"
