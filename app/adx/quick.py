"""Calling adX's C++ scene-graph items from Python.

PianoRollItem and WaveformItem are C++ types PySide6 has no Python wrapper for, so
their invokable methods are reached through Qt's meta-object system: one
``QMetaObject.invokeMethod`` per call, synchronous on the GUI thread. That is a Qt call,
not an engine call - it never counts against Rule 2's FFI budget - and it happens once
per interaction (an upload after a rebuild, a view change after a pan), never per note.

Their signals are forwarded through QML ``Connections`` into a Python object's slots
(see the panels' view.qml), for the same reason.
"""

from __future__ import annotations

from PySide6.QtCore import Q_ARG, QGenericArgumentHolder, QMetaObject, QObject, Qt


class ULong(int):
    """An int that crosses as ``qulonglong`` (an address, a revision)."""


def _argument(value: object) -> QGenericArgumentHolder:
    if isinstance(value, bool):
        return Q_ARG(bool, value)
    if isinstance(value, ULong):
        return Q_ARG("qulonglong", int(value))
    if isinstance(value, int):
        return Q_ARG(int, value)
    if isinstance(value, float):
        return Q_ARG(float, value)
    if isinstance(value, list):
        return Q_ARG("QVariantList", value)
    raise TypeError(f"no Qt argument type for {type(value).__name__}")


def call(target: QObject, method: str, *args: object) -> None:
    """Invoke a ``Q_INVOKABLE`` that returns nothing, synchronously."""
    ok = QMetaObject.invokeMethod(
        target, method, Qt.ConnectionType.DirectConnection, *[_argument(a) for a in args]
    )
    if not ok:
        raise RuntimeError(f"{type(target).__name__}.{method} could not be invoked")
