"""The widget tree, as the control channel shows it (phase_10.5.md 4.2).

Addressing is by objectName. A panel is a dock named by its ``panel_id``; widgets inside
carry names like ``spectrum.fft_size``. A target is either one name, which must be unique
among visible widgets, or a path of names joined by ``/`` from any named ancestor
(``mixer/insert:1/mute``) when a name repeats.

Widgets that were not given a name get one here, once, when the server starts and when a
panel builds new widgets: the nearest named ancestor's name, then the widget's text,
tooltip or kind and its index among like siblings (``mixer.button.Mute.0``). The
construction order is fixed, so the names are the same on every launch
(``control_tree_names_stable``).
"""

from __future__ import annotations

import re
from typing import Any

from PySide6.QtCore import QObject, QPoint
from PySide6.QtQuickWidgets import QQuickWidget
from PySide6.QtWidgets import (
    QAbstractButton,
    QAbstractItemView,
    QAbstractSlider,
    QAbstractSpinBox,
    QApplication,
    QComboBox,
    QLabel,
    QLineEdit,
    QPlainTextEdit,
    QTabBar,
    QTextEdit,
    QWidget,
)

from adx.control.protocol import AMBIGUOUS, NO_TARGET, ControlError, Json

#: What a user can press, type into or drag: what must have a name.
INTERACTIVE: tuple[type[QWidget], ...] = (
    QAbstractButton,
    QComboBox,
    QAbstractSpinBox,
    QLineEdit,
    QAbstractSlider,
    QAbstractItemView,
    QQuickWidget,
    QPlainTextEdit,
    QTextEdit,
    QTabBar,
)


def _slug(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "-", text).strip("-")[:32]


def _kind(widget: QWidget) -> str:
    for base, name in (
        (QAbstractButton, "button"),
        (QComboBox, "combo"),
        (QAbstractSpinBox, "spin"),
        (QLineEdit, "field"),
        (QAbstractSlider, "slider"),
        (QAbstractItemView, "list"),
        (QQuickWidget, "view"),
        (QPlainTextEdit, "text"),
        (QTextEdit, "text"),
        (QTabBar, "tabs"),
    ):
        if isinstance(widget, base):
            return name
    return type(widget).__name__


def named_ancestor(widget: QObject | None) -> QObject | None:
    # Qt's own names (qt_scrollarea_viewport, ...) repeat in every panel; skip them.
    parent = widget.parent() if widget is not None else None
    while parent is not None and (not parent.objectName() or parent.objectName().startswith("qt_")):
        parent = parent.parent()
    return parent


def ensure_names(root: QWidget) -> int:
    """Name every unnamed interactive widget under ``root``; returns how many it named."""
    named = 0
    counters: dict[tuple[int, str], int] = {}
    for widget in root.findChildren(QWidget):
        if widget.objectName() or not isinstance(widget, INTERACTIVE):
            continue
        anchor = named_ancestor(widget)
        prefix = anchor.objectName() if anchor is not None else "window"
        label = ""
        text = getattr(widget, "text", None)
        if callable(text) and isinstance(widget, (QAbstractButton,)):
            label = _slug(str(text()))
        if not label and widget.toolTip():
            label = _slug(widget.toolTip())
        stem = f"{prefix}.{_kind(widget)}" + (f".{label}" if label else "")
        key = (id(anchor), stem)
        index = counters.get(key, 0)
        counters[key] = index + 1
        widget.setObjectName(f"{stem}.{index}")
        named += 1
    return named


def _roots() -> list[QWidget]:
    app = QApplication.instance()
    if not isinstance(app, QApplication):
        return []
    return [w for w in app.topLevelWidgets() if w.isVisible()]


def _widgets(roots: list[QWidget]) -> list[QWidget]:
    """Every widget under ``roots``, each once: a window parented to the main window (a
    plugin editor) is a root of its own and the main window's child too."""
    seen: set[int] = set()
    out = []
    for root in roots:
        for widget in [root, *root.findChildren(QWidget)]:
            if id(widget) not in seen:
                seen.add(id(widget))
                out.append(widget)
    return out


def _value(widget: QWidget) -> Json:
    if isinstance(widget, QAbstractButton) and widget.isCheckable():
        return widget.isChecked()
    if isinstance(widget, QComboBox):
        return widget.currentText()
    if isinstance(widget, (QAbstractSpinBox,)):
        value = getattr(widget, "value", None)
        return value() if callable(value) else None
    if isinstance(widget, QAbstractSlider):
        return widget.value()
    if isinstance(widget, QLineEdit):
        return widget.text()
    if isinstance(widget, QPlainTextEdit):
        return widget.toPlainText()[:2000]
    own = getattr(widget, "value", None)  # adX's knobs and faders: a property
    if isinstance(own, (int, float)) and not isinstance(own, bool):
        return float(own)
    return None


def describe(widget: QWidget) -> dict[str, Any]:
    top_left = widget.mapToGlobal(QPoint(0, 0))
    text = ""
    if isinstance(widget, (QAbstractButton, QLabel, QLineEdit)):
        text = widget.text()
    elif isinstance(widget, QComboBox):
        text = widget.currentText()
    return {
        "name": widget.objectName(),
        "path": path_of(widget),
        "class": type(widget).__name__,
        "geometry": [top_left.x(), top_left.y(), widget.width(), widget.height()],
        "visible": widget.isVisible(),
        "enabled": widget.isEnabled(),
        "text": text,
        "value": _value(widget),
    }


def path_of(widget: QObject) -> str:
    names = []
    node: QObject | None = widget
    while node is not None:
        if node.objectName():
            names.append(node.objectName())
        node = node.parent()
    return "/".join(reversed(names))


def tree(root: QWidget | None = None, interactive_only: bool = True) -> list[dict[str, Any]]:
    roots = [root] if root is not None else _roots()
    out = []
    for widget in _widgets(roots):
        if not widget.isVisible():
            continue
        if interactive_only and not isinstance(widget, INTERACTIVE) and widget not in roots:
            continue
        out.append(describe(widget))
    return out


def resolve(target: str) -> QWidget:
    """The one visible widget ``target`` names (see the module docstring)."""
    parts = [p for p in target.split("/") if p]
    if not parts:
        raise ControlError("an empty target", NO_TARGET)
    candidates = [w for w in _widgets(_roots()) if w.objectName() == parts[-1]]
    if len(parts) > 1:
        candidates = [w for w in candidates if _has_ancestors(w, parts[:-1])]
    visible = [w for w in candidates if w.isVisible()]
    found = visible or candidates
    if not found:
        raise ControlError(f"no widget named {target!r}", NO_TARGET)
    if len(found) > 1:
        paths = ", ".join(path_of(w) for w in found[:6])
        raise ControlError(f"{target!r} names {len(found)} widgets: {paths}", AMBIGUOUS)
    return found[0]


def _has_ancestors(widget: QObject, names: list[str]) -> bool:
    """``names`` appear, in order, among the widget's named ancestors."""
    chain = path_of(widget).split("/")[:-1]
    at = 0
    for name in chain:
        if at < len(names) and name == names[at]:
            at += 1
    return at == len(names)


def find(name: str = "", cls: str = "", text: str = "") -> list[dict[str, Any]]:
    out = []
    for widget in _widgets(_roots()):
        if not widget.isVisible():
            continue
        if name and name not in widget.objectName():
            continue
        if cls and type(widget).__name__ != cls:
            continue
        if text:
            own = getattr(widget, "text", None)
            if not callable(own) or text not in str(own()):
                continue
        out.append(describe(widget))
    return out
