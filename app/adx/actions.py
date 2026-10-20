"""Every user-triggerable operation, registered once (phase_6.md 4.10).

An action has an id, a label, a default shortcut, an enablement predicate and a place
in the menus. Menus, toolbars, context menus and the keymap editor are all built from
this registry, so a menu item and its toolbar button cannot disagree about whether they
are enabled - the class of bug the registry exists to prevent.

``actions_registry_complete`` checks it: every menu item resolves to a registered action,
ids are unique, every action has an enablement predicate, and no two actions share a
shortcut.
"""

from __future__ import annotations

from collections.abc import Callable, Iterator
from dataclasses import dataclass

from PySide6.QtCore import QObject, Qt
from PySide6.QtGui import QAction, QKeySequence
from PySide6.QtWidgets import QMenu, QMenuBar, QWidget

from adx.shortcuts import Keymap


def always() -> bool:
    """The enablement predicate of an action that is always available."""
    return True


@dataclass(frozen=True, slots=True)
class ActionSpec:
    """One operation, declared once."""

    id: str
    label: str
    handler: Callable[[], object]
    #: The menu it lives in ("&File", "&Edit"...), or "" for one reached only by key or
    #: context menu.
    menu: str
    shortcut: str = ""
    enabled: Callable[[], bool] = always
    checkable: bool = False
    #: A separator goes before it in its menu.
    separator: bool = False
    tooltip: str = ""
    #: "" for a global action; a panel id for one whose shortcut works only while that
    #: panel has focus (a tool key), so two panels may use the same key.
    scope: str = ""


class ActionRegistry(QObject):
    """The registry, and the QActions built from it."""

    def __init__(self, keymap: Keymap, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self.keymap = keymap
        self._specs: dict[str, ActionSpec] = {}
        self._actions: dict[str, QAction] = {}
        self._order: list[str] = []

    def register(self, spec: ActionSpec) -> QAction:
        """Declare an action. Ids are unique: registering one twice is an error."""
        if spec.id in self._specs:
            raise ValueError(f"action {spec.id!r} is registered twice")
        self._specs[spec.id] = spec
        self._order.append(spec.id)
        action = QAction(spec.label, self)
        action.setObjectName(spec.id)
        action.setCheckable(spec.checkable)
        action.setToolTip(spec.tooltip or spec.label)
        action.setShortcutContext(
            Qt.ShortcutContext.WidgetWithChildrenShortcut
            if spec.scope
            else Qt.ShortcutContext.ApplicationShortcut
        )
        action.triggered.connect(lambda _checked=False, s=spec: self._run(s))
        self._actions[spec.id] = action
        self.keymap.declare(spec.id, spec.shortcut, spec.scope)
        self._apply_shortcut(spec.id)
        return action

    def _run(self, spec: ActionSpec) -> None:
        if spec.enabled():
            spec.handler()

    def _apply_shortcut(self, action_id: str) -> None:
        text = self.keymap.shortcut(action_id)
        self._actions[action_id].setShortcut(QKeySequence(text) if text else QKeySequence())

    def remap(self, action_id: str, shortcut: str) -> None:
        """Give an action a new shortcut, persisted by the keymap."""
        self.keymap.set(action_id, shortcut)
        self._apply_shortcut(action_id)

    def action(self, action_id: str) -> QAction:
        """The QAction for an id."""
        return self._actions[action_id]

    def spec(self, action_id: str) -> ActionSpec:
        """The declaration for an id."""
        return self._specs[action_id]

    def specs(self) -> Iterator[ActionSpec]:
        """Every declaration, in registration order."""
        return (self._specs[i] for i in self._order)

    def scoped(self, scope: str) -> list[QAction]:
        """The actions of one panel, for it to add to itself (so their keys work there)."""
        return [self._actions[i] for i in self._order if self._specs[i].scope == scope]

    def trigger(self, action_id: str) -> bool:
        """Run an action if it is enabled; returns whether it ran."""
        spec = self._specs[action_id]
        if not spec.enabled():
            return False
        spec.handler()
        return True

    def update_enabled(self) -> None:
        """Re-evaluate every predicate. Called when the selection or project changes."""
        for action_id, spec in self._specs.items():
            self._actions[action_id].setEnabled(bool(spec.enabled()))

    def build_menus(self, bar: QMenuBar) -> dict[str, QMenu]:
        """The menu bar, from the registry: one menu per distinct ``menu``, in order."""
        menus: dict[str, QMenu] = {}
        for spec in self.specs():
            if not spec.menu:
                continue
            menu = menus.get(spec.menu)
            if menu is None:
                menu = bar.addMenu(spec.menu)
                menus[spec.menu] = menu
            if spec.separator:
                menu.addSeparator()
            menu.addAction(self._actions[spec.id])
        for menu in menus.values():
            menu.aboutToShow.connect(self.update_enabled)
        return menus

    def context_menu(self, ids: list[str], parent: QWidget | None = None) -> QMenu:
        """A context menu of registered actions ("" is a separator)."""
        menu = QMenu(parent)
        for action_id in ids:
            if not action_id:
                menu.addSeparator()
                continue
            action = self._actions[action_id]
            action.setEnabled(bool(self._specs[action_id].enabled()))
            menu.addAction(action)
        return menu


def menu_action_ids(bar: QMenuBar) -> list[str]:
    """Every action id reachable from a menu bar (submenus included), for the
    registry-completeness test."""
    ids: list[str] = []

    def walk(menu: QMenu) -> None:
        for action in menu.actions():
            if action.isSeparator():
                continue
            sub = action.menu()
            if isinstance(sub, QMenu):
                walk(sub)
            else:
                ids.append(action.objectName())

    for action in bar.actions():
        sub = action.menu()
        if isinstance(sub, QMenu):
            walk(sub)
    return ids
