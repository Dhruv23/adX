"""Audio preview: a file plays on click, through a channel of its own that bypasses the
mixer (phase_6.md 4.6) - Qt Multimedia's player straight to the default output, so a
preview never touches the project, its graph or its meters."""

from __future__ import annotations

from PySide6.QtCore import QObject, QUrl


class Preview(QObject):
    """Plays one file at a time; a second click stops it."""

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._player: object | None = None
        self._output: object | None = None
        self.current = ""

    def _ensure(self) -> object | None:
        if self._player is None:
            try:
                from PySide6.QtMultimedia import QAudioOutput, QMediaPlayer
            except ImportError:
                return None
            self._output = QAudioOutput(self)
            player = QMediaPlayer(self)
            player.setAudioOutput(self._output)
            self._player = player
        return self._player

    def play(self, path: str) -> bool:
        """Play ``path`` (stopping whatever was playing); False when there is no output."""
        player = self._ensure()
        if player is None:
            return False
        player.stop()  # type: ignore[attr-defined]
        if path == self.current:
            self.current = ""
            return True
        self.current = path
        player.setSource(QUrl.fromLocalFile(path))  # type: ignore[attr-defined]
        player.play()  # type: ignore[attr-defined]
        return True

    def stop(self) -> None:
        """Stop."""
        if self._player is not None:
            self._player.stop()  # type: ignore[attr-defined]
        self.current = ""
