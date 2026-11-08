"""Plugin UI follow-ups (STATE.md P9-7, P9-8): writing automation from the record
toolbar, the plugin window's never-embed choice and health bar, the mixer's late-block
counter, and the crash notice's restart button."""

from __future__ import annotations

import pathlib
from collections.abc import Iterator

import pytest
from PySide6.QtCore import QSettings

from adx import engine_bridge
from adx.engine_bridge import Engine, Project


@pytest.fixture
def settings(tmp_path: pathlib.Path) -> Iterator[QSettings]:
    """Default-constructed QSettings in an ini under tmp_path, never the user's registry."""
    QSettings.setDefaultFormat(QSettings.Format.IniFormat)
    QSettings.setPath(QSettings.Format.IniFormat, QSettings.Scope.UserScope, str(tmp_path))
    store = QSettings()
    store.clear()
    yield store
    QSettings.setDefaultFormat(QSettings.Format.NativeFormat)


def _project() -> Project:
    project, _ = Project.loads(
        "[PROJECT]\nADX_VERSION=2\n\n[CHANNEL Lead]\nINSTRUMENT=testtone\n\n"
        '[MIXER]\nINSERT 1 name="Master"\n'
    )
    return project


def test_never_embed_is_remembered_per_plugin(qapp: object, settings: QSettings) -> None:
    from adx.panels.plugins import window
    from adx.preferences import Preferences

    assert not window.never_embed("uid-a")
    window.set_never_embed("uid-a", True)
    window.set_never_embed("uid-b", True)
    window.set_never_embed("uid-b", False)
    assert window.never_embed("uid-a")
    assert not window.never_embed("uid-b")
    assert not window.never_embed("")
    # The preferences read the same list.
    assert Preferences.load(QSettings()).plugins_never_embed == ["uid-a"]


def test_plugin_window_bar_without_an_instance(qapp: object, settings: QSettings) -> None:
    from adx.panels.plugins.window import PluginWindow, health_text

    win = PluginWindow(0, "Nothing", uid="uid-x")
    try:
        assert not win.embedded
        assert win.health.text() == "not loaded"
        assert not win.restart_button.isEnabled()
        assert win.check_latency() == -1
        win.no_embed.setChecked(True)
        from adx.panels.plugins.window import never_embed

        assert never_embed("uid-x")
    finally:
        win.close()
        win.deleteLater()
    row = {"state": "running", "late": 3, "latency": 64}
    assert health_text(row) == "running · 3 late blocks · 64 samples latency"
    assert health_text({"state": "crashed", "late": 1, "latency": 0}).startswith(
        "crashed · 1 late block "
    )


def test_plugin_window_restart_follows_state(
    qapp: object, settings: QSettings, monkeypatch: pytest.MonkeyPatch
) -> None:
    from adx.panels.plugins import window

    state = {"value": "crashed"}
    calls: list[int] = []
    monkeypatch.setattr(
        engine_bridge.Plugins,
        "instances",
        staticmethod(
            lambda: [{"id": 7, "name": "Fixture", "state": state["value"], "late": 0, "latency": 0}]
        ),
    )
    monkeypatch.setattr(engine_bridge.Plugins, "open_editor", staticmethod(lambda _i: None))
    monkeypatch.setattr(
        engine_bridge.Plugins, "restart", staticmethod(lambda i: calls.append(i) or 1)
    )
    win = window.PluginWindow(7, "Fixture", uid="u")
    try:
        assert win.restart_button.isEnabled()
        state["value"] = "running"
        assert win.restart() == 1
        assert calls == [7]
        assert not win.restart_button.isEnabled()
    finally:
        win.close()
        win.deleteLater()


def test_restart_notice(qapp: object, monkeypatch: pytest.MonkeyPatch) -> None:
    from adx.plugin_runtime import PluginRuntime, RestartButton
    from adx.session import Session

    calls: list[int] = []
    monkeypatch.setattr(
        engine_bridge.Plugins, "instances", staticmethod(lambda: [{"id": 4, "name": "Dexed"}])
    )
    monkeypatch.setattr(
        engine_bridge.Plugins, "restart", staticmethod(lambda i: calls.append(i) or 1)
    )
    engine = Engine(null_backend=True)
    runtime = PluginRuntime(engine, Session(engine), start=False)
    button = RestartButton(runtime)
    assert button.isHidden()
    runtime.crashed.emit(4)
    assert not button.isHidden()
    assert button.text() == "Restart Dexed"
    assert button.restart() == 1
    assert calls == [4]
    assert button.isHidden()
    button.deleteLater()


def test_mixer_health_counter(qapp: object, monkeypatch: pytest.MonkeyPatch) -> None:
    from adx.panels.mixer.strip import SlotRow

    row = SlotRow(0)
    row.show_slot({"id": 3, "type": "vst3", "bypass": False, "mix": 1.0})
    row.show_health("vst3", 12, "running")
    assert row.button.text() == "vst3  ⚠ 12"
    assert row.button.property("plugin_warn")
    assert "12 late block" in row.button.toolTip()
    row.show_health("vst3", 0, "running")
    assert row.button.text() == "vst3"
    assert not row.button.property("plugin_warn")
    row.show_health("vst3", 0, "crashed")
    assert row.button.text() == "vst3  ⚠"
    row.deleteLater()


def test_write_automation_pass_without_changes(qapp: object) -> None:
    engine = Engine(null_backend=True)
    project = _project()
    engine.set_project(project)
    assert engine.write_automation(True) == ""
    assert engine.automation_lanes == 0
    # Nothing moved: nothing is added, and the history is untouched.
    before = project.history()
    assert engine.write_automation(False, project) == ""
    assert project.history() == before
    # Stopping with no project discards the pass.
    engine.write_automation(True)
    assert engine.write_automation(False) == ""


def test_record_toolbar_automation_button(qapp: object) -> None:
    from adx.panels.record.toolbar import RecordToolbar
    from adx.session import Session

    engine = Engine(null_backend=True)
    session = Session(engine)
    session.open(_project())
    messages: list[str] = []
    bar = RecordToolbar(engine, session, lambda: None, lambda: None, lambda: "")
    bar.message.connect(messages.append)
    bar.automation.setChecked(True)
    assert messages[-1].startswith("Writing automation")
    bar.automation.setChecked(False)
    assert messages[-1] == "Write automation: nothing moved"
    # And the MIDI button refuses without a channel to record into.
    bar.midi.setChecked(True)
    assert not bar.midi.isChecked()
    assert "select the channel" in messages[-1]
    bar.deleteLater()


def test_midi_take_through_the_toolbar(qapp: object) -> None:
    """With a channel to play, the MIDI button records; with nothing played it says so."""
    from adx.panels.record.toolbar import RecordToolbar
    from adx.session import Session

    engine = Engine(null_backend=True)
    engine.start()
    try:
        session = Session(engine)
        project = _project()
        session.open(project)
        channel = int(project.channels_info()[0]["id"])
        messages: list[str] = []
        bar = RecordToolbar(
            engine, session, lambda: None, lambda: None, lambda: "", midi_channel=lambda: channel
        )
        bar.message.connect(messages.append)
        bar.midi.setChecked(True)
        assert bar.midi_origin is not None
        assert messages[-1] == "Recording MIDI…"
        bar.midi.setChecked(False)
        assert messages[-1] == "Record MIDI: nothing was played"
        bar.deleteLater()
    finally:
        engine.stop()
