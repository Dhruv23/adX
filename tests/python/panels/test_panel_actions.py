"""The action registry (phase_6.md 4.10, 5: ``actions_registry_complete``)."""

from __future__ import annotations

from adx.actions import menu_action_ids
from adx.mainwindow import MainWindow


def test_actions_registry_complete(studio: MainWindow) -> None:
    registry = studio.registry
    specs = list(registry.specs())
    ids = [spec.id for spec in specs]
    # Unique ids; every one has an enablement predicate.
    assert len(ids) == len(set(ids))
    assert all(callable(spec.enabled) for spec in specs)
    # Every menu item resolves to a registered action, and every action with a menu is in it.
    in_menus = menu_action_ids(studio.menuBar())
    assert in_menus
    assert set(in_menus) <= set(ids)
    assert {spec.id for spec in specs if spec.menu} == set(in_menus)
    # No two actions in one scope share a shortcut (a panel's tool keys may reuse a key
    # another panel uses, never one a global action uses).
    seen: dict[tuple[str, str], str] = {}
    globals_: set[str] = set()
    for spec in specs:
        key = studio.keymap.shortcut(spec.id)
        if not key:
            continue
        assert (spec.scope, key) not in seen, f"{spec.id} and {seen[(spec.scope, key)]}: {key}"
        seen[(spec.scope, key)] = spec.id
        if not spec.scope:
            globals_.add(key)
    for (scope, key), action_id in seen.items():
        if scope:
            assert key not in globals_, f"{action_id} shadows a global shortcut {key}"


def test_actions_follow_their_predicates(studio: MainWindow) -> None:
    registry = studio.registry
    registry.update_enabled()
    # Nothing selected and nothing done: delete and undo are disabled, and a disabled
    # action triggered by key does nothing.
    assert not registry.action("edit.delete").isEnabled()
    assert not registry.action("edit.undo").isEnabled()
    assert not registry.trigger("edit.undo")
    studio.channel_rack.add_channel()
    registry.update_enabled()
    assert registry.action("edit.undo").isEnabled()
    assert registry.trigger("edit.undo")
    assert len(studio.project.channels_info()) == 1


def test_shortcut_remap_persists(studio: MainWindow) -> None:
    studio.registry.remap("edit.duplicate", "Ctrl+Shift+D")
    assert studio.registry.action("edit.duplicate").shortcut().toString() == "Ctrl+Shift+D"
    assert studio.keymap.shortcut("edit.duplicate") == "Ctrl+Shift+D"
