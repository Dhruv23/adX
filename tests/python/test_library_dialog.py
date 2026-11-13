"""File > Get sound libraries (phase_12.md 4.8): the dialog lists the store's libraries
and runs the fetch tool's verify without blocking the window."""

from __future__ import annotations

import pathlib

import pytest
from pytestqt.qtbot import QtBot

from adx.panels.library.dialog import LibraryDialog, libraries


@pytest.fixture
def empty_store(tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch) -> pathlib.Path:
    monkeypatch.setenv("ADX_LIBRARY_STORE", str(tmp_path / "store"))
    return tmp_path / "store"


def test_lists_every_library(qtbot: QtBot, empty_store: pathlib.Path) -> None:
    dialog = LibraryDialog()
    qtbot.addWidget(dialog)
    assert not dialog.isModal()
    ids = [str(lib["id"]) for lib in libraries()]
    assert {"vsco2ce", "vcsl", "salamander", "generaluser"} <= set(ids)
    assert dialog.table.rowCount() == len(ids)
    assert all(dialog.state_of(i) == "not fetched" for i in ids)
    # GeneralUser GS is fetched, never bundled: the row says so.
    licences = [dialog.table.item(r, 2).text() for r in range(dialog.table.rowCount())]
    assert any("fetched only" in text for text in licences)


def test_fake_fetched_library_verifies_and_reports(qtbot: QtBot, empty_store: pathlib.Path) -> None:
    """A library folder with nothing in it is "fetched" but fails its check: the tool
    runs in the background, its output reaches the log, and the exit code is kept."""
    lib = next(x for x in libraries() if x["id"] == "generaluser")
    pathlib.Path(str(lib["folder"])).mkdir(parents=True)
    dialog = LibraryDialog()
    qtbot.addWidget(dialog)
    dialog.show()
    assert dialog.state_of("generaluser") == "fetched"
    dialog.run("verify", "generaluser")
    assert dialog.process is not None  # running, and the window is not blocked
    qtbot.waitUntil(lambda: dialog.last_exit is not None, timeout=60000)
    assert dialog.last_exit == 1
    assert "generaluser: FAILED" in dialog.log.toPlainText()
    assert "failed" in dialog.status.text()
