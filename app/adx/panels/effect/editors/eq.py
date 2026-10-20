"""The EQ editor: a draggable response curve over a live spectrum (phase_6.md 4.4).

The curve is EqCurveGeometry's - the effect's own biquad designs evaluated by the engine -
and each band is a handle: drag across for frequency, up and down for gain. Under it, the
spectrum of what the EQ is putting out, from ParametricEq's tap (Engine.eq_spectrum),
refreshed at 30 Hz while the editor is visible. The three-band "EQ" has fixed frequencies,
so its handles move up and down only.
"""

from __future__ import annotations

import numpy as np
from PySide6.QtCore import QPointF, QRectF, Qt, QTimer
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPainterPath, QPaintEvent, QPen
from PySide6.QtWidgets import QWidget

from adx import engine_bridge
from adx.engine_bridge import EqCurveGeometry
from adx.panels.effect.generic_editor import params_in_order, slot_target
from adx.session import Session
from adx.theme.tokens import DARK

RANGE_DB = 24.0
#: The v1 three-band EQ's fixed bands.
EQ3 = (("low", 250.0), ("mid", 1200.0), ("high", 4000.0))


class EqView(QWidget):
    """Curve, handles and spectrum."""

    def __init__(self, session: Session, insert: int, slot: int, effect: str) -> None:
        super().__init__()
        self.session = session
        self.insert, self.slot, self.effect = insert, slot, effect
        self.target = slot_target(session, insert, slot)
        self.curve = EqCurveGeometry()
        self.response = np.zeros(0)
        self._curve: list[QPointF] = []
        self._spectrum: list[QPointF] = []
        self._grab: int | None = None
        self.setMinimumHeight(180)
        self._timer = QTimer(self)
        self._timer.setInterval(33)
        self._timer.timeout.connect(self._spectrum_tick)
        self.refresh()

    def bands(self) -> list[tuple[str, float, float]]:
        """(prefix, frequency, gain) per active band, read now."""
        values = self.target.read_values()
        if self.effect == "EQ":
            return [(name, hz, values.get(name, 0.0)) for name, hz in EQ3]
        out = []
        for band in range(1, 9):
            kind = values.get(f"band{band}.type", 0.0)
            if round(kind) != 0:
                out.append(
                    (
                        f"band{band}",
                        values.get(f"band{band}.freq", 1000.0),
                        values.get(f"band{band}.gain", 0.0),
                    )
                )
        return out

    def refresh(self) -> None:
        """Recompute the curve through the engine."""
        params = np.asarray(
            params_in_order(self.session, self.insert, self.slot, self.effect), dtype=np.float32
        )
        self.response = self.curve.build(
            self.effect, params, float(self.session.engine.sample_rate)
        )
        width, height = max(1, self.width()), max(1, self.height())
        count = max(1, len(self.response) - 1)
        self._curve = [
            QPointF(i / count * width, self._y(db) * height) for i, db in enumerate(self.response)
        ]
        self.update()

    @staticmethod
    def _y(db: float) -> float:
        return min(1.0, max(0.0, 0.5 - db / (2 * RANGE_DB)))

    def _spectrum_tick(self) -> None:
        if not self.isVisible() or self.effect != "ParametricEq":
            return
        if self.session.engine.eq_spectrum(self.slot, self.curve):
            with self.curve.view(2) as view:
                vertices = np.array(view.array, copy=True)
            width, height = self.width(), self.height()
            self._spectrum = [QPointF(float(v[0]) * width, float(v[1]) * height) for v in vertices]
            self.update()

    def showEvent(self, event: object) -> None:  # noqa: N802 - Qt's name
        self._timer.start()

    def hideEvent(self, event: object) -> None:  # noqa: N802 - Qt's name
        self._timer.stop()

    def _handle_at(self, x: float, y: float) -> int | None:
        for index, (_name, hz, gain) in enumerate(self.bands()):
            hx = engine_bridge.eq_axis_position(hz) * self.width()
            hy = self._y(gain) * self.height()
            if abs(hx - x) < 8 and abs(hy - y) < 8:
                return index
        return None

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor(DARK.raised))
        painter.setPen(QColor(DARK.border))
        painter.drawLine(0, self.height() // 2, self.width(), self.height() // 2)
        for points, color, pen in (
            (self._spectrum, DARK.text_dim, 1),
            (self._curve, DARK.roles["eq_curve"], 2),
        ):
            if len(points) > 1:
                path = QPainterPath(points[0])
                for point in points[1:]:
                    path.lineTo(point)
                painter.setPen(QPen(QColor(color), pen))
                painter.drawPath(path)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(DARK.accent))
        for _name, hz, gain in self.bands():
            painter.drawEllipse(
                QPointF(
                    engine_bridge.eq_axis_position(hz) * self.width(), self._y(gain) * self.height()
                ),
                5,
                5,
            )
        painter.setPen(QColor(DARK.text_dim))
        painter.drawText(QRectF(4, 2, 200, 14), Qt.AlignmentFlag.AlignLeft, f"±{RANGE_DB:.0f} dB")
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self._grab = self._handle_at(event.position().x(), event.position().y())

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._grab is None:
            return
        bands = self.bands()
        if self._grab >= len(bands):
            return
        name, _hz, _gain = bands[self._grab]
        gain = (0.5 - event.position().y() / max(1, self.height())) * 2 * RANGE_DB
        gain = max(-RANGE_DB, min(RANGE_DB, gain))
        if self.effect == "EQ":
            self.target.turn(name, gain)
            return
        frequency = engine_bridge.eq_axis_frequency(event.position().x() / max(1, self.width()))
        self.target.turn(f"{name}.freq", frequency)
        self.target.turn(f"{name}.gain", gain)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self._grab = None
