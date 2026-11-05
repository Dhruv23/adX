"""Synthesized input on real widgets (phase_10.5.md 4.2): what a mouse and a keyboard do,
delivered as Qt events through the same handlers, never as a direct command call - the
wiring between panels is part of what is being tested.

Pointer events go through ``QTest``, which posts real ``QMouseEvent``s to the widget; on a
GeometryView (a QuickHost) they are passed on to the Quick window to geometry.qml's
MouseArea and the panel's handler, as a user's do.

What this does not cover, on purpose: a drag *between* widgets starts ``QDrag.exec``, the
operating system's drag loop, which synthesized events cannot drive on Windows. ``drop``
does the second half instead: it takes the source's own MIME data (the widget's model or
its ``mimeData`` hook) and sends drag-enter, drag-move and drop events to the target, so
both the source's encoding and the target's handling are exercised.
"""

from __future__ import annotations

from PySide6.QtCore import QMimeData, QModelIndex, QPoint, QPointF, Qt
from PySide6.QtGui import (
    QContextMenuEvent,
    QDragEnterEvent,
    QDragMoveEvent,
    QDropEvent,
    QKeySequence,
)
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QAbstractItemView, QApplication, QComboBox, QMenu, QWidget

from adx.control.protocol import AMBIGUOUS, BAD_REQUEST, ControlError
from adx.panels.quick_host import QuickHost

BUTTONS = {
    "left": Qt.MouseButton.LeftButton,
    "right": Qt.MouseButton.RightButton,
    "middle": Qt.MouseButton.MiddleButton,
}
MODIFIERS = {
    "shift": Qt.KeyboardModifier.ShiftModifier,
    "ctrl": Qt.KeyboardModifier.ControlModifier,
    "alt": Qt.KeyboardModifier.AltModifier,
}


def _modifiers(names: list[str] | None) -> Qt.KeyboardModifier:
    out = Qt.KeyboardModifier.NoModifier
    for name in names or []:
        out |= MODIFIERS[name]
    return out


def _receiver(widget: QWidget) -> QWidget:
    """Where mouse events go: a scroll area's viewport, else the widget."""
    if isinstance(widget, QAbstractItemView):
        return widget.viewport()
    return widget


def _point(widget: QWidget, x: float | None, y: float | None) -> QPoint:
    return QPoint(
        int(widget.width() / 2 if x is None else x), int(widget.height() / 2 if y is None else y)
    )


def _settle() -> None:
    QApplication.processEvents()


def click(
    widget: QWidget,
    x: float | None = None,
    y: float | None = None,
    button: str = "left",
    modifiers: list[str] | None = None,
    double: bool = False,
) -> None:
    target = _receiver(widget)
    at = _point(target, x, y)
    mods = _modifiers(modifiers)
    if double:
        QTest.mouseDClick(target, BUTTONS[button], mods, at)
    else:
        QTest.mouseClick(target, BUTTONS[button], mods, at)
    _settle()


def drag(
    widget: QWidget,
    start: tuple[float, float],
    end: tuple[float, float],
    button: str = "left",
    modifiers: list[str] | None = None,
    steps: int = 8,
) -> None:
    """A press, moves and a release inside one widget (a note drag, a fader, a rubber band)."""
    target = _receiver(widget)
    mods = _modifiers(modifiers)
    a = QPoint(int(start[0]), int(start[1]))
    b = QPoint(int(end[0]), int(end[1]))
    QTest.mousePress(target, BUTTONS[button], mods, a)
    for i in range(1, steps + 1):
        p = QPoint(a.x() + (b.x() - a.x()) * i // steps, a.y() + (b.y() - a.y()) * i // steps)
        if isinstance(target, QuickHost):
            # QTest.mouseMove sends no button state; the Quick window needs it to drag.
            QTest.mouseMove(target, p)
            _quick_move(target, p, BUTTONS[button], mods)
        else:
            QTest.mouseMove(target, p)
        _settle()
    QTest.mouseRelease(target, BUTTONS[button], mods, b)
    _settle()


def _quick_move(
    widget: QWidget, at: QPoint, button: Qt.MouseButton, mods: Qt.KeyboardModifier
) -> None:
    from PySide6.QtCore import QEvent
    from PySide6.QtGui import QMouseEvent

    local = QPointF(at)
    event = QMouseEvent(
        QEvent.Type.MouseMove,
        local,
        widget.mapToGlobal(local),
        Qt.MouseButton.NoButton,
        button,
        mods,
    )
    QApplication.sendEvent(widget, event)


def type_text(widget: QWidget, text: str) -> None:
    widget.setFocus()
    QTest.keyClicks(widget, text)
    _settle()


def key(widget: QWidget | None, sequence: str) -> None:
    """A key or chord in QKeySequence's spelling: "Ctrl+Z", "Space", "Delete"."""
    keys = QKeySequence(sequence)
    if keys.isEmpty():
        raise ControlError(f"not a key sequence: {sequence!r}", BAD_REQUEST)
    receiver = widget or QApplication.focusWidget() or QApplication.activeWindow()
    if receiver is None:
        raise ControlError("no widget has focus", BAD_REQUEST)
    for i in range(keys.count()):
        combo = keys[i]  # type: ignore[index]  # indexable at runtime; the stubs lack it
        QTest.keyClick(receiver, combo.key(), combo.keyboardModifiers())
    _settle()


def scroll(
    widget: QWidget,
    dy: float,
    x: float | None = None,
    y: float | None = None,
    modifiers: list[str] | None = None,
) -> None:
    from PySide6.QtCore import QPoint
    from PySide6.QtGui import QWheelEvent

    target = _receiver(widget)
    at = QPointF(_point(target, x, y))
    event = QWheelEvent(
        at,
        target.mapToGlobal(at),
        QPoint(0, 0),
        QPoint(0, int(dy)),
        Qt.MouseButton.NoButton,
        _modifiers(modifiers),
        Qt.ScrollPhase.NoScrollPhase,
        False,
    )
    QApplication.sendEvent(target, event)
    _settle()


def choose(widget: QWidget, text: str | None = None, index: int | None = None) -> None:
    """Pick an item of a combo box by its text (or its position), as the popup's click
    would: the index changes and ``activated`` fires."""
    if not isinstance(widget, QComboBox):
        raise ControlError("choose wants a combo box", BAD_REQUEST)
    if text is not None:
        index = widget.findText(text)
    if index is None or not 0 <= index < widget.count():
        items = [widget.itemText(i) for i in range(widget.count())]
        raise ControlError(
            f"{widget.objectName()} has no item {text if text is not None else index!r} "
            f"({', '.join(items)})",
            BAD_REQUEST,
        )
    widget.setCurrentIndex(index)
    widget.activated.emit(index)
    _settle()


def context_menu(widget: QWidget, x: float | None = None, y: float | None = None) -> None:
    """A right-click's context-menu request: the right press and release, then the
    QContextMenuEvent the window system follows them with (QTest sends no such event)."""
    target = _receiver(widget)
    at = _point(target, x, y)
    QTest.mouseClick(target, Qt.MouseButton.RightButton, Qt.KeyboardModifier.NoModifier, at)
    event = QContextMenuEvent(QContextMenuEvent.Reason.Mouse, at, target.mapToGlobal(at))
    QApplication.sendEvent(target, event)
    _settle()


def item_point(widget: QWidget, item: str | list[str]) -> QPoint:
    """The centre of an item view's row whose columns hold every text in ``item`` (one
    text, or several: ``["Dexed", "CLAP"]``), in the viewport's coordinates."""
    if not isinstance(widget, QAbstractItemView) or widget.model() is None:
        raise ControlError(f"{widget.objectName()} is not an item view", BAD_REQUEST)
    wanted = [item] if isinstance(item, str) else [str(t) for t in item]
    model = widget.model()
    found = []

    def walk(parent: QModelIndex) -> None:
        for row in range(model.rowCount(parent)):
            first = model.index(row, 0, parent)
            texts = [
                str(model.index(row, c, parent).data() or "")
                for c in range(model.columnCount(parent))
            ]
            if all(w in texts for w in wanted):
                found.append(first)
            walk(first)

    walk(QModelIndex())
    if len(found) != 1:
        raise ControlError(
            f"{widget.objectName()}: {len(found)} items match {wanted!r}",
            BAD_REQUEST if not found else AMBIGUOUS,
        )
    widget.scrollTo(found[0])
    return widget.visualRect(found[0]).center()


def choose_menu(text: str) -> str:
    """Click the item of the open popup menu whose text is ``text`` (``&`` ignored), as a
    mouse would: on the item's own rectangle."""
    menu = QApplication.activePopupWidget()
    if not isinstance(menu, QMenu):
        raise ControlError("no menu is open", BAD_REQUEST)
    wanted = text.replace("&", "").lower()
    for action in menu.actions():
        if action.text().replace("&", "").lower() == wanted:
            if not action.isEnabled():
                raise ControlError(f"menu item {text!r} is disabled", BAD_REQUEST)
            QTest.mouseClick(
                menu,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
                menu.actionGeometry(action).center(),
            )
            _settle()
            return action.text()
    items = [a.text() for a in menu.actions() if a.text()]
    raise ControlError(f"the menu has no item {text!r} ({', '.join(items)})", BAD_REQUEST)


def drop(
    source_mime: QMimeData,
    target: QWidget,
    x: float | None = None,
    y: float | None = None,
    action: Qt.DropAction = Qt.DropAction.CopyAction,
) -> bool:
    """The second half of a drag: enter, move and drop on ``target``. True if accepted."""
    receiver = _receiver(target)
    at = QPointF(_point(receiver, x, y))
    buttons = Qt.MouseButton.LeftButton
    mods = Qt.KeyboardModifier.NoModifier
    enter = QDragEnterEvent(at.toPoint(), action, source_mime, buttons, mods)
    QApplication.sendEvent(receiver, enter)
    if not enter.isAccepted():
        return False
    move = QDragMoveEvent(at.toPoint(), action, source_mime, buttons, mods)
    QApplication.sendEvent(receiver, move)
    event = QDropEvent(at, action, source_mime, buttons, mods)
    QApplication.sendEvent(receiver, event)
    _settle()
    return event.isAccepted()


def mime_from(widget: QWidget, row: int | None = None, text: str | None = None) -> QMimeData:
    """A drag source's own MIME data: an item view's model for the item at ``row`` or
    with ``text``, or the widget's ``drag_mime()`` hook."""
    if isinstance(widget, QAbstractItemView) and widget.model() is not None:
        model = widget.model()
        index = None
        if text is not None:
            matches = model.match(
                model.index(0, 0),
                Qt.ItemDataRole.DisplayRole,
                text,
                1,
                Qt.MatchFlag.MatchExactly | Qt.MatchFlag.MatchRecursive,
            )
            index = matches[0] if matches else None
        elif row is not None:
            index = model.index(row, 0)
        if index is None or not index.isValid():
            raise ControlError(f"{widget.objectName()} has no item {text or row!r}", BAD_REQUEST)
        mime = model.mimeData([index])
        if mime is None:
            raise ControlError(f"{widget.objectName()}'s model makes no drag data", BAD_REQUEST)
        return mime
    hook = getattr(widget, "drag_mime", None)
    if callable(hook):
        mime = hook()
        if isinstance(mime, QMimeData):
            return mime
    raise ControlError(f"{widget.objectName()} is not a drag source", BAD_REQUEST)
