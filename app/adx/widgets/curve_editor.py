"""A breakpoint shape you drag: an envelope's ADSR, a GrossBeat volume curve
(phase_6.md 4.4: "Envelope/ADSR anywhere: needs the shared curve_editor widget").

Points are (x, y) with y in 0..1 and x in the editor's own units (seconds, steps); each
segment carries the curve of the point that starts it. Drawing tessellates the shape
with the engine's own core::Curve (through AutomationGeometry), so the line on screen is
the shape the engine plays - the UI never reimplements curve maths. Drag a point to move
it; alt-drag a segment to bend it (its tension).
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPainterPath, QPaintEvent, QPen
from PySide6.QtWidgets import QWidget

from adx import engine_bridge
from adx.engine_bridge import PPQ, AutomationGeometry
from adx.theme.tokens import DARK, Theme

#: Curve kinds (engine/core/Curve.h).
LINEAR, EXPONENTIAL, LOGARITHMIC, STEP, SMOOTH, BEZIER, HOLD = range(7)
_HANDLE = 5.0


@dataclass(slots=True)
class Shape:
    """Points, and the curve of each segment (the one starting at that point)."""

    xs: list[float] = field(default_factory=list)
    ys: list[float] = field(default_factory=list)
    kinds: list[int] = field(default_factory=list)
    tensions: list[float] = field(default_factory=list)
    #: Which points may move horizontally / vertically.
    movable_x: list[bool] = field(default_factory=list)
    movable_y: list[bool] = field(default_factory=list)

    def points_array(self, x_scale: float) -> np.ndarray:
        """(n, 4) for AutomationGeometry: tick, value, kind, tension."""
        out = np.zeros((len(self.xs), 4), dtype=np.float64)
        out[:, 0] = np.asarray(self.xs, dtype=np.float64) * x_scale
        out[:, 1] = self.ys
        out[:, 2] = self.kinds
        out[:, 3] = self.tensions
        return out


class CurveEditor(QWidget):
    """Emits ``dragging(Shape)`` while dragging and ``edited(Shape)`` once per gesture."""

    dragging = Signal(object)
    edited = Signal(object)

    def __init__(self, shape: Shape | None = None, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.shape = shape or Shape()
        self.x_max = 1.0
        self._theme: Theme = DARK
        self._geometry = AutomationGeometry()
        self._polyline: list[QPointF] = []
        self._grab: tuple[str, int] | None = None
        self._grab_y = 0.0
        self.setMinimumSize(160, 70)

    def set_shape(self, shape: Shape, x_max: float | None = None) -> None:
        """Show a shape without emitting; re-tessellates through the engine."""
        self.shape = shape
        self.x_max = max(x_max if x_max is not None else (max(shape.xs) if shape.xs else 1.0), 1e-6)
        self._tessellate()
        self.update()

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self._theme = theme
        self.update()

    # --- geometry -------------------------------------------------------------------

    def _plot(self) -> QRectF:
        return QRectF(6, 6, max(1, self.width() - 12), max(1, self.height() - 12))

    def _tessellate(self) -> None:
        """The shape as the engine's curve evaluator draws it: one build, one lease."""
        if len(self.shape.xs) < 2:
            self._polyline = []
            return
        scale = PPQ * 64.0 / self.x_max
        points = self.shape.points_array(scale)
        self._geometry.build(
            points,
            (0.0, 1.0, False),
            (0.0, PPQ * 64.0, 1.0, 100.0),
            np.zeros(0, dtype=np.uint8),
            segments=32,
        )
        plot = self._plot()
        with self._geometry.view(0) as view:
            vertices = np.array(view.array, copy=True)
        line: list[QPointF] = []
        for i in range(0, len(vertices), 2):
            for v in (vertices[i], vertices[i + 1]):
                beats, y = float(v[0]), float(v[1])
                x = plot.left() + beats * PPQ / (PPQ * 64.0) * plot.width()
                line.append(QPointF(x, plot.top() + y * plot.height()))
        self._polyline = line

    def _point_px(self, index: int) -> QPointF:
        plot = self._plot()
        return QPointF(
            plot.left() + self.shape.xs[index] / self.x_max * plot.width(),
            plot.top() + (1.0 - self.shape.ys[index]) * plot.height(),
        )

    # --- painting -------------------------------------------------------------------

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        theme = self._theme
        painter.fillRect(self.rect(), QColor(theme.raised))
        if self._polyline:
            path = QPainterPath(self._polyline[0])
            for point in self._polyline[1:]:
                path.lineTo(point)
            painter.setPen(QPen(QColor(theme.accent), 2))
            painter.drawPath(path)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(theme.text))
        for i in range(len(self.shape.xs)):
            if self.shape.movable_x[i] or self.shape.movable_y[i]:
                painter.drawEllipse(self._point_px(i), _HANDLE - 1, _HANDLE - 1)
        painter.end()

    # --- editing --------------------------------------------------------------------

    def _hit_point(self, at: QPointF) -> int:
        for i in range(len(self.shape.xs) - 1, -1, -1):
            if not (self.shape.movable_x[i] or self.shape.movable_y[i]):
                continue
            p = self._point_px(i)
            if abs(p.x() - at.x()) <= _HANDLE + 2 and abs(p.y() - at.y()) <= _HANDLE + 2:
                return i
        return -1

    def _hit_segment(self, at: QPointF) -> int:
        for i in range(len(self.shape.xs) - 1):
            if self._point_px(i).x() <= at.x() <= self._point_px(i + 1).x():
                return i
        return -1

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        at = event.position()
        if event.modifiers() & Qt.KeyboardModifier.AltModifier:
            segment = self._hit_segment(at)
            if segment >= 0:
                self._grab = ("bend", segment)
                self._grab_y = at.y()
            return
        index = self._hit_point(at)
        if index >= 0:
            self._grab = ("point", index)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._grab is None:
            return
        what, index = self._grab
        at = event.position()
        plot = self._plot()
        shape = self.shape
        if what == "bend":
            dy = (self._grab_y - at.y()) / max(1.0, plot.height())
            self._grab_y = at.y()
            if shape.kinds[index] == LINEAR:
                shape.kinds[index] = EXPONENTIAL
            shape.tensions[index] = max(-1.0, min(1.0, shape.tensions[index] + dy * 2.0))
        else:
            if shape.movable_x[index]:
                low = shape.xs[index - 1] if index > 0 else 0.0
                high = shape.xs[index + 1] if index + 1 < len(shape.xs) else self.x_max
                x = (at.x() - plot.left()) / plot.width() * self.x_max
                shape.xs[index] = max(low, min(high, x))
            if shape.movable_y[index]:
                shape.ys[index] = max(0.0, min(1.0, 1.0 - (at.y() - plot.top()) / plot.height()))
        self._tessellate()
        self.update()
        self.dragging.emit(shape)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._grab is not None:
            self._grab = None
            self.edited.emit(self.shape)


def adsr_shape(
    attack: float, decay: float, sustain: float, release: float, curves: list[tuple[int, float]]
) -> Shape:
    """An ADSR as a Shape: 0 -> peak -> sustain, held, -> 0. ``curves`` gives each
    stage's (kind, tension); x is seconds, with a fixed half-second sustain hold drawn."""
    hold = 0.5
    xs = [0.0, attack, attack + decay, attack + decay + hold, attack + decay + hold + release]
    ys = [0.0, 1.0, sustain, sustain, 0.0]
    kinds = [curves[0][0], curves[1][0], LINEAR, curves[2][0], LINEAR]
    tensions = [curves[0][1], curves[1][1], 0.0, curves[2][1], 0.0]
    return Shape(
        xs=xs,
        ys=ys,
        kinds=kinds,
        tensions=tensions,
        movable_x=[False, True, True, False, True],
        movable_y=[False, False, True, False, False],
    )


def adsr_values(shape: Shape) -> tuple[float, float, float, float]:
    """(attack, decay, sustain, release) back out of an ADSR shape."""
    xs, ys = shape.xs, shape.ys
    return xs[1], xs[2] - xs[1], ys[2], xs[4] - xs[3]


def evaluate(kind: int, tension: float, t: float) -> float:
    """The engine's curve at t (a tooltip's arithmetic: one call)."""
    return engine_bridge.curve_evaluate(kind, tension, 0.33, 0.0, 0.67, 1.0, t)
