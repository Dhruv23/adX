"""A QQuickWidget hosting one GeometryItem: the shared body of every Phase 6 hot surface
(the playlist, the step grid, automation lanes, the mixer's meters).

Python owns the layout, the view transform and what input means; C++ builds the
vertices (engine/geometry/) and draws them (bindings/qml/GeometryItem.h). A pan or zoom
is :meth:`GeometryView.set_view` - a matrix, no engine call - and a rebuild is one
builder call plus one lease and one memcpy per layer (:meth:`GeometryView.upload`).
"""

from __future__ import annotations

from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol, cast

from PySide6.QtCore import QObject, Qt, QUrl, Signal, Slot
from PySide6.QtGui import QColor
from PySide6.QtQuick import QQuickItem
from PySide6.QtQuickWidgets import QQuickWidget
from PySide6.QtWidgets import QWidget

from adx.engine_bridge import GeometryView as Lease
from adx.quick import ULong, call
from adx.theme.tokens import DARK, Theme, qml_palette, role_colors

QML = Path(__file__).with_name("geometry.qml")

#: GeometryItem layer modes (bindings/qml/GeometryItem.h, LayerMode).
WORLD_TRIANGLES, WORLD_LINES, PIXEL_TRIANGLES, PIXEL_LINES = 0, 1, 2, 3

LEFT = int(Qt.MouseButton.LeftButton.value)
RIGHT = int(Qt.MouseButton.RightButton.value)
MIDDLE = int(Qt.MouseButton.MiddleButton.value)
SHIFT = int(Qt.KeyboardModifier.ShiftModifier.value)
CTRL = int(Qt.KeyboardModifier.ControlModifier.value)
ALT = int(Qt.KeyboardModifier.AltModifier.value)


@dataclass(frozen=True, slots=True)
class Pointer:
    """One pointer event in item pixels."""

    x: float
    y: float
    button: int = LEFT
    modifiers: int = 0

    @property
    def shift(self) -> bool:
        return bool(self.modifiers & SHIFT)

    @property
    def ctrl(self) -> bool:
        return bool(self.modifiers & CTRL)

    @property
    def alt(self) -> bool:
        return bool(self.modifiers & ALT)


class InputHandler(Protocol):
    """What a panel implements to receive a GeometryView's input."""

    def on_press(self, pointer: Pointer) -> None: ...
    def on_move(self, pointer: Pointer) -> None: ...
    def on_release(self, pointer: Pointer) -> None: ...
    def on_double_click(self, pointer: Pointer) -> None: ...
    def on_wheel(self, x: float, y: float, dx: float, dy: float, modifiers: int) -> None: ...
    def on_resize(self, width: float, height: float) -> None: ...


class _Forward(QObject):
    """Where geometry.qml sends the item's raw input."""

    def __init__(self, handler: InputHandler, parent: QObject) -> None:
        super().__init__(parent)
        self.handler = handler

    @Slot(float, float, int, int)
    def pressed(self, x: float, y: float, button: int, modifiers: int) -> None:
        view = self.parent()
        if isinstance(view, GeometryView):
            view.gesture_started.emit()
        self.handler.on_press(Pointer(x, y, button, modifiers))

    @Slot(float, float, int, int)
    def moved(self, x: float, y: float, buttons: int, modifiers: int) -> None:
        self.handler.on_move(Pointer(x, y, buttons, modifiers))

    @Slot(float, float, int, int)
    def released(self, x: float, y: float, button: int, modifiers: int) -> None:
        self.handler.on_release(Pointer(x, y, button, modifiers))

    @Slot(float, float, int)
    def doubleClicked(self, x: float, y: float, modifiers: int) -> None:  # noqa: N802 - QML
        self.handler.on_double_click(Pointer(x, y, LEFT, modifiers))

    @Slot(float, float, int)
    def hovered(self, x: float, y: float, modifiers: int) -> None:
        pass

    @Slot(float, float, float, float, int)
    def wheeled(self, x: float, y: float, dx: float, dy: float, modifiers: int) -> None:
        self.handler.on_wheel(x, y, dx, dy, modifiers)

    @Slot(int, int, str)
    def keyPressed(self, key: int, modifiers: int, text: str) -> None:  # noqa: N802 - QML
        on_key: Callable[[int, int], None] | None = getattr(self.handler, "on_key", None)
        if on_key is not None:
            on_key(key, modifiers)

    @Slot(float, float)
    def resized(self, width: float, height: float) -> None:
        self.handler.on_resize(width, height)


class GeometryView(QQuickWidget):
    """One GeometryItem in a widget; layers declared once, uploaded per build."""

    #: A press arrived, before the handler sees it: a new gesture, so a new history
    #: entry (the main window seals the command stack's coalescing on it).
    gesture_started = Signal()

    def __init__(
        self, handler: InputHandler, modes: Iterable[int], parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.setResizeMode(QQuickWidget.ResizeMode.SizeRootObjectToView)
        #: Where geometry.qml delivers input - and where a test delivers it too.
        self.forward = _Forward(handler, self)
        context = self.rootContext()
        context.setContextProperty("viewInput", self.forward)
        context.setContextProperty("adxTheme", qml_palette(DARK))
        self.setSource(QUrl.fromLocalFile(str(QML)))
        errors = self.errors()
        if errors:
            raise RuntimeError("geometry QML failed: " + "; ".join(e.toString() for e in errors))
        root = self.rootObject()
        item = root.findChild(QQuickItem, "view") if root is not None else None
        if root is None or item is None:
            raise RuntimeError("geometry QML has no GeometryItem")
        self.root: QQuickItem = root
        self.item: QQuickItem = item
        self.modes = list(modes)
        call(self.item, "setLayers", list(self.modes))
        self.apply_theme(DARK)

    def set_view(self, origin_x: float, origin_y: float, scale_x: float, scale_y: float) -> None:
        """World to pixels. A matrix: no rebuild, no engine call."""
        call(self.item, "setView", float(origin_x), float(origin_y), float(scale_x), float(scale_y))

    def upload(self, layer: int, lease: Lease) -> None:
        """Copy one layer from a leased engine buffer: call inside the lease's block."""
        call(
            self.item,
            "upload",
            layer,
            ULong(lease.address),
            lease.vertex_count,
            ULong(lease.revision),
        )

    def upload_raw(self, layer: int, address: int, count: int, revision: int) -> None:
        """Copy from an address the engine just returned (Engine.frame_with_meters)."""
        call(self.item, "upload", layer, ULong(address), count, ULong(revision))

    def clear(self) -> None:
        """Empty every layer."""
        for layer in range(len(self.modes)):
            call(self.item, "upload", layer, ULong(0), 0, ULong(0))

    def set_playhead(self, x: float) -> None:
        """The playhead's world x; negative hides it. Never dirties a layer."""
        self.item.setProperty("playheadX", float(x))

    def set_selection_rect(self, rect: tuple[float, float, float, float] | None) -> None:
        """The rubber band, in pixels, or none."""
        if rect is None:
            call(self.item, "setSelectionRect", 0.0, 0.0, 0.0, 0.0, False)
        else:
            call(self.item, "setSelectionRect", *[float(c) for c in rect], True)

    def set_labels(self, labels: list[list[object]]) -> None:
        """[x, y, width, text] per label, in pixels."""
        self.root.setProperty("labels", labels)

    def set_hint(self, text: str) -> None:
        """The corner hint line."""
        self.root.setProperty("hint", text)

    def apply_theme(self, theme: Theme) -> None:
        """Recolour every layer: no new geometry. The clear colour too - a QQuickWidget
        clears to white, which showed through wherever no geometry was drawn."""
        self.rootContext().setContextProperty("adxTheme", qml_palette(theme))
        self.setClearColor(QColor(theme.window))
        call(self.item, "setPalette", list(role_colors(theme)))

    def paint_counts(self) -> tuple[int, int, int]:
        """(paint updates, geometry refills, transform-only updates) - for tests."""
        from PySide6.QtCore import Q_RETURN_ARG, QMetaObject

        result = QMetaObject.invokeMethod(
            self.item,
            "paintCounts",
            Qt.ConnectionType.DirectConnection,
            Q_RETURN_ARG("QVariantList"),
        )
        values = [int(v) for v in cast("list[int]", result)] if result else [0, 0, 0]
        return values[0], values[1], values[2]

    def vertex_count(self, layer: int) -> int:
        """Vertices staged in a layer - for tests."""
        from PySide6.QtCore import Q_ARG, Q_RETURN_ARG, QMetaObject

        result = QMetaObject.invokeMethod(
            self.item,
            "layerVertexCount",
            Qt.ConnectionType.DirectConnection,
            Q_RETURN_ARG(int),
            Q_ARG(int, layer),
        )
        return int(cast(int, result)) if result is not None else 0
