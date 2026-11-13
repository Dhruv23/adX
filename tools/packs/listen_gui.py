"""A small listening-notes window for the phase 12 demo set.

    python tools/packs/listen_gui.py [FOLDER]

FOLDER defaults to build/phase12-listening. Every .wav under it is listed; pick one,
press Play (nothing autoplays), and type notes. Notes, a verdict per sound and the
overall verdict are written to FOLDER/listening_notes.json on every keystroke
(atomically, so a crash never leaves a half-written file). Ctrl+T puts the playback
time in the notes as [m:ss].

Float32 WAVs are converted to 16-bit in a temp folder first; the originals are never
touched.
"""

from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
from datetime import datetime
from pathlib import Path

import numpy as np
from PySide6.QtCore import Qt, QTimer, QUrl
from PySide6.QtGui import QKeySequence, QShortcut
from PySide6.QtMultimedia import QAudioOutput, QMediaPlayer
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QPlainTextEdit,
    QPushButton,
    QSlider,
    QVBoxLayout,
    QWidget,
)

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from wavio import read_any  # noqa: E402

DEFAULT = REPO / "build" / "phase12-listening"
VERDICTS = ["", "good", "ok, minor issues", "needs work", "broken"]


def playable_copy(path: Path, cache: Path) -> Path:
    """A 16-bit copy QtMultimedia always plays, normalised never: just clipped."""
    target = cache / (path.stem + ".wav")
    if target.exists() and target.stat().st_mtime >= path.stat().st_mtime:
        return target
    audio, rate = read_any(path)
    pcm = (np.clip(audio, -1.0, 1.0) * 32767.0).astype("<i2")
    channels = pcm.shape[1]
    raw = pcm.tobytes()
    block = channels * 2
    header = b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * block, block, 16)
    header += b"data" + struct.pack("<I", len(raw))
    target.write_bytes(header + raw)
    return target


def clock(ms: int) -> str:
    seconds = max(0, ms) // 1000
    return f"{seconds // 60}:{seconds % 60:02d}"


class Listener(QMainWindow):
    def __init__(self, folder: Path) -> None:
        super().__init__()
        self.folder = folder
        self.store = folder / "listening_notes.json"
        self.cache = Path(tempfile.mkdtemp(prefix="adx_listen_"))
        self.files = sorted(folder.rglob("*.wav"))
        self.state: dict[str, object] = {"overall": "", "overall_verdict": "", "sounds": {}}
        self.current: Path | None = None
        self.loading = False
        self.status = QLabel("")
        self._load()

        self.setWindowTitle(f"adX listening notes - {folder.name}")
        self.resize(1000, 640)

        self.list = QListWidget()
        for path in self.files:
            item = QListWidgetItem(path.stem)
            item.setData(Qt.ItemDataRole.UserRole, str(path))
            self.list.addItem(item)
        self.list.setMaximumWidth(340)
        self.list.currentRowChanged.connect(self._select)

        self.audio = QAudioOutput()
        self.player = QMediaPlayer()
        self.player.setAudioOutput(self.audio)
        self.player.positionChanged.connect(self._position)
        self.player.durationChanged.connect(lambda d: self.seek.setRange(0, d))
        self.player.playbackStateChanged.connect(self._state)

        self.play = QPushButton("Play")
        self.play.clicked.connect(self._toggle)
        self.seek = QSlider(Qt.Orientation.Horizontal)
        self.seek.sliderMoved.connect(self.player.setPosition)
        self.time = QLabel("0:00 / 0:00")
        volume = QSlider(Qt.Orientation.Horizontal)
        volume.setRange(0, 100)
        volume.setValue(80)
        volume.setMaximumWidth(110)
        volume.valueChanged.connect(lambda v: self.audio.setVolume(v / 100.0))
        self.audio.setVolume(0.8)

        transport = QHBoxLayout()
        transport.addWidget(self.play)
        transport.addWidget(self.seek, 1)
        transport.addWidget(self.time)
        transport.addWidget(QLabel("vol"))
        transport.addWidget(volume)

        self.title = QLabel("")
        self.title.setStyleSheet("font-size: 16px; font-weight: 600;")
        self.verdict = QComboBox()
        self.verdict.addItems(VERDICTS)
        self.verdict.currentTextChanged.connect(self._changed)
        stamp = QPushButton("Insert time  (Ctrl+T)")
        stamp.clicked.connect(self._stamp)
        head = QHBoxLayout()
        head.addWidget(self.title, 1)
        head.addWidget(QLabel("verdict"))
        head.addWidget(self.verdict)
        head.addWidget(stamp)

        self.notes = QPlainTextEdit()
        self.notes.setPlaceholderText("Notes for this sound. Saved as you type.")
        self.notes.textChanged.connect(self._changed)

        self.overall_verdict = QComboBox()
        self.overall_verdict.addItems(["", "ready for pack building", "not ready"])
        self.overall_verdict.currentTextChanged.connect(self._changed)
        self.overall = QPlainTextEdit()
        self.overall.setPlaceholderText(
            "Overall: is the set ready for pack building? In your own words."
        )
        self.overall.setMaximumHeight(110)
        self.overall.textChanged.connect(self._changed)
        foot = QHBoxLayout()
        foot.addWidget(QLabel("Overall"))
        foot.addWidget(self.overall_verdict)
        foot.addStretch(1)
        foot.addWidget(self.status)

        right = QVBoxLayout()
        right.addLayout(head)
        right.addLayout(transport)
        right.addWidget(self.notes, 1)
        right.addLayout(foot)
        right.addWidget(self.overall)

        root = QHBoxLayout()
        root.addWidget(self.list)
        side = QWidget()
        side.setLayout(right)
        root.addWidget(side, 1)
        holder = QWidget()
        holder.setLayout(root)
        self.setCentralWidget(holder)

        QShortcut(QKeySequence("Ctrl+T"), self, activated=self._stamp)
        QShortcut(QKeySequence("Space"), self.list, activated=self._toggle)
        QShortcut(QKeySequence("Ctrl+Down"), self, activated=lambda: self._step(1))
        QShortcut(QKeySequence("Ctrl+Up"), self, activated=lambda: self._step(-1))

        self.loading = True
        self.overall.setPlainText(str(self.state.get("overall", "")))
        self.overall_verdict.setCurrentText(str(self.state.get("overall_verdict", "")))
        self.loading = False
        if self.files:
            self.list.setCurrentRow(0)

    # --- storage -----------------------------------------------------------------------

    def _load(self) -> None:
        try:
            loaded = json.loads(self.store.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                self.state.update(loaded)
        except (OSError, ValueError):
            pass

    def _key(self, path: Path) -> str:
        return path.relative_to(self.folder).as_posix()

    def _save(self) -> None:
        if self.loading:
            return
        if self.current is not None:
            sounds = self.state["sounds"]
            assert isinstance(sounds, dict)
            sounds[self._key(self.current)] = {
                "notes": self.notes.toPlainText(),
                "verdict": self.verdict.currentText(),
                "updated": datetime.now().isoformat(timespec="seconds"),
            }
        self.state["overall"] = self.overall.toPlainText()
        self.state["overall_verdict"] = self.overall_verdict.currentText()
        self.state["saved"] = datetime.now().isoformat(timespec="seconds")
        temp = self.store.with_suffix(".json.tmp")
        try:
            temp.write_text(json.dumps(self.state, indent=2, ensure_ascii=False), encoding="utf-8")
            os.replace(temp, self.store)
            self.status.setText(f"saved {datetime.now():%H:%M:%S} -> {self.store.name}")
        except OSError as error:
            self.status.setText(f"SAVE FAILED: {error}")

    def _changed(self, *_: object) -> None:
        self._save()

    # --- playback ----------------------------------------------------------------------

    def _select(self, row: int) -> None:
        if row < 0:
            return
        self._save()  # the note on screen belongs to the sound being left
        path = Path(self.list.item(row).data(Qt.ItemDataRole.UserRole))
        self.player.stop()
        self.loading = True
        self.current = path
        self.title.setText(path.stem)
        saved = self.state["sounds"]
        assert isinstance(saved, dict)
        entry = saved.get(self._key(path), {})
        self.notes.setPlainText(str(entry.get("notes", "")))
        self.verdict.setCurrentText(str(entry.get("verdict", "")))
        self.loading = False
        try:
            self.player.setSource(QUrl.fromLocalFile(str(playable_copy(path, self.cache))))
        except (OSError, ValueError) as error:
            self.status.setText(str(error))
            return
        # Loaded and waiting: Play (or Space) starts it.

    def _toggle(self) -> None:
        if self.player.playbackState() == QMediaPlayer.PlaybackState.PlayingState:
            self.player.pause()
        else:
            if self.player.position() >= self.player.duration() > 0:
                self.player.setPosition(0)
            self.player.play()

    def _state(self, state: QMediaPlayer.PlaybackState) -> None:
        playing = state == QMediaPlayer.PlaybackState.PlayingState
        self.play.setText("Pause" if playing else "Play")

    def _position(self, ms: int) -> None:
        if not self.seek.isSliderDown():
            self.seek.setValue(ms)
        self.time.setText(f"{clock(ms)} / {clock(self.player.duration())}")

    def _stamp(self) -> None:
        self.notes.insertPlainText(f"[{clock(self.player.position())}] ")
        self.notes.setFocus()

    def _step(self, delta: int) -> None:
        row = min(max(self.list.currentRow() + delta, 0), self.list.count() - 1)
        self.list.setCurrentRow(row)

    def closeEvent(self, event) -> None:  # noqa: N802, ANN001
        self.player.stop()
        self._save()
        super().closeEvent(event)


def main() -> int:
    folder = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    if not folder.is_dir():
        print(f"listen_gui: no folder {folder}", file=sys.stderr)
        return 1
    app = QApplication(sys.argv)
    window = Listener(folder)
    if not window.files:
        print(f"listen_gui: no .wav files under {folder}", file=sys.stderr)
        return 1
    window.show()
    QTimer.singleShot(0, window.list.setFocus)
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
