"""scenario_real_plugins[plugin-format] (phase_10.5.md 4.6): P9-2, through the GUI.

For each fetched plugin (tools/test_plugins.toml) and format: scan it from the Plugins
panel, put it on a channel (an instrument, on both of WannaCry's hands) or an insert (an
effect), automate one of its parameters from the generic editor's menu, open its editor,
save; render twice; reopen through adX.cmd and check the state blob and the named
parameters survive and the render matches.

The browser has no drag source: a plugin goes on with its Use button, after the channel
or insert is selected by a click, which is the route Phase 9 built (corrections, §11).
Not covered: typing into a plugin's own editor (a native window synthesized input does
not reach); the editor is checked by its window and a screenshot of it.
"""

from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import time
from typing import Any

import pytest
from wannacry import MATERIAL, VIA, project_for

from adx.engine_bridge import Project
from adx.testing.describe import compare, describe
from adx.testing.driver import Driver
from adx.testing.wav import read_wav

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import fetch_test_plugins  # noqa: E402

pytestmark = [
    pytest.mark.realplugin,
    pytest.mark.skipif(sys.platform != "win32", reason="the Windows launcher and plugin host"),
]

#: Plugins whose output is not the same twice, and why; they are compared by descriptors.
#: Measured 2026-11-03 on two renders of the same saved project.
NOT_DETERMINISTIC: dict[str, str] = {
    # Its oscillators start each voice at a random phase and drift at random: the two
    # renders differ from the first note (1.7 ms) at up to 0.64, with equal descriptors.
    "Surge XT": "Surge XT randomizes oscillator phase and drift per voice",
    # Up to 2.4e-5 (-92 dBFS) in under 0.2 % of the samples, a fixed-point LSB of its
    # DX7 engine, first well into the song; the descriptors are equal.
    "Dexed": "Dexed's output differs by one fixed-point step in a few samples",
}


#: Plugins whose saved state also records their editor, and how; a re-save after a session
#: without the editor open differs there and nowhere else, so their STATE lines are left
#: out of the reopen comparison (the render is still compared).
EDITOR_IN_STATE: dict[str, str] = {
    # Measured 2026-11-03: the only difference in 51,911 bytes of state XML.
    "Surge XT": 'instanceZoomFactor: the editor zoom, "-1" until the editor has opened',
}


#: For NOT_DETERMINISTIC plugins: their peak is where random phases happen to line up.
#: Surge XT on WannaCry, 8 renders: -1.53 to -2.35 dBFS (0.82 dB), loudness within 0.02 LU.
RANDOM_PHASE = {"peak_db": 3.0}


def without_state(text: str) -> str:
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("STATE"))


def bundles() -> list[Any]:
    out = []
    for pin in fetch_test_plugins.pins():
        for bundle in pin["bundles"]:  # type: ignore[attr-defined]
            name, suffix = str(bundle).rsplit(".", 1)
            out.append(pytest.param(str(pin["name"]), name, suffix, id=f"{name}-{suffix}"))
    return out


@pytest.fixture(scope="session")
def plugin_folder() -> pathlib.Path:
    if os.environ.get("ADX_SKIP_PLUGIN_FETCH"):
        pytest.skip("plugin fetch disabled")
    run = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "fetch_test_plugins.py")],
        capture_output=True,
        text=True,
        check=False,
    )
    assert run.returncode == 0, run.stderr
    return pathlib.Path(run.stdout.strip().splitlines()[-1])


def render(song: pathlib.Path, out: pathlib.Path, env: dict[str, str]) -> pathlib.Path:
    run = subprocess.run(
        [
            sys.executable,
            "-m",
            "adx",
            "render",
            str(song),
            "-o",
            str(out),
            "--format",
            "wav32f",
            "--tail",
            "1s",
        ],
        capture_output=True,
        text=True,
        check=False,
        env=env,
        cwd=ROOT,
    )
    assert run.returncode == 0, run.stderr
    return out


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def scan(app: Driver) -> None:
    app.trigger("view.plugins")
    app.click("plugins.scan")
    app.wait_for("plugins.scanning", predicate="falsy", timeout=600)
    app.wait_for("plugins.known", predicate="gte", value=1, timeout=10)


def params_in(app: Driver, root: str) -> list[dict[str, Any]]:
    return [
        w
        for w in app.tree(root=root, interactive=False)
        if w["name"].startswith("param:") and w["visible"]
    ]


@pytest.mark.parametrize("material", MATERIAL)
@pytest.mark.parametrize(("pin", "plugin", "suffix"), bundles())
def test_real_plugins(
    pin: str,
    plugin: str,
    suffix: str,
    material: str,
    plugin_folder: pathlib.Path,
    tmp_path: pathlib.Path,
    evidence: pathlib.Path,
) -> None:
    fmt = suffix.upper()
    song = tmp_path / "song.adx"
    shutil.copy(project_for(material), song)
    model, _ = Project.load(str(song))
    channels = model.channels_info()
    env = dict(
        os.environ,
        ADX_PLUGIN_PATH=str(plugin_folder / pin),
        ADX_PLUGIN_CACHE=str(tmp_path / "plugin-cache.json"),
    )

    with Driver.launch(song, via=VIA, env=env, settings_dir=evidence / "settings") as app:
        app.wait_for("project.open", timeout=30)
        scan(app)
        before = len(app.state()["plugins"]["instances"])
        instrument = None
        if pin in ("Dexed", "Surge XT") and "Effects" not in plugin:
            instrument = True
            # Both hands of WannaCry; the stand-in's first channel.
            targets = channels[:2] if material == "wannacry" else channels[:1]
            for channel in targets:
                app.trigger("view.channel_rack")
                app.click(f"channel_rack.row:{channel['id']}.name")
                app.trigger("view.plugins")
                app.click("plugins.list", item=[plugin, fmt])
                app.click("plugins.use")
            wanted = before + len(targets)
        else:
            instrument = False
            insert = int(channels[0]["output"])
            app.trigger("view.mixer")
            app.click(f"mixer.strip:{insert}.name")
            app.trigger("view.plugins")
            app.click("plugins.list", item=[plugin, fmt])
            app.click("plugins.use")
            wanted = before + 1
        app.wait_for(lambda s: len(s["plugins"]["instances"]) >= wanted, timeout=60)
        assert app.state()["control"]["errors"] == []

        # The editor that holds its parameters, then one of them automated from its menu.
        if instrument:
            app.trigger("view.channel_rack")
            app.click(f"channel_rack.row:{channels[0]['id']}.name")
            app.trigger("view.instrument")
            panel, button = "instrument", "instrument.plugin_window"
        else:
            app.trigger("view.mixer")
            app.click(f"mixer.strip:{insert}/slot:0.button")
            app.trigger("view.effect")
            panel, button = "effect", "effect.plugin_window"
        knobs = params_in(app, panel)
        assert knobs, f"the {panel} panel shows no parameters of {plugin}"
        depth = app.state()["project"]["undo_depth"]
        app.context_menu(knobs[0]["path"])
        app.wait_for("popup", timeout=5)
        app.menu("Create automation")
        app.wait_for("project.undo_depth", predicate="gt", value=depth, timeout=10)
        assert app.state()["project"]["history"][-1].startswith("Automate")

        # Its own editor: embedded and drawn, or the generic editor with every parameter.
        app.click(button)
        windows = app.find(name="plugin-window:", cls="PluginWindow")
        assert len(windows) == 1, windows
        window = windows[0]["name"]
        embedded = app.script(
            "from PySide6.QtWidgets import QApplication\n"
            f"result = [w.embedded for w in QApplication.topLevelWidgets() if w.objectName() == {window!r}]"
        )
        if embedded == [True]:
            app.wait_for(lambda s: True, timeout=1)  # one more frame for the plugin to paint
            shot = app.screenshot(
                evidence / f"{plugin}-{suffix}-editor.png", target=window, screen=True
            )
            assert shot["colours"] > 1, f"{plugin}'s editor drew nothing"
        else:
            assert params_in(app, window), f"{plugin}: no editor and no generic controls"
        app.trigger("file.save")
        app.wait_for("project.dirty", predicate="falsy", timeout=30)
        saved = song.read_text(encoding="utf-8")
    assert "STATE" in saved, "the plugin's state was not saved"

    first = render(song, tmp_path / "first.wav", env)
    second = render(song, tmp_path / "second.wav", env)
    a, rate = read_wav(first)
    b, _ = read_wav(second)
    assert float(abs(a).max()) > 1e-4, "the render is silent"
    if plugin in NOT_DETERMINISTIC:
        assert compare(describe(a, rate), describe(b, rate), RANDOM_PHASE) == [], NOT_DETERMINISTIC[
            plugin
        ]
    else:
        assert a.shape == b.shape and (a == b).all(), f"{plugin} rendered differently twice"

    # Reopened through the launcher: the same text (state blob, named parameters), and
    # saved again by the app, the same render.
    again = tmp_path / "again.adx"
    with Driver.launch(song, via=VIA, env=env, settings_dir=evidence / "settings2") as app:
        app.wait_for("project.open", timeout=30)
        app.wait_for(lambda s: len(s["plugins"]["instances"]) >= 1, timeout=60)
        assert app.state()["project"]["text_sha256"] == _sha(Project.load(str(song))[0].dumps())
        # A freshly loaded plugin reporting its values is not an edit (P10.5-2): once it
        # has settled, the reopened project has nothing to undo.
        time.sleep(2.0)
        opened = app.state()["project"]
        assert opened["undo_depth"] == 0, f"{plugin}: reopening made edits: {opened['history']}"
        app.quit(save=True, path=again)
    resaved, original = Project.load(str(again))[0].dumps(), Project.load(str(song))[0].dumps()
    if plugin in EDITOR_IN_STATE:
        assert without_state(resaved) == without_state(original), EDITOR_IN_STATE[plugin]
    else:
        assert resaved == original
    third, _ = read_wav(render(again, tmp_path / "third.wav", env))
    if plugin in NOT_DETERMINISTIC:
        assert compare(describe(a, rate), describe(third, rate), RANDOM_PHASE) == []
    else:
        assert a.shape == third.shape and (a == third).all(), (
            f"{plugin}: the reopened project renders differently"
        )
