"""A Qt Quick scene in its own native window, embedded in the widget tree.

Every hot surface (the playlist, the piano roll, the mixer's meters, the analyzers, the
waveform, the step grid, automation lanes) used to be a QQuickWidget. A QQuickWidget
renders on the GUI thread into a texture that the widget compositor then draws and
presents with the rest of the window. With eight of them animating at once that path
held the display near 45 fps, while the frame work itself was under a millisecond
(bench/bench_ui.py, STATE.md P10-6).

Here the scene is a QQuickView inside ``QWidget.createWindowContainer``: a native child
window with its own swapchain, drawn by Qt's threaded render loop, which syncs on the GUI
thread and renders and presents on its own thread. The GUI thread no longer waits on any
present.

:class:`QuickHost` keeps the slice of QQuickWidget's API that panels use, so a panel only
changes its base class. Input a user makes lands in the native window directly. Input
synthesized on the host (QTest, the control channel's ``inputs``) is passed on to the
window, and drags the window receives are passed back to the host, where panels filter
them as before.

On Qt's offscreen platform (the test suite's) the host holds a QQuickWidget instead:
qoffscreen.dll dereferences null destroying a native child window whose dock was floated
(STATE.md P10-6), and offscreen presents nothing, so there is no frame rate to win there.
Everything above the scene - the API, input forwarding, drags - is the same code.
"""

from __future__ import annotations

from PySide6.QtCore import QEvent, QObject, QPointF, Qt, QUrl
from PySide6.QtGui import QColor, QGuiApplication, QImage, QKeyEvent, QMouseEvent, QWheelEvent
from PySide6.QtQml import QQmlContext, QQmlError
from PySide6.QtQuick import QQuickItem, QQuickView, QQuickWindow
from PySide6.QtQuickWidgets import QQuickWidget
from PySide6.QtWidgets import QApplication, QVBoxLayout, QWidget

_MOUSE = (
    QEvent.Type.MouseButtonPress,
    QEvent.Type.MouseButtonRelease,
    QEvent.Type.MouseButtonDblClick,
    QEvent.Type.MouseMove,
)
_KEYS = (QEvent.Type.KeyPress, QEvent.Type.KeyRelease)
_DRAGS = (QEvent.Type.DragEnter, QEvent.Type.DragMove, QEvent.Type.DragLeave, QEvent.Type.Drop)


class QuickHost(QWidget):
    """A QQuickView in a window container, with QQuickWidget's calls."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self._view: QQuickView | None = None
        self._widget: QQuickWidget | None = None
        if QGuiApplication.platformName() == "offscreen":
            self._widget = QQuickWidget(self)
            self._widget.setResizeMode(QQuickWidget.ResizeMode.SizeRootObjectToView)
            self._target: QObject = self._widget
            self._window: QQuickWindow = self._widget.quickWindow()
            body: QWidget = self._widget
        else:
            self._view = QQuickView()
            self._view.setResizeMode(QQuickView.ResizeMode.SizeRootObjectToView)
            self._view.installEventFilter(self)
            self._target = self._window = self._view
            body = QWidget.createWindowContainer(self._view, self)
            body.setMinimumSize(1, 1)
            # Qt reports the container as the focus widget when the scene has focus, so
            # input synthesized on "the focused widget" lands there: pass it on too.
            body.installEventFilter(self)
            self._body = body
        body.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setFocusProxy(body)
        layout.addWidget(body)

    @property
    def native(self) -> bool:
        """Whether the scene is a native window (False on the offscreen platform)."""
        return self._view is not None

    # --- QQuickWidget's API, as the panels use it ---------------------------------

    def rootContext(self) -> QQmlContext:  # noqa: N802 - QQuickWidget's name
        scene = self._view or self._widget
        assert scene is not None
        return scene.rootContext()

    def setSource(self, url: QUrl) -> None:  # noqa: N802 - QQuickWidget's name
        scene = self._view or self._widget
        assert scene is not None
        scene.setSource(url)

    def errors(self) -> list[QQmlError]:
        scene = self._view or self._widget
        assert scene is not None
        return list(scene.errors())

    def rootObject(self) -> QQuickItem | None:  # noqa: N802 - QQuickWidget's name
        scene = self._view or self._widget
        assert scene is not None
        return scene.rootObject()

    def setClearColor(self, color: QColor) -> None:  # noqa: N802 - QQuickWidget's name
        if self._widget is not None:
            self._widget.setClearColor(color)
        else:
            self._window.setColor(color)

    def quickWindow(self) -> QQuickWindow:  # noqa: N802 - QQuickWidget's name
        return self._window

    def grabFramebuffer(self) -> QImage:  # noqa: N802 - QQuickWidget's name
        if self._widget is not None:
            return self._widget.grabFramebuffer()
        return self._window.grabWindow()

    def request_frame(self) -> None:
        """Schedule a redraw of the scene (a QWidget.update() would not reach it)."""
        self._window.update()

    # --- input in both directions -------------------------------------------------

    def event(self, event: QEvent) -> bool:
        kind = event.type()
        if kind in _MOUSE and isinstance(event, QMouseEvent):
            local = QPointF(event.position())
            clone = QMouseEvent(
                kind,
                local,
                local,
                event.globalPosition(),
                event.button(),
                event.buttons(),
                event.modifiers(),
            )
            QApplication.sendEvent(self._target, clone)
            return True
        if kind == QEvent.Type.Wheel and isinstance(event, QWheelEvent):
            clone_wheel = QWheelEvent(
                event.position(),
                event.globalPosition(),
                event.pixelDelta(),
                event.angleDelta(),
                event.buttons(),
                event.modifiers(),
                event.phase(),
                event.inverted(),
            )
            QApplication.sendEvent(self._target, clone_wheel)
            return True
        if (kind in _KEYS or kind == QEvent.Type.ShortcutOverride) and isinstance(event, QKeyEvent):
            clone_key = QKeyEvent(
                kind,
                event.key(),
                event.modifiers(),
                event.text(),
                event.isAutoRepeat(),
                event.count(),
            )
            clone_key.setAccepted(False)
            QApplication.sendEvent(self._key_target(), clone_key)
            if kind == QEvent.Type.ShortcutOverride:
                # The scene claims the key (a text field in it) or leaves it to shortcuts.
                event.setAccepted(clone_key.isAccepted())
            return True
        return super().event(event)

    def _key_target(self) -> QObject:
        """Where a forwarded key goes. A native Quick window gives its focus item active
        focus only while it is the OS focus window, which a click makes it for a user
        but synthesized input never does: then the item holding focus gets the key."""
        if self._view is None:
            return self._target
        active = self._view.activeFocusItem()
        if active is not None and active is not self._view.contentItem():
            return active
        root = self._view.rootObject()
        stack = [root] if root is not None else []
        found: QQuickItem | None = None
        while stack:
            item = stack.pop()
            if item.hasFocus() and item is not root:
                found = item
            stack.extend(item.childItems())
        return found or self._target

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802 - Qt's name
        if self._view is not None and watched is getattr(self, "_body", None):
            kind = event.type()
            if (
                kind in _MOUSE
                or kind in _KEYS
                or kind
                in (
                    QEvent.Type.Wheel,
                    QEvent.Type.ShortcutOverride,
                )
            ):
                return self.event(event)
        if self._view is not None and watched is self._view and event.type() in _DRAGS:
            # A drop on the native window: deliver it to the host, where the panel's
            # filter (the playlist's) decides, as it did on the QQuickWidget.
            QApplication.sendEvent(self, event)
            return True
        return super().eventFilter(watched, event)
