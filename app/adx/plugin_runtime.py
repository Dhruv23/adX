"""What runs beside the engine at UI rate for plugins and MIDI learn (phase_9.md).

Four small jobs, each one engine call per tick:

- values a learned MIDI control wrote go into the project, so the knob moves and the
  save has them (``Engine.apply_learned``);
- parameter changes made in a plugin's own editor go in the same way
  (``Engine.apply_plugin_echoes``);
- the sandbox's notices - a crash, a hang, a mute - reach the status bar, and a latency
  change rebuilds the graph so delay compensation follows (``Plugins.notices``);
- learn mode: armed on a parameter, the next control that moves is bound to it.

None of it is on the audio path. A crashing plugin is the sandbox's problem; this
only tells the user, and offers the restart.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from PySide6.QtCore import QObject, QTimer, Signal
from PySide6.QtWidgets import QPushButton, QWidget

from adx.engine_bridge import Engine, Plugins
from adx.session import Session

#: How often the runtime looks, in milliseconds.
TICK_MS = 50

NOTICE_TEXT = {
    "crashed": "{plugin} crashed: {message}",
    "hung": "{plugin} {message}",
    "restarted": "{plugin} {message}",
    "late": "{plugin} {message}",
    "nan": "{plugin} {message}",
    "latency": "{plugin}: {message}",
    "state": "{plugin}: {message}",
    "latency-mismatch": "{plugin} {message}",
}


def describe_notice(notice: dict[str, Any]) -> str:
    """One notice as the status bar says it."""
    template = NOTICE_TEXT.get(str(notice["kind"]), "{plugin}: {message}")
    return template.format(plugin=notice["plugin"] or "A plugin", message=notice["message"])


class PluginRuntime(QObject):
    """The per-tick pump, and MIDI learn's armed state."""

    #: A notice, as text (the status bar shows it).
    notice = Signal(str)
    #: A plugin crashed: the instance id, for an offer to restart it.
    crashed = Signal(int)
    #: Learn mode changed: the armed path, or "" when disarmed.
    armed = Signal(str)
    #: A binding was made: the path, and the control as text ("CC 74 on channel 1").
    learned = Signal(str, str)

    def __init__(
        self,
        engine: Engine,
        session: Session,
        report: Callable[[str], None] = lambda _message: None,
        parent: QObject | None = None,
        start: bool = True,
    ) -> None:
        super().__init__(parent)
        self.engine = engine
        self.session = session
        self.report = report
        #: The parameter path learn mode will bind, or "".
        self.armed_path = ""
        #: Every notice shown this session, oldest first, as {kind, instance, text}.
        self.history: list[dict[str, Any]] = []
        self.timer = QTimer(self)
        self.timer.setInterval(TICK_MS)
        self.timer.timeout.connect(self.tick)
        self.notice.connect(report)
        if start:
            self.timer.start()

    # --- learn mode -----------------------------------------------------------------

    def arm(self, path: str) -> None:
        """Bind the next control that moves to ``path``."""
        # Whatever moved before the user asked is not the answer.
        self.engine.midi_last_control()
        self.armed_path = path
        self.armed.emit(path)
        self.report(f"MIDI learn: move a control for {path} (Esc cancels)")

    def disarm(self) -> None:
        if self.armed_path:
            self.armed_path = ""
            self.armed.emit("")

    def _learn(self) -> None:
        control = self.engine.midi_last_control()
        if control is None or not self.armed_path or self.session.project is None:
            return
        if control["kind"] == "note" and not control["pressed"]:
            return  # a key's release is not a choice
        path = self.armed_path
        try:
            self.session.call(
                self.session.project.add_midi_binding,
                str(control["kind"]),
                int(control["number"]),
                path,
                int(control["channel"]),
                str(control["device"]),
            )
        finally:
            self.disarm()
        text = control_text(control)
        self.learned.emit(path, text)
        self.report(f"MIDI learn: {text} now moves {path}")

    # --- the tick -------------------------------------------------------------------

    def tick(self) -> None:
        project = self.session.project
        if self.armed_path:
            self._learn()
        if project is not None:
            moved = self.engine.apply_learned(project) + self.engine.apply_plugin_echoes(project)
            if moved:
                self.session.notify()
        self._notices()

    def _notices(self) -> None:
        rebuild = False
        for notice in Plugins.notices():
            text = describe_notice(notice)
            self.history.append(
                {"kind": str(notice["kind"]), "instance": int(notice["instance"]), "text": text}
            )
            self.notice.emit(text)
            if notice["kind"] == "crashed" and int(notice["instance"]):
                self.crashed.emit(int(notice["instance"]))
            if notice["kind"] in ("latency", "restarted"):
                rebuild = True
        if rebuild and self.session.project is not None:
            # One recompensation (phase_9.md 4.5): a new delay plan for the new latency.
            self.engine.refresh(self.session.project)

    # --- around saves and exports ---------------------------------------------------

    def sync_states(self) -> int:
        """Store every live plugin's state in the project: before a save or an export,
        so the file renders what the user hears."""
        project = self.session.project
        if project is None:
            return 0
        changed = self.engine.sync_plugin_states(project)
        if changed:
            self.session.notify()
        return changed

    def restart_crashed(self) -> int:
        """Bring crashed plugins back with their last state."""
        restarted = Plugins.restart(0)
        if restarted:
            self.report(f"Restarted {restarted} plugin process(es)")
        return restarted


class RestartButton(QPushButton):
    """The crash notice's restart (P9-8): shown in the status bar when a plugin crashes,
    restarting that instance with its last state; hidden again once it has."""

    def __init__(self, runtime: PluginRuntime, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("plugin-restart-notice")
        self.instance = 0
        self.hide()
        runtime.crashed.connect(self.offer)
        self.clicked.connect(self.restart)

    def offer(self, instance: int) -> None:
        name = next((str(r["name"]) for r in Plugins.instances() if int(r["id"]) == instance), "")
        self.instance = instance
        self.setText(f"Restart {name}" if name else "Restart the plugin")
        self.setToolTip("Bring the crashed plugin back with its last state")
        self.show()

    def restart(self) -> int:
        restarted = Plugins.restart(self.instance) if self.instance else 0
        self.instance = 0
        self.hide()
        return restarted


def control_text(control: dict[str, Any]) -> str:
    """'CC 74 on channel 1', 'note 36 on channel 10', 'pitch bend on channel 2'."""
    kind = str(control["kind"])
    channel = int(control["channel"])
    where = f" on channel {channel}" if channel else ""
    if kind == "cc":
        return f"CC {int(control['number'])}{where}"
    if kind == "note":
        return f"note {int(control['number'])}{where}"
    if kind == "bend":
        return f"pitch bend{where}"
    return f"channel pressure{where}"
