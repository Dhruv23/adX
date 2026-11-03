"""Hosted plugins from Python (phase_9.md 5): scan, load, render, sync state, survive a
crash, and render headless from the CLI.

adx_test_plugin and the sandbox child are build products of the C++ tree (the wheel
build does not make the fixture). These tests find them under build/ and skip when no
build has made them.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import time

import pytest

from adx import engine_bridge
from adx.engine_bridge import Engine, Plugins, Project

ROOT = pathlib.Path(__file__).resolve().parents[2]
PRESETS = ("windows-x64-release", "windows-x64-relwithdebinfo", "windows-x64-debug")


def _built(pattern: str) -> pathlib.Path | None:
    for preset in PRESETS:
        found = sorted((ROOT / "build" / preset).glob(pattern))
        if found:
            return found[0]
    return None


def _fixture(suffix: str) -> pathlib.Path | None:
    return _built(f"fixtures/plugins/adx_test_plugin*.{suffix}")


@pytest.fixture(scope="module")
def plugin_host(tmp_path_factory: pytest.TempPathFactory) -> pathlib.Path:
    """The child and a private cache; skips when neither the wheel nor a build has them."""
    if not Plugins.available():
        host = _built("engine/plugin/adx_plugin_host.exe")
        if host is None:
            pytest.skip("no adx_plugin_host.exe built")
        Plugins.set_host(str(host))
    cache = tmp_path_factory.mktemp("plugin-cache") / "cache.json"
    ok, error = Plugins.load_cache(str(cache))
    assert ok, error
    return pathlib.Path(Plugins.host_path())


@pytest.fixture(scope="module")
def clap_fixture(plugin_host: pathlib.Path) -> pathlib.Path:
    found = _fixture("clap")
    if found is None:
        pytest.skip("adx_test_plugin.clap not built")
    return found


def hosted_text(plugin: pathlib.Path, slot_extras: str = "", midi: str = "") -> str:
    path = str(plugin).replace("\\", "/")
    return (
        "[PROJECT]\nADX_VERSION=2\n"
        "\n[CHANNEL Tone]\nINSTRUMENT=testtone\nOUTPUT=insert.2\n"
        "\n[PATTERN P]\nLENGTH=4:0:0\nNOTES Tone\n  A4 0:0:0 4:0:0 100\n"
        '\n[PLAYLIST]\nTRACK 1 name="L"\n  PATTERN P 0:0:0\n'
        '\n[MIXER]\nINSERT 1 name="Master"\nINSERT 2 name="Hosted"\n'
        f'  SLOT 1 clap plugin="com.adx.test.effect" name="adX Test Effect" path="{path}"'
        f"{slot_extras}\nROUTE insert.2 -> insert.1\n" + (f"\n[MIDI]\n{midi}" if midi else "")
    )


def test_scan_lists_the_fixture(clap_fixture: pathlib.Path) -> None:
    Plugins.scan([str(clap_fixture.parent)], force=True)
    names = {(p["format"], p["name"]) for p in Plugins.listing()}
    assert ("clap", "adX Test Effect") in names
    assert ("clap", "adX Test Synth") in names
    described = Plugins.describe("clap", "com.adx.test.effect", str(clap_fixture))
    assert described is not None
    assert [p["key"] for p in described["params"]] == ["gain", "mode", "latency", "seed"]


def test_plugin_project_renders_and_saves_state(
    clap_fixture: pathlib.Path, tmp_path: pathlib.Path
) -> None:
    project, diagnostics = Project.loads(hosted_text(clap_fixture, " gain=0.5"))
    assert not [d for d in diagnostics if d["severity"] == "error"]
    slot = project.mixer_info()["inserts"][1]["slots"][0]
    assert slot["plugin"]["uid"] == "com.adx.test.effect"
    out = tmp_path / "hosted.wav"
    stats = engine_bridge.render_to_file(project, str(out), tail_seconds=0.0)
    assert out.exists()
    assert float(stats["peak"]) > 0.01

    # A live engine: the plugin's state reaches the project on request.
    engine = Engine(loopback=0)
    engine.set_project(project)
    engine.start()
    engine.play()
    engine.render_offline(4800)
    instance = engine.plugin_instance_of_slot(1)
    assert instance > 0
    assert engine.sync_plugin_states(project) == 1
    saved = tmp_path / "hosted.adx"
    project.save(str(saved))
    assert 'STATE "base64:' in saved.read_text(encoding="utf-8")
    assert engine.sync_plugin_states(project) == 0  # nothing changed since
    engine.stop()


def test_crash_is_a_notice_and_restart_brings_it_back(clap_fixture: pathlib.Path) -> None:
    project, _ = Project.loads(hosted_text(clap_fixture, " gain=1 mode=0"))
    engine = Engine(loopback=0)
    engine.set_project(project)
    engine.start()
    engine.play()
    engine.render_offline(2048)
    Plugins.notices()
    engine.set_param(project, "insert.2.slot.1.mode", 1.0)  # crash on process
    engine.render_offline(4096)
    deadline = time.monotonic() + 5.0
    notices: list[dict[str, object]] = []
    while time.monotonic() < deadline and not any(n["kind"] == "crashed" for n in notices):
        notices += Plugins.notices()
        time.sleep(0.02)
    assert any(n["kind"] == "crashed" for n in notices)
    instance = engine.plugin_instance_of_slot(1)
    state = next(i for i in Plugins.instances() if i["id"] == instance)["state"]
    assert state == "crashed"
    # Playback went on: the engine still renders, the insert passes its input.
    engine.render_offline(2048)
    # Put it back to normal, then restart: it reloads with its last state.
    engine.set_param(project, "insert.2.slot.1.mode", 0.0)
    assert Plugins.restart(instance) >= 1
    assert next(i for i in Plugins.instances() if i["id"] == instance)["state"] == "running"
    engine.stop()


def test_generic_descriptors_come_from_the_plugin(clap_fixture: pathlib.Path) -> None:
    from adx.panels.effect.generic_editor import plugin_descriptors

    descriptors = plugin_descriptors(
        {"uid": "com.adx.test.effect", "path": str(clap_fixture)}, "clap"
    )
    by_name = {d["name"]: d for d in descriptors}
    assert set(by_name) == {"gain", "mode", "latency", "seed"}
    assert by_name["gain"]["max"] == 2.0
    assert by_name["mode"]["scale"] == "stepped"


def test_headless_render_with_plugins(clap_fixture: pathlib.Path, tmp_path: pathlib.Path) -> None:
    source = tmp_path / "hosted.adx"
    source.write_text(hosted_text(clap_fixture, " gain=0.5"), encoding="utf-8")
    out = tmp_path / "hosted.wav"
    env = dict(os.environ)
    env["ADX_PLUGIN_HOST"] = Plugins.host_path()
    env["ADX_PLUGIN_CACHE"] = str(tmp_path / "cache.json")
    # No display at all: the CLI must not need Qt to host a plugin.
    env.pop("QT_QPA_PLATFORM", None)
    result = subprocess.run(
        [sys.executable, "-m", "adx", "render", str(source), "-o", str(out), "--format", "wav32f"],
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    assert out.exists()
    assert out.stat().st_size > 48000


def test_plugin_window_button_is_usable(
    clap_fixture: pathlib.Path, tmp_path: pathlib.Path, studio: object
) -> None:
    """The effect panel's Plugin window button is shown and enabled for a hosted slot (it
    was hidden before its toolbar took it, which leaves a QToolBar widget disabled)."""
    from adx.mainwindow import MainWindow

    assert isinstance(studio, MainWindow)
    song = tmp_path / "hosted.adx"
    song.write_text(hosted_text(clap_fixture), encoding="utf-8")
    assert studio.open_project(str(song))
    studio.show()
    slot = int(studio.project.mixer_info()["inserts"][1]["slots"][0]["id"])
    studio.effect.show_slot(2, slot)
    assert studio.effect.plugin_action.isVisible()
    assert studio.effect.plugin_button.isEnabled()
