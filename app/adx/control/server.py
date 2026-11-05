"""The control channel (phase_10.5.md 4.2): a way into the running app for an agent.

Off unless asked for - ``adx gui --control`` or ``ADX_CONTROL=1`` (which is how a launch
through adX.cmd asks) - and then a ``QLocalServer`` on a pipe named for this process and a
random suffix, reachable by the current user only, behind a token. The pipe's name and the
token go to the control file (``--control-file``, or ``ADX_CONTROL_FILE``), which the
launcher reads back. There is no network socket.

Requests run on the main thread from the event loop, so they see what the user sees and
never touch the audio thread. A request that opens a modal dialog runs inside that
dialog's event loop, where the channel stays live: ``answer`` presses one of its buttons.
Such a request is sent with ``"async": true``: it is acknowledged at once and performed on
the next turn of the event loop, so the reply does not wait for the dialog (or the popup
menu, which ``menu`` then chooses from) to close. A failure of an async request shows in
the next ``state`` as ``control.errors``.
"""

from __future__ import annotations

import json
import os
import secrets
import time
import traceback
from collections.abc import Callable
from pathlib import Path
from typing import TYPE_CHECKING, Any

from PySide6.QtCore import QObject, QTimer
from PySide6.QtNetwork import QLocalServer, QLocalSocket
from PySide6.QtWidgets import QAbstractButton, QApplication, QDialog, QWidget

from adx import __version__
from adx.control import inputs, inspect, probes
from adx.control.protocol import (
    BAD_REQUEST,
    BAD_TOKEN,
    FAILED,
    OPS,
    PROTOCOL_VERSION,
    TIMEOUT,
    UNKNOWN_OP,
    ControlError,
    Json,
    decode,
    encode,
)

if TYPE_CHECKING:
    from adx.mainwindow import MainWindow


def requested() -> bool:
    """Whether the environment asks for a control channel."""
    return os.environ.get("ADX_CONTROL", "") not in ("", "0")


class _Connection(QObject):
    def __init__(self, socket: QLocalSocket, server: ControlServer) -> None:
        super().__init__(server)
        self.socket = socket
        self.server = server
        self.buffer = b""
        self.authenticated = False
        socket.readyRead.connect(self._read)
        socket.disconnected.connect(self._gone)

    def _gone(self) -> None:
        self.server.connections.discard(self)
        try:
            self.socket.deleteLater()
            self.deleteLater()
        except RuntimeError:
            pass  # already deleted with the app

    def send(self, message: dict[str, Any]) -> None:
        if self.socket.state() == QLocalSocket.LocalSocketState.ConnectedState:
            self.socket.write(encode(message))
            self.socket.flush()

    def _read(self) -> None:
        self.buffer += bytes(self.socket.readAll().data())
        while b"\n" in self.buffer:
            line, self.buffer = self.buffer.split(b"\n", 1)
            if line.strip():
                self._handle(line)

    def _handle(self, line: bytes) -> None:
        try:
            message = decode(line)
        except ValueError as error:
            self.send({"ok": False, "error": f"not JSON: {error}", "code": BAD_REQUEST})
            return
        if not self.authenticated:
            if (
                message.get("hello") != self.server.token
                or message.get("protocol") != PROTOCOL_VERSION
            ):
                self.send({"ok": False, "error": "wrong token or protocol", "code": BAD_TOKEN})
                self.socket.disconnectFromServer()
                return
            self.authenticated = True
            self.send(
                {
                    "ok": True,
                    "protocol": PROTOCOL_VERSION,
                    "version": __version__,
                    "pid": os.getpid(),
                }
            )
            return
        self.server.dispatch(self, message)


class ControlServer(QObject):
    def __init__(self, window: MainWindow, control_file: str | None = None) -> None:
        super().__init__(window)
        self.window = window
        self.token = secrets.token_hex(16)
        self.name = f"adx-control-{os.getpid()}-{secrets.token_hex(4)}"
        self.server = QLocalServer(self)
        self.server.setSocketOptions(QLocalServer.SocketOption.UserAccessOption)
        self.connections: set[_Connection] = set()
        self.server.newConnection.connect(self._accept)
        if not self.server.listen(self.name):
            raise RuntimeError(
                f"control channel: cannot listen on {self.name}: {self.server.errorString()}"
            )
        self.control_file = control_file or os.environ.get("ADX_CONTROL_FILE") or None
        if self.control_file:
            Path(self.control_file).write_text(
                json.dumps(
                    {
                        "pipe": self.server.fullServerName(),
                        "name": self.name,
                        "token": self.token,
                        "pid": os.getpid(),
                        "protocol": PROTOCOL_VERSION,
                    }
                ),
                encoding="utf-8",
            )
        inspect.ensure_names(window)
        self.handlers: dict[str, Callable[[dict[str, Any]], Any]] = {
            "ops": lambda _m: list(OPS),
            "tree": self._tree,
            "find": lambda m: inspect.find(
                m.get("name", ""), m.get("class", ""), m.get("text", "")
            ),
            "click": self._click,
            "double_click": lambda m: self._click({**m, "double": True}),
            "drag": self._drag,
            "drop": self._drop,
            "type": lambda m: inputs.type_text(
                self._target(m) if m.get("target") else self._focus(), str(m["text"])
            ),
            "key": lambda m: inputs.key(
                self._target(m) if m.get("target") else None, str(m["keys"])
            ),
            "scroll": lambda m: inputs.scroll(
                self._target(m),
                float(m.get("dy", -120)),
                m.get("x"),
                m.get("y"),
                m.get("modifiers"),
            ),
            "choose": lambda m: inputs.choose(
                self._target(m), None if m.get("text") is None else str(m["text"]), m.get("index")
            ),
            "trigger": self._trigger,
            "actions": self._actions,
            "state": lambda _m: probes.state(self.window),
            "screenshot": self._screenshot,
            "script": self._script,
            "open": self._open,
            "capture_start": lambda _m: self.window.engine.capture_start(),
            "capture_stop": self._capture_stop,
            "answer": self._answer,
            "menu": lambda m: inputs.choose_menu(str(m["text"])),
            "context_menu": lambda m: inputs.context_menu(self._target(m), m.get("x"), m.get("y")),
            "quit": self._quit,
        }
        #: Failures of async requests, oldest first (probes.state shows them).
        self.async_errors: list[str] = []

    def close(self) -> None:
        for connection in list(self.connections):
            connection.socket.disconnected.disconnect(connection._gone)
        self.connections.clear()
        self.server.close()
        if self.control_file:
            Path(self.control_file).unlink(missing_ok=True)

    def _accept(self) -> None:
        while self.server.hasPendingConnections():
            socket = self.server.nextPendingConnection()
            self.connections.add(_Connection(socket, self))

    def dispatch(self, connection: _Connection, message: dict[str, Any]) -> None:
        request = message.get("id")
        op = str(message.get("op", ""))
        if op == "wait_for":
            self._wait_for(connection, request, message)
            return
        handler = self.handlers.get(op)
        if handler is None:
            connection.send(
                {"id": request, "ok": False, "error": f"unknown op {op!r}", "code": UNKNOWN_OP}
            )
            return
        if message.get("async"):
            connection.send({"id": request, "ok": True, "result": {"queued": True}})

            def later() -> None:
                try:
                    inspect.ensure_names(self.window)
                    handler(message)
                except Exception as error:
                    self.async_errors.append(f"{op}: {type(error).__name__}: {error}")

            QTimer.singleShot(0, later)
            return
        try:
            inspect.ensure_names(self.window)
            result = handler(message)
            connection.send({"id": request, "ok": True, "result": result})
        except ControlError as error:
            connection.send({"id": request, "ok": False, "error": str(error), "code": error.code})
        except Exception as error:
            connection.send(
                {
                    "id": request,
                    "ok": False,
                    "error": f"{type(error).__name__}: {error}",
                    "code": FAILED,
                    "trace": traceback.format_exc(limit=6),
                }
            )

    # --- requests ----------------------------------------------------------------------

    def _target(self, message: dict[str, Any]) -> QWidget:
        target = message.get("target")
        if not target:
            raise ControlError("this request needs a target", BAD_REQUEST)
        return inspect.resolve(str(target))

    def _focus(self) -> QWidget:
        """Where typing goes with no target: the focused widget (a dialog's field)."""
        widget = inputs.focused()
        if widget is None:
            raise ControlError("nothing has the keyboard focus", BAD_REQUEST)
        return widget

    def _tree(self, message: dict[str, Any]) -> list[dict[str, Any]]:
        root = inspect.resolve(str(message["root"])) if message.get("root") else None
        return inspect.tree(root, bool(message.get("interactive", True)))

    def _click(self, message: dict[str, Any]) -> dict[str, Any]:
        widget = self._target(message)
        if not widget.isEnabled():
            raise ControlError(f"{widget.objectName()} is disabled", FAILED)
        x, y = message.get("x"), message.get("y")
        if message.get("item") is not None:
            at = inputs.item_point(widget, message["item"])
            x, y = at.x(), at.y()
        inputs.click(
            widget,
            x,
            y,
            str(message.get("button", "left")),
            message.get("modifiers"),
            bool(message.get("double", False)),
        )
        return inspect.describe(widget) if not _deleted(widget) else {}

    def _drag(self, message: dict[str, Any]) -> None:
        widget = self._target(message)
        start, end = message["from"], message["to"]
        inputs.drag(
            widget,
            (float(start[0]), float(start[1])),
            (float(end[0]), float(end[1])),
            str(message.get("button", "left")),
            message.get("modifiers"),
            int(message.get("steps", 8)),
        )

    def _drop(self, message: dict[str, Any]) -> bool:
        source = inspect.resolve(str(message["source"]))
        mime = inputs.mime_from(source, message.get("row"), message.get("item"))
        accepted = inputs.drop(mime, self._target(message), message.get("x"), message.get("y"))
        if not accepted:
            raise ControlError(f"{message.get('target')} refused the drop", FAILED)
        return True

    def _trigger(self, message: dict[str, Any]) -> bool:
        action = str(message["action"])
        if action not in {spec.id for spec in self.window.registry.specs()}:
            raise ControlError(f"no action {action!r}", BAD_REQUEST)
        ok = bool(self.window.registry.trigger(action))
        QApplication.processEvents()
        return ok

    def _actions(self, _message: dict[str, Any]) -> list[dict[str, Any]]:
        out = []
        for spec in self.window.registry.specs():
            action = self.window.registry.action(spec.id)
            out.append(
                {
                    "id": spec.id,
                    "text": action.text().replace("&", ""),
                    "menu": spec.menu.replace("&", ""),
                    "enabled": action.isEnabled(),
                    "shortcut": action.shortcut().toString(),
                }
            )
        return out

    def _screenshot(self, message: dict[str, Any]) -> dict[str, Any]:
        widget = self._target(message) if message.get("target") else self.window
        if message.get("screen"):
            # What is on screen, native child windows (a plugin's editor) included, which
            # grab() cannot see.
            window = widget.window()
            window.raise_()
            QApplication.processEvents()
            origin = widget.mapTo(window, widget.rect().topLeft())
            image = (
                window.screen()
                .grabWindow(window.winId(), origin.x(), origin.y(), widget.width(), widget.height())
                .toImage()
            )
        elif hasattr(widget, "grabFramebuffer"):
            image = widget.grabFramebuffer()
        else:
            image = widget.grab().toImage()
        path = str(message["path"])
        image.save(path)
        # One colour everywhere is "drew nothing" (phase_10.5.md 4.6, the plugin editor).
        small = image.scaled(32, 32)
        colours = {small.pixel(x, y) for x in range(small.width()) for y in range(small.height())}
        return {
            "path": path,
            "width": image.width(),
            "height": image.height(),
            "colours": len(colours),
        }

    def _script(self, message: dict[str, Any]) -> Json:
        """Setup and readback with no GUI path, in the scripting API (token-gated, as every
        request is)."""
        import adx.scripting as adx_api

        namespace: dict[str, Any] = {"adx": adx_api, "window": self.window, "result": None}
        exec(str(message["code"]), namespace)
        result = namespace.get("result")
        try:
            json.dumps(result)
            return result
        except TypeError:
            return repr(result)

    def _open(self, message: dict[str, Any]) -> dict[str, Any]:
        """What File > Open does once its dialog has a path: the dialog itself is the
        operating system's and synthesized input cannot reach it."""
        if message.get("discard"):
            project = self.window.session.project
            if project is not None:
                self.window.saved_revision = project.revision()
        if not self.window.open_project(str(message["path"])):
            raise ControlError(f"could not open {message['path']}", FAILED)
        QApplication.processEvents()
        opened: dict[str, Any] = probes.state(self.window)["project"]
        return opened

    def _capture_stop(self, message: dict[str, Any]) -> dict[str, Any]:
        result = self.window.engine.capture_stop(message.get("path"))
        result.pop("samples", None)
        return result

    def _answer(self, message: dict[str, Any]) -> str:
        modal = QApplication.activeModalWidget()
        if not isinstance(modal, QDialog):
            raise ControlError("no dialog is open", FAILED)
        wanted = str(message["button"]).replace("&", "").lower()
        for button in modal.findChildren(QAbstractButton):
            if button.text().replace("&", "").lower() == wanted and button.isVisible():
                inputs.click(button)
                return button.text()
        raise ControlError(f"the dialog has no button {message['button']!r}", BAD_REQUEST)

    def _quit(self, message: dict[str, Any]) -> bool:
        """Close as the user would, the save prompt answered by the request: ``save`` true
        saves (to ``path`` if given), false discards."""
        window = self.window
        project = window.session.project
        if project is not None:
            if message.get("save"):
                if message.get("path"):
                    window.path = str(message["path"])
                window.save()
            else:
                window.saved_revision = project.revision()
        QTimer.singleShot(0, window.close)
        return True

    def _wait_for(self, connection: _Connection, request: Json, message: dict[str, Any]) -> None:
        path = str(message.get("path", ""))
        op = str(message.get("predicate", "truthy"))
        want = message.get("value")
        deadline = time.monotonic() + float(message.get("timeout", 10.0))
        timer = QTimer(self)
        timer.setInterval(int(message.get("interval_ms", 20)))

        def check() -> None:
            try:
                snapshot = probes.state(self.window)
                value = probes.lookup(snapshot, path)
                if probes.holds(value, op, want):
                    timer.stop()
                    timer.deleteLater()
                    connection.send({"id": request, "ok": True, "result": value})
                elif time.monotonic() > deadline:
                    timer.stop()
                    timer.deleteLater()
                    connection.send(
                        {
                            "id": request,
                            "ok": False,
                            "code": TIMEOUT,
                            "error": f"{path} {op} {want!r} not met; last value {value!r}",
                            "last": value,
                        }
                    )
            except Exception as error:
                timer.stop()
                timer.deleteLater()
                connection.send({"id": request, "ok": False, "code": FAILED, "error": str(error)})

        timer.timeout.connect(check)
        timer.start()
        check()


def _deleted(widget: QWidget) -> bool:
    try:
        widget.objectName()
        return False
    except RuntimeError:
        return True
