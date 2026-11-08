"""A hosted plugin's own editor, in a window of ours (phase_9.md 4.6).

The plugin's GUI lives in its sandbox process: the child makes a native window, the
plugin draws into it, and this window embeds it by handle (``QWindow.fromWinId``). A
crash takes the editor with the child, never this window.

Some plugins embed badly, and some have no editor at all. The generic editor - every
parameter from the plugin's own list - is always there, below, so such a plugin stays
usable rather than not. A plugin that embeds badly can be told never to embed again
(remembered per plugin); the bar above says how it is doing - late blocks, a crash -
and offers a restart and a latency check (STATE.md P9-8).
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from PySide6.QtCore import QSettings, Qt, QTimer
from PySide6.QtGui import QCloseEvent, QWindow
from PySide6.QtWidgets import QCheckBox, QHBoxLayout, QLabel, QPushButton, QVBoxLayout, QWidget

from adx.engine_bridge import Plugins

#: Where the plugins that must not embed are remembered: the preferences group, so the
#: preferences dataclass reads the same list (Preferences.plugins_never_embed).
NEVER_EMBED_KEY = "preferences/plugins_never_embed"


def never_embed(uid: str) -> bool:
    """True when ``uid``'s editor is not to be embedded."""
    return bool(uid) and uid in _never_embed_list()


def set_never_embed(uid: str, on: bool) -> None:
    """Remember (or forget) that ``uid``'s editor is not to be embedded."""
    if not uid:
        return
    names = [n for n in _never_embed_list() if n != uid]
    if on:
        names.append(uid)
    QSettings().setValue(NEVER_EMBED_KEY, names)


def _never_embed_list() -> list[str]:
    stored = QSettings().value(NEVER_EMBED_KEY)
    if isinstance(stored, list):
        return [str(v) for v in stored]
    return [str(stored)] if stored else []


def instance_health(instance: int) -> dict[str, Any] | None:
    """The sandbox's row for ``instance`` (state, late blocks, latency), or None."""
    if not instance:
        return None
    for row in Plugins.instances():
        if int(row["id"]) == instance:
            return row
    return None


def health_text(row: dict[str, Any] | None) -> str:
    """'running · 0 late blocks · 64 samples latency', or why there is none."""
    if row is None:
        return "not loaded"
    late = int(row["late"])
    blocks = "1 late block" if late == 1 else f"{late} late blocks"
    return f"{row['state']} · {blocks} · {int(row['latency'])} samples latency"


class PluginWindow(QWidget):
    """One instance's editor: embedded if it has one, the generic editor regardless."""

    def __init__(
        self,
        instance: int,
        title: str,
        generic: Callable[[], QWidget] | None = None,
        embed: bool = True,
        parent: QWidget | None = None,
        uid: str = "",
    ) -> None:
        super().__init__(parent, Qt.WindowType.Window)
        self.instance = instance
        self.uid = uid
        self.setWindowTitle(title)
        self.setObjectName(f"plugin-window:{instance}")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        bar = QHBoxLayout()
        bar.setContentsMargins(6, 4, 6, 4)
        self.health = QLabel()
        self.health.setObjectName("plugin-health")
        bar.addWidget(self.health, 1)
        self.no_embed = QCheckBox("Don't embed its editor")
        self.no_embed.setObjectName("plugin-no-embed")
        self.no_embed.setToolTip(
            "For a plugin whose editor draws badly here: from the next time this window opens, "
            "only the generic editor is shown"
        )
        self.no_embed.setChecked(never_embed(uid))
        self.no_embed.toggled.connect(lambda on: set_never_embed(self.uid, on))
        self.no_embed.setEnabled(bool(uid))
        bar.addWidget(self.no_embed)
        self.latency_button = QPushButton("Check latency")
        self.latency_button.setObjectName("plugin-check-latency")
        self.latency_button.setToolTip(
            "Measure the plugin's delay with an impulse and compare it with what it reports "
            "(stop playback first)"
        )
        self.latency_button.clicked.connect(self.check_latency)
        bar.addWidget(self.latency_button)
        self.restart_button = QPushButton("Restart")
        self.restart_button.setObjectName("plugin-restart")
        self.restart_button.setToolTip("Bring the plugin back with its last state")
        self.restart_button.clicked.connect(self.restart)
        bar.addWidget(self.restart_button)
        layout.addLayout(bar)

        self.native: QWindow | None = None
        self.container: QWidget | None = None
        wanted = embed and not never_embed(uid)
        opened = Plugins.open_editor(instance) if wanted and instance else None
        if opened is not None:
            handle, width, height = opened
            self.native = QWindow.fromWinId(handle)
            self.container = QWidget.createWindowContainer(self.native, self)
            self.container.setMinimumSize(max(width, 100), max(height, 60))
            layout.addWidget(self.container, 1)
        else:
            text = (
                "Embedding is off for this plugin; its parameters are below."
                if wanted is False and never_embed(uid)
                else "This plugin has no editor of its own here; its parameters are below."
            )
            note = QLabel(text)
            note.setWordWrap(True)
            note.setMargin(8)
            layout.addWidget(note)
        self.generic = generic() if generic is not None else None
        if self.generic is not None:
            layout.addWidget(self.generic, 0 if self.container is not None else 1)
        if self.container is not None:
            self.resize(self.container.minimumWidth(), self.container.minimumHeight() + 260)
        else:
            self.resize(640, 440)

        self._poll = QTimer(self)
        self._poll.setInterval(500)
        self._poll.timeout.connect(self.refresh_health)
        self._poll.start()
        self.refresh_health()

    @property
    def embedded(self) -> bool:
        """The plugin's own editor is showing (not only the generic one)."""
        return self.container is not None

    def refresh_health(self) -> None:
        """The bar's readout, and Restart enabled only when there is something to restart."""
        row = instance_health(self.instance)
        self.health.setText(health_text(row))
        state = str(row["state"]) if row is not None else ""
        self.restart_button.setEnabled(state in ("crashed", "hung", "late-bypassed", "muted"))

    def check_latency(self) -> int:
        """Measure; a mismatch with the plugin's report also arrives as a notice."""
        if not self.instance:
            return -1
        measured = Plugins.check_latency(self.instance)
        row = instance_health(self.instance)
        reported = int(row["latency"]) if row is not None else 0
        if measured < 0:
            self.health.setText("latency check needs playback stopped")
        elif measured == reported:
            self.health.setText(f"latency {measured} samples, as it reports")
        else:
            self.health.setText(f"latency measured {measured} samples, it reports {reported}")
        return measured

    def restart(self) -> int:
        """Restart this instance's sandbox child."""
        restarted = Plugins.restart(self.instance) if self.instance else 0
        self.refresh_health()
        return restarted

    def closeEvent(self, event: QCloseEvent) -> None:  # noqa: N802 - Qt's name
        self._poll.stop()
        if self.instance and self.embedded:
            Plugins.close_editor(self.instance)
        super().closeEvent(event)
