"""The bridge's Qt half: the UI's 60 Hz frame clock and the engine-error signal.

Kept out of :mod:`adx.engine_bridge` so the bridge imports without Qt: ``adx render``
runs headless in CI and must not load PySide6 (phase_8.md 3.6).
"""

from __future__ import annotations

import time

import numpy as np
import numpy.typing as npt
from PySide6.QtCore import QObject, Qt, QTimer, Signal

from adx.engine_bridge import LEVEL_COLUMNS, AnalyzerFrame, Analyzers, Engine, MeterGeometry

__all__ = ["ErrorChannel", "FrameClock", "errors"]


class FrameClock(QObject):
    """The 60 Hz UI frame: one :meth:`Engine.frame` call per tick (phase_5.md 4.8).

    Emits ``frame(position_ticks, rolling)`` and keeps the latest meter rows in
    :attr:`levels`, a buffer allocated once and refilled in place. With :attr:`meters`
    set (the mixer sets it), the same one call also builds every strip's meter vertices,
    and :attr:`meter_upload` holds (address, vertex count, revision) for this frame's
    upload - which must happen before any other engine call (phase_6.md 4.3). With
    :attr:`analyzers` set (an analyzer panel is open), the call is
    frame_with_analyzers instead, and :attr:`analyzer_frame` holds every analyzer's
    upload too (phase_10.md 4.4): six analyzers, still one call.
    """

    frame = Signal(int, bool)

    def __init__(
        self, engine: Engine, interval_ms: int = 16, parent: QObject | None = None
    ) -> None:
        super().__init__(parent)
        self._engine = engine
        self.levels: npt.NDArray[np.float32] = np.zeros((32, LEVEL_COLUMNS), dtype=np.float32)
        self.strips = 0
        self.position_ticks = 0
        self.position_seconds = 0.0
        self.rolling = False
        self.meters: MeterGeometry | None = None
        self.meter_upload: tuple[int, int, int] = (0, 0, 0)
        self.analyzers: Analyzers | None = None
        self.analyzer_frame: AnalyzerFrame | None = None
        self._last = time.monotonic()
        self._timer = QTimer(self)
        # Coarse timers on Windows land on the 15.6 ms system tick, so a 16 ms interval
        # alternates 15.6 and 31.2 ms and the clock runs near 56 Hz (bench_ui.py).
        self._timer.setTimerType(Qt.TimerType.PreciseTimer)
        self._timer.setInterval(interval_ms)
        self._timer.timeout.connect(self.tick)

    def start(self) -> None:
        """Start ticking."""
        self._timer.start()

    def stop(self) -> None:
        """Stop ticking."""
        self._timer.stop()

    def tick(self) -> None:
        """One frame: exactly one engine call."""
        now = time.monotonic()
        dt, self._last = now - self._last, now
        if self.analyzers is not None:
            result = self._engine.frame_with_analyzers(self.levels, self.meters, self.analyzers, dt)
            ticks, rolling, strips, samples = (
                result.ticks,
                result.rolling,
                result.strips,
                result.samples,
            )
            if result.meters is not None:
                self.meter_upload = result.meters
            self.analyzer_frame = result
        elif self.meters is not None:
            ticks, rolling, strips, samples, address, count, revision = (
                self._engine.frame_with_meters(self.levels, self.meters)
            )
            self.meter_upload = (address, count, revision)
        else:
            ticks, rolling, strips, samples = self._engine.frame(self.levels)
        if strips > self.levels.shape[0]:
            # Too small: grow once (no engine call); the next frame fills every strip.
            self.levels = np.zeros((strips * 2, LEVEL_COLUMNS), dtype=np.float32)
        self.strips = min(strips, self.levels.shape[0])
        self.position_ticks = ticks
        self.position_seconds = samples / self._engine.sample_rate
        self.rolling = rolling
        self.frame.emit(ticks, rolling)


class ErrorChannel(QObject):
    """Engine errors as a Qt signal, so a panel never has to catch them itself."""

    error = Signal(str, str)

    def report(self, context: str, error: BaseException) -> None:
        """Turn ``error`` into an ``error(context, message)`` signal."""
        self.error.emit(context, str(error))


errors = ErrorChannel()
