"""The driver (phase_10.5.md 4.3): launch adX the way the user does and work it from a
test or a script, through the control channel.

    with Driver.launch(project=tmp / "song.adx", via="adX.cmd") as app:
        app.trigger("transport.play")
        app.wait_for("transport.playing")
        app.click("mixer.insert.1.mute")
        wav = app.capture(seconds=4, path=tmp / "take.wav")

Launch modes: through ``adX.cmd`` (what a double-click runs, so the launcher, its pathing
and its environment are covered) or ``python -m adx gui`` directly; with the real default
audio device or ``null_audio``. The driver needs no Qt: on Windows the channel is a named
pipe opened as a file, elsewhere a Unix socket.
"""

from __future__ import annotations

import contextlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable
from pathlib import Path
from types import TracebackType
from typing import Any

from adx.control.protocol import PROTOCOL_VERSION, TIMEOUT, Json

ROOT = Path(__file__).resolve().parents[3]
LAUNCHER = ROOT / "adX.cmd"


class DriverError(RuntimeError):
    """A request the app refused; ``code`` says why (adx/control/protocol.py)."""

    def __init__(
        self, message: str, code: str = "", response: dict[str, Any] | None = None
    ) -> None:
        super().__init__(message)
        self.code = code
        self.response = response or {}


class _Channel:
    """One connection: lines out, lines in, nothing else."""

    def __init__(self, address: str) -> None:
        self._buffer = b""
        if sys.platform == "win32":
            deadline = time.monotonic() + 10.0
            while True:
                try:
                    self._pipe = open(address, "r+b", buffering=0)  # noqa: SIM115 - closed in close()
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.05)
            self._socket: socket.socket | None = None
        else:
            self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._socket.connect(address)

    def send(self, message: dict[str, Any]) -> None:
        data = (json.dumps(message, separators=(",", ":")) + "\n").encode("utf-8")
        if self._socket is not None:
            self._socket.sendall(data)
        else:
            self._pipe.write(data)

    def receive(self) -> dict[str, Any]:
        while b"\n" not in self._buffer:
            chunk = self._socket.recv(65536) if self._socket is not None else self._pipe.read(65536)
            if not chunk:
                raise DriverError("the app closed the control channel", "closed")
            self._buffer += chunk
        line, self._buffer = self._buffer.split(b"\n", 1)
        value: dict[str, Any] = json.loads(line.decode("utf-8"))
        return value

    def close(self) -> None:
        try:
            if self._socket is not None:
                self._socket.close()
            else:
                self._pipe.close()
        except OSError:
            pass


def _alive(pid: int) -> bool:
    if sys.platform == "win32":
        import ctypes

        synchronize = 0x00100000
        handle = ctypes.windll.kernel32.OpenProcess(synchronize, False, pid)
        if not handle:
            return False
        try:
            waited = ctypes.windll.kernel32.WaitForSingleObject(handle, 0)
            return bool(waited == 0x102)  # WAIT_TIMEOUT
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def _kill(pid: int) -> None:
    if sys.platform == "win32":
        subprocess.run(["taskkill", "/PID", str(pid), "/F", "/T"], capture_output=True, check=False)
    else:
        with contextlib.suppress(OSError):
            os.kill(pid, 9)


class Driver:
    """A connected app. Every method is one request (see adx/control/protocol.py)."""

    def __init__(
        self, control_file: str | Path, process: subprocess.Popen[bytes] | None = None
    ) -> None:
        facts = json.loads(Path(control_file).read_text(encoding="utf-8"))
        self.pid = int(facts["pid"])
        self.control_file = Path(control_file)
        self.process = process
        self._next = 0
        self._early: dict[int, dict[str, Any]] = {}
        self._channel = _Channel(str(facts["pipe"]))
        self._channel.send({"hello": facts["token"], "protocol": PROTOCOL_VERSION})
        reply = self._channel.receive()
        if not reply.get("ok"):
            raise DriverError(
                f"handshake refused: {reply.get('error')}", str(reply.get("code", ""))
            )
        self.version = str(reply.get("version", ""))

    # --- launching -----------------------------------------------------------------------

    @classmethod
    def launch(
        cls,
        project: str | Path | None = None,
        *,
        via: str = "gui",
        null_audio: bool = True,
        env: dict[str, str] | None = None,
        timeout: float = 90.0,
        settings_dir: str | Path | None = None,
        detach: bool = False,
    ) -> Driver:
        """Start adX with its control channel and connect. ``via`` is "adX.cmd" (the
        launcher, as a double-click runs it) or "gui" (``python -m adx gui``). ``detach``
        lets the app outlive the caller and keeps it off the caller's stdout, which a
        shell waiting on ``adx drive launch`` would otherwise wait on until it quits."""
        control_file = Path(tempfile.mkdtemp(prefix="adx-drive-")) / "control.json"
        environment = dict(os.environ, **(env or {}))
        environment["ADX_CONTROL"] = "1"
        environment["ADX_CONTROL_FILE"] = str(control_file)
        environment.setdefault("ADX_NO_QUICKSTART", "1")
        # Its own settings, so a driven run never touches the user's layout or recent files.
        settings = Path(settings_dir or control_file.parent)
        settings.mkdir(parents=True, exist_ok=True)
        environment["ADX_SETTINGS_DIR"] = str(settings)
        arguments = (["--null-audio"] if null_audio else []) + ([str(project)] if project else [])
        process: subprocess.Popen[bytes] | None = None
        if via == "adX.cmd":
            # The launcher's `start` hands its handles to the app, so a pipe here would stay
            # open until the app quits; a file does not hold the launcher up.
            log = control_file.parent / "launcher.log"
            with log.open("wb") as out:
                started = subprocess.run(
                    ["cmd", "/c", str(LAUNCHER), *arguments],
                    env=environment,
                    cwd=ROOT,
                    stdin=subprocess.DEVNULL,
                    stdout=out,
                    stderr=subprocess.STDOUT,
                    check=False,
                )
            if started.returncode != 0:
                raise DriverError(f"adX.cmd failed: {log.read_text(errors='replace')}", "launch")
        elif via == "gui":
            quiet: dict[str, Any] = {}
            if detach:
                quiet = {
                    "stdin": subprocess.DEVNULL,
                    "stdout": subprocess.DEVNULL,
                    "stderr": subprocess.DEVNULL,
                }
                if sys.platform == "win32":
                    quiet["creationflags"] = (
                        subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
                    )
                else:
                    quiet["start_new_session"] = True
            process = subprocess.Popen(
                [sys.executable, "-m", "adx", "gui", *arguments], env=environment, cwd=ROOT, **quiet
            )
        else:
            raise ValueError(f"via is 'adX.cmd' or 'gui', not {via!r}")
        deadline = time.monotonic() + timeout
        while not control_file.exists():
            if process is not None and process.poll() is not None:
                raise DriverError(
                    f"adx gui exited with {process.returncode} before opening the channel", "launch"
                )
            if time.monotonic() > deadline:
                if process is not None:
                    process.kill()
                raise DriverError(f"no control channel within {timeout:.0f} s", "launch")
            time.sleep(0.1)
        time.sleep(0.05)  # written in one call; let it close
        return cls(control_file, process)

    def __enter__(self) -> Driver:
        return self

    def __exit__(
        self,
        kind: type[BaseException] | None,
        _value: BaseException | None,
        _trace: TracebackType | None,
    ) -> None:
        self.close(force=kind is not None)

    def close(self, force: bool = False, timeout: float = 30.0) -> None:
        """Quit the app (discarding unsaved work) and wait for the process to end; kill it
        if it will not."""
        if not force:
            try:
                self.quit(save=False)
            except (DriverError, OSError):
                force = True
        self._channel.close()
        deadline = time.monotonic() + timeout
        while _alive(self.pid) and time.monotonic() < deadline:
            time.sleep(0.1)
        if _alive(self.pid):
            _kill(self.pid)

    # --- requests ------------------------------------------------------------------------

    def request(self, op: str, **fields: Json) -> Json:
        self._next += 1
        request_id = self._next
        self._channel.send(
            {"id": request_id, "op": op, **{k: v for k, v in fields.items() if v is not None}}
        )
        while True:
            reply = self._early.pop(request_id, None) or self._channel.receive()
            if reply.get("id") != request_id:
                self._early[int(reply.get("id", -1))] = reply
                continue
            if not reply.get("ok"):
                raise DriverError(f"{op}: {reply.get('error')}", str(reply.get("code", "")), reply)
            return reply.get("result")

    def tree(self, root: str | None = None, interactive: bool = True) -> list[dict[str, Any]]:
        return list(self.request("tree", root=root, interactive=interactive))

    def find(self, name: str = "", cls: str = "", text: str = "") -> list[dict[str, Any]]:
        return list(self.request("find", name=name, **{"class": cls}, text=text))

    def click(
        self,
        target: str,
        x: float | None = None,
        y: float | None = None,
        button: str = "left",
        modifiers: list[str] | None = None,
        *,
        item: str | list[str] | None = None,
        wait: bool = True,
    ) -> dict[str, Any]:
        """``item`` clicks an item view's row by its texts; ``wait=False`` for a click that
        opens a menu or a dialog (the reply comes before it closes)."""
        return dict(
            self.request(
                "click",
                target=target,
                x=x,
                y=y,
                button=button,
                modifiers=modifiers,
                item=item,
                **{"async": True} if not wait else {},
            )
            or {}
        )

    def context_menu(self, target: str, x: float | None = None, y: float | None = None) -> None:
        """Right-click for a context menu; acknowledged before the menu closes."""
        self.request("context_menu", target=target, x=x, y=y, **{"async": True})

    def menu(self, text: str) -> str:
        """Choose an item of the open popup menu."""
        return str(self.request("menu", text=text))

    def double_click(
        self, target: str, x: float | None = None, y: float | None = None, *, wait: bool = True
    ) -> None:
        self.request("double_click", target=target, x=x, y=y, **{"async": True} if not wait else {})

    def drag(
        self,
        target: str,
        start: tuple[float, float],
        end: tuple[float, float],
        button: str = "left",
        modifiers: list[str] | None = None,
        steps: int = 8,
    ) -> None:
        self.request(
            "drag",
            target=target,
            **{"from": list(start), "to": list(end)},
            button=button,
            modifiers=modifiers,
            steps=steps,
        )

    def drop(
        self,
        source: str,
        target: str,
        *,
        item: str | None = None,
        row: int | None = None,
        x: float | None = None,
        y: float | None = None,
    ) -> None:
        self.request("drop", source=source, target=target, item=item, row=row, x=x, y=y)

    def type(self, target: str | None, text: str) -> None:
        """Type into ``target``, or with None into whatever has the focus."""
        self.request("type", target=target, text=text)

    def key(self, keys: str, target: str | None = None) -> None:
        self.request("key", keys=keys, target=target)

    def scroll(
        self,
        target: str,
        dy: float = -120,
        x: float | None = None,
        y: float | None = None,
        modifiers: list[str] | None = None,
    ) -> None:
        self.request("scroll", target=target, dy=dy, x=x, y=y, modifiers=modifiers)

    def choose(self, target: str, text: str | None = None, *, index: int | None = None) -> None:
        self.request("choose", target=target, text=text, index=index)

    def trigger(self, action: str, *, wait: bool = True) -> bool:
        """``wait=False`` for an action that opens a dialog."""
        return bool(self.request("trigger", action=action, **{"async": True} if not wait else {}))

    def actions(self) -> list[dict[str, Any]]:
        return list(self.request("actions"))

    def state(self) -> dict[str, Any]:
        return dict(self.request("state"))

    def wait_for(
        self,
        what: str | Callable[[dict[str, Any]], bool],
        predicate: str = "truthy",
        value: Json = None,
        timeout: float = 10.0,
    ) -> Json:
        """A probe path ("transport.playing") with a predicate (truthy, falsy, equals,
        not_equals, gt, gte, lte, contains), checked in the app; or a callable on the
        whole state, checked here. Fails at the timeout with the last state."""
        if callable(what):
            deadline = time.monotonic() + timeout
            last: dict[str, Any] = {}
            while time.monotonic() < deadline:
                last = self.state()
                if what(last):
                    return last
                time.sleep(0.05)
            raise DriverError(
                f"wait_for: the predicate did not hold within {timeout} s; last state {last}",
                TIMEOUT,
            )
        return self.request(
            "wait_for", path=what, predicate=predicate, value=value, timeout=timeout
        )

    def screenshot(
        self, path: str | Path, target: str | None = None, screen: bool = False
    ) -> dict[str, Any]:
        """``screen`` grabs what is on screen there, native child windows included."""
        return dict(
            self.request("screenshot", path=str(path), target=target, screen=screen or None)
        )

    def script(self, code: str) -> Json:
        return self.request("script", code=code)

    def open(self, path: str | Path, discard: bool = False) -> dict[str, Any]:
        return dict(self.request("open", path=str(path), discard=discard))

    def capture_start(self) -> None:
        self.request("capture_start")

    def capture_stop(self, path: str | Path | None = None) -> dict[str, Any]:
        return dict(self.request("capture_stop", path=str(path) if path else None))

    def capture(self, seconds: float, path: str | Path) -> dict[str, Any]:
        """Capture the master for ``seconds`` of wall time to a float WAV."""
        self.capture_start()
        time.sleep(seconds)
        return self.capture_stop(path)

    def answer(self, button: str) -> str:
        return str(self.request("answer", button=button))

    def quit(self, save: bool = False, path: str | Path | None = None) -> None:
        self.request("quit", save=save, path=str(path) if path else None)
