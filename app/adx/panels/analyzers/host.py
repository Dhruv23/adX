"""The analyzer hub for every analyzer panel (phase_10.md 4.4).

One :class:`~adx.engine_bridge.Analyzers` on the engine, made when the first analyzer
panel shows and handed to the frame clock, whose one call a frame then builds every open
analyzer. A panel that hides switches its analyzer off; the last one to hide takes the
hub off the clock, so a closed analyzer costs nothing.
"""

from __future__ import annotations

from PySide6.QtCore import QObject
from PySide6.QtWidgets import QWidget

from adx.engine_bridge import Analyzers, Engine
from adx.frame_clock import FrameClock


def on_screen(widget: QWidget) -> bool:
    """Visible and not covered: a dock tabbed behind another is isVisible() with an
    empty visible region."""
    return widget.isVisible() and not widget.visibleRegion().isEmpty()


#: The settings key each analyzer is switched by.
KINDS: tuple[str, ...] = ("spectrum", "spectrogram", "scope", "vector", "loudness", "particles")


class AnalyzerHost(QObject):
    def __init__(
        self, engine: Engine, clock: FrameClock | None = None, parent: QObject | None = None
    ) -> None:
        super().__init__(parent)
        self.engine = engine
        self.clock = clock
        self.analyzers: Analyzers | None = None
        self.open: set[str] = set()

    def attach(self, clock: FrameClock) -> None:
        self.clock = clock
        self._update_clock()

    def set_open(self, kind: str, shown: bool, **settings: object) -> None:
        """Switch one analyzer on or off, with any settings it changes."""
        if shown:
            self.open.add(kind)
        else:
            self.open.discard(kind)
        if self.analyzers is None:
            if not self.open:
                return
            self.analyzers = Analyzers(self.engine)
        self.analyzers.configure(**{kind: shown}, **settings)
        self._update_clock()

    def configure(self, **settings: object) -> None:
        """Change settings without opening or closing anything."""
        if self.analyzers is None:
            self.analyzers = Analyzers(self.engine)
        self.analyzers.configure(**settings)

    def _update_clock(self) -> None:
        if self.clock is not None:
            self.clock.analyzers = self.analyzers if self.open else None
            if not self.open:
                self.clock.analyzer_frame = None
