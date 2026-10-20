"""The mixer (phase_6.md 4.3, 5)."""

from __future__ import annotations

from PySide6.QtCore import QPoint, Qt
from PySide6.QtTest import QTest

from adx import engine_bridge
from adx.mainwindow import MainWindow
from adx.panels.mixer.routing_matrix import CELL, HEADER


def add_inserts(studio: MainWindow, count: int) -> list[int]:
    """``count`` inserts routed to the master, in one group (setup, not the test)."""
    with studio.session.group("setup"):
        for _ in range(count):
            studio.mixer.add_insert()
    return sorted(int(i["id"]) for i in studio.project.mixer_info()["inserts"])


def test_mixer_meters_one_ffi_call(studio: MainWindow) -> None:
    add_inserts(studio, 199)
    assert len(studio.mixer.strips) == 200
    clock = studio.clock
    assert clock.meters is studio.mixer.meters
    studio.engine.play()
    clock.tick()  # the first frame may grow the level buffer
    with engine_bridge.CALLS.measure() as calls:
        for _ in range(60):
            clock.tick()  # emits frame -> every panel draws, the mixer uploads
    assert calls[0] == 60
    studio.engine.stop_playback()
    # Every strip's meter is in the one buffer the frame built.
    _address, count, _revision = clock.meter_upload
    assert count >= 200 * 6


def test_mixer_slot_reorder(studio: MainWindow) -> None:
    session = studio.session
    master = int(studio.project.mixer_info()["master"])
    for effect in ("EQ", "Delay", "Reverb"):
        session.build_and_execute("AddSlot", master, effect)

    def chain() -> list[tuple[int, str]]:
        insert = studio.project.mixer_info()["inserts"][0]
        return [(int(s["id"]), str(s["type"])) for s in insert["slots"]]

    before = chain()
    assert [e for _i, e in before] == ["EQ", "Delay", "Reverb"]
    depth = studio.project.undo_depth()
    # The strip's slot row reports the drop of slot 1 (EQ) at index 2.
    strip = studio.mixer.strips[0]
    strip.slots[0].moved.emit(before[0][0], 2)
    after = chain()
    assert [e for _i, e in after] == ["Delay", "Reverb", "EQ"]
    # One command; ids kept, so the engine keeps each effect's state (the C++
    # mixer_slot_reorder_keeps_nodes asserts the nodes survive).
    assert studio.project.undo_depth() == depth + 1
    assert sorted(after) == sorted(before)
    session.undo()
    assert chain() == before


def test_mixer_routing_prevents_cycle(studio: MainWindow) -> None:
    ids = add_inserts(studio, 2)
    master, a, b = ids
    studio.mixer.toggle_route(a, b)  # a -> b (and a -> master from add_insert)
    studio.mixer.refresh()
    matrix = studio.mixer.matrix
    matrix.show()
    row, column = matrix.ids.index(b), matrix.ids.index(a)
    # b -> a would close a cycle: the cell is disabled before any command exists.
    assert not matrix.enabled(row, column)
    depth = studio.project.undo_depth()
    issued = list(matrix.issued)
    centre = QPoint(HEADER + column * CELL + CELL // 2, HEADER + row * CELL + CELL // 2)
    QTest.mouseClick(matrix, Qt.MouseButton.LeftButton, pos=centre)
    assert matrix.issued == issued
    assert studio.project.undo_depth() == depth
    # A legal cell does issue exactly one command.
    row, column = matrix.ids.index(b), matrix.ids.index(master)
    assert matrix.enabled(row, column)
    centre = QPoint(HEADER + column * CELL + CELL // 2, HEADER + row * CELL + CELL // 2)
    QTest.mouseClick(matrix, Qt.MouseButton.LeftButton, pos=centre)
    assert studio.project.undo_depth() == depth + 1
    routes = {(int(r[1]), int(r[2])) for r in studio.project.mixer_info()["routes"]}
    assert (b, a) not in routes
