"""What every geometry analyzer panel shares: a GeometryView over the analyzer's layers, a
row of options above it, and switching its analyzer on while it is visible."""

from __future__ import annotations

from typing import ClassVar

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QHideEvent, QShowEvent
from PySide6.QtWidgets import QHBoxLayout, QLabel, QVBoxLayout, QWidget

from adx.engine_bridge import AnalyzerFrame
from adx.panels.analyzers.host import AnalyzerHost, on_screen
from adx.panels.base import DockPanel
from adx.panels.geometry_view import GeometryView, Pointer
from adx.theme.tokens import Theme


class AnalyzerPanel(DockPanel):
    """Subclasses name their analyzer, its layers and the world rectangle they draw in."""

    kind: ClassVar[str] = ""
    #: (ANALYZER_LAYERS name, GeometryView layer mode) per layer, drawn in this order.
    layers: ClassVar[tuple[tuple[str, int], ...]] = ()
    #: x0, y0, x1, y1 of the world the layers are built in.
    world: ClassVar[tuple[float, float, float, float]] = (0.0, 0.0, 1.0, 1.0)
    default_area = Qt.DockWidgetArea.BottomDockWidgetArea
    # The option row is wider than a narrow dock: scroll rather than pin the splitter.
    scrolls = True

    def __init__(self, host: AnalyzerHost, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.host = host
        body = QWidget()
        layout = QVBoxLayout(body)
        layout.setContentsMargins(4, 4, 4, 4)
        self.options = QHBoxLayout()
        layout.addLayout(self.options)
        self.view = GeometryView(self, [mode for _name, mode in self.layers], body)
        self.view.setObjectName(f"{self.panel_id}.view")
        self.view.setMinimumHeight(64)
        layout.addWidget(self.view, 1)
        self.readout = QLabel("")
        self.readout.setObjectName(f"{self.panel_id}.readout")
        layout.addWidget(self.readout)
        self.setWidget(body)
        self.uploads = 0
        self.visibilityChanged.connect(lambda _v: QTimer.singleShot(0, self._recheck))

    # --- what subclasses override ----------------------------------------------------

    def settings(self) -> dict[str, object]:
        """This analyzer's settings, as Analyzers.configure takes them."""
        return {}

    def show_readouts(self, frame: AnalyzerFrame) -> None:
        """Numbers under the view, from the frame's readouts."""

    # --- the frame -------------------------------------------------------------------

    # On screen, not merely "visible": a dock tabbed behind another reports isVisible()
    # but has an empty visible region. Rechecked after every show, hide and tab switch
    # (visibilityChanged), a beat later, once the layout has settled.
    def showEvent(self, event: QShowEvent) -> None:  # noqa: N802 - Qt's name
        super().showEvent(event)
        QTimer.singleShot(0, self._recheck)

    def hideEvent(self, event: QHideEvent) -> None:  # noqa: N802 - Qt's name
        super().hideEvent(event)
        QTimer.singleShot(0, self._recheck)

    def _recheck(self) -> None:
        try:
            shown = on_screen(self)
        except RuntimeError:
            return  # deleted meanwhile
        if shown != (self.kind in self.host.open):
            self.host.set_open(self.kind, shown, **(self.settings() if shown else {}))

    def apply(self) -> None:
        """Send changed options to the hub (an option widget calls this)."""
        if self.kind in self.host.open:
            self.host.configure(**self.settings())

    def frame(self, frame: AnalyzerFrame | None) -> None:
        """Upload what the frame clock's one call built: Qt calls, no engine call."""
        if frame is None or not self.isVisible():
            return
        for layer, (name, _mode) in enumerate(self.layers):
            address, count, revision = frame.layers[name]
            self.view.upload_raw(layer, address, count, revision)
        self.uploads += 1
        self.show_readouts(frame)

    def apply_theme(self, theme: Theme) -> None:
        self.view.apply_theme(theme)

    # --- the view's input: none, but it must say where the world is ---------------------

    def on_resize(self, width: float, height: float) -> None:
        x0, y0, x1, y1 = self.world
        self.view.set_view(x0, y0, width / (x1 - x0), height / (y1 - y0))

    def on_press(self, pointer: Pointer) -> None: ...
    def on_move(self, pointer: Pointer) -> None: ...
    def on_release(self, pointer: Pointer) -> None: ...
    def on_double_click(self, pointer: Pointer) -> None: ...
    def on_wheel(self, x: float, y: float, dx: float, dy: float, modifiers: int) -> None: ...
