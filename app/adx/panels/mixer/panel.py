"""The mixer (phase_6.md 4.3): a strip per insert, scrolling sideways, with every strip's
meter drawn as ONE vertex buffer.

The meters are the reason this panel is shaped as it is. The frame clock's single engine
call per frame (Engine.frame_with_meters) reads every strip's levels and builds every
meter's vertices in C++; this panel then uploads that one buffer - so 200 inserts cost
one FFI call and one upload a frame, not 200 (``mixer_meters_one_ffi_call``).
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import QPoint, Qt, Signal
from PySide6.QtWidgets import QHBoxLayout, QScrollArea, QSplitter, QToolBar, QVBoxLayout, QWidget

from adx import dnd, engine_bridge
from adx.engine_bridge import FrameClock, MeterGeometry
from adx.panels.base import DockPanel
from adx.panels.geometry_view import WORLD_TRIANGLES, GeometryView, Pointer
from adx.panels.mixer.routing_matrix import RoutingMatrix
from adx.panels.mixer.send_editor import send_menu
from adx.panels.mixer.strip import STRIP_WIDTH, MixerStrip, effect_menu
from adx.selection import Context, Kind, Selection
from adx.session import Session
from adx.theme.tokens import Theme

METER_HEIGHT = 96


class _MeterInput:
    """The meter row takes no input."""

    def on_press(self, pointer: Pointer) -> None: ...
    def on_move(self, pointer: Pointer) -> None: ...
    def on_release(self, pointer: Pointer) -> None: ...
    def on_double_click(self, pointer: Pointer) -> None: ...
    def on_wheel(self, x: float, y: float, dx: float, dy: float, modifiers: int) -> None: ...
    def on_resize(self, width: float, height: float) -> None: ...


class MixerPanel(DockPanel):
    """Every insert's strip, the routing matrix, and the meters."""

    panel_id = "mixer"
    title = "Mixer"
    default_area = Qt.DockWidgetArea.BottomDockWidgetArea

    #: (insert, slot): open that effect's editor.
    open_effect = Signal(int, int)

    def __init__(
        self, session: Session, selection: Selection, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.selection = selection
        self.meters = MeterGeometry()
        self.strips: list[MixerStrip] = []
        self._effects = [
            str(t["name"]) for t in engine_bridge.type_catalog() if t["kind"] == "effect"
        ]
        body = QWidget()
        outer = QVBoxLayout(body)
        outer.setContentsMargins(0, 0, 0, 0)
        bar = QToolBar()
        bar.addAction("+ Insert").triggered.connect(self.add_insert)
        routing = bar.addAction("Routing")
        routing.setCheckable(True)
        bar.addAction("Reset clips").triggered.connect(self.meters.reset_clips)
        outer.addWidget(bar)
        split = QSplitter(Qt.Orientation.Horizontal)
        self.scroller = QScrollArea()
        self.scroller.setWidgetResizable(True)
        content = QWidget()
        column = QVBoxLayout(content)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(2)
        self.meter_view = GeometryView(_MeterInput(), (WORLD_TRIANGLES,))
        self.meter_view.setFixedHeight(METER_HEIGHT)
        column.addWidget(self.meter_view, 0, Qt.AlignmentFlag.AlignLeft)
        self.strip_row = QHBoxLayout()
        self.strip_row.setSpacing(0)
        self.strip_row.setContentsMargins(0, 0, 0, 0)
        self.strip_row.addStretch(1)  # strips are inserted before it
        column.addLayout(self.strip_row, 1)
        self.scroller.setWidget(content)
        split.addWidget(self.scroller)
        matrix_scroll = QScrollArea()
        self.matrix = RoutingMatrix()
        self.matrix.toggled.connect(self.toggle_route)
        matrix_scroll.setWidget(self.matrix)
        matrix_scroll.setVisible(False)
        routing.toggled.connect(matrix_scroll.setVisible)
        split.addWidget(matrix_scroll)
        outer.addWidget(split, 1)
        self.setWidget(body)
        session.changed.connect(self.refresh)

    # --- reading --------------------------------------------------------------------

    def refresh(self) -> None:
        """Re-read the mixer and show it."""
        project = self.session.project
        if project is None:
            return
        info = project.mixer_info()
        inserts = sorted(info["inserts"], key=lambda i: int(i["id"]))
        names = {int(i["id"]): str(i["name"]) or str(i["id"]) for i in inserts}
        routes = [(int(r[1]), int(r[2])) for r in info["routes"]]
        self._sync_strips([int(i["id"]) for i in inserts])
        for strip, insert in zip(self.strips, inserts, strict=True):
            own = int(insert["id"])
            outputs = [(i, n) for i, n in names.items() if i != own]
            routed = next((to for frm, to in routes if frm == own), 0)
            strip.show_insert(insert, outputs, routed, names)
        self.meter_view.setFixedWidth(max(1, STRIP_WIDTH * len(self.strips)))
        self.meter_view.set_view(0.0, 0.0, float(STRIP_WIDTH), float(METER_HEIGHT))
        ids, states = project.routing_matrix()
        self.matrix.show_matrix(ids, states)

    def _sync_strips(self, ids: list[int]) -> None:
        if [s.insert for s in self.strips] == ids:
            return
        # Incremental, as the channel rack's rows: only new inserts build a strip.
        existing = {strip.insert: strip for strip in self.strips}
        for insert, strip in existing.items():
            if insert not in ids:
                strip.setParent(None)
                strip.deleteLater()
        self.strips = []
        for insert in ids:
            strip = existing.get(insert) or MixerStrip(insert)
            if insert not in existing:
                self._wire(strip)
            self.strips.append(strip)
        for index, strip in enumerate(self.strips):
            self.strip_row.insertWidget(index, strip)

    def _wire(self, strip: MixerStrip) -> None:
        s = self.session
        strip.field_turned.connect(lambda i, f, v: s.set_param(f"insert.{i}.{f}", v))
        strip.field_set.connect(lambda i, f, v: s.build_and_execute("SetInsertField", i, f, v))
        strip.renamed.connect(lambda i, n: s.build_and_execute("RenameInsert", i, n))
        strip.output_chosen.connect(self.set_output)
        strip.slot_clicked.connect(self._slot_clicked)
        strip.slot_bypassed.connect(lambda slot, on: s.build_and_execute("SetSlotBypass", slot, on))
        strip.slot_mix.connect(
            lambda slot, v, insert=strip.insert: s.set_param(f"insert.{insert}.slot.{slot}.mix", v)
        )
        strip.slot_moved.connect(
            lambda _i, slot, index: s.build_and_execute("MoveSlot", slot, index)
        )
        strip.slot_menu.connect(self._slot_menu)
        strip.slot_dropped.connect(
            lambda insert, _index, slot, payload: dnd.perform(
                s, payload, dnd.SlotTarget(insert, slot)
            )
        )
        strip.send_turned.connect(
            lambda send, v, insert=strip.insert: s.set_param(
                f"insert.{insert}.send.{send}.level", v
            )
        )
        strip.send_removed.connect(lambda send: s.build_and_execute("RemoveSend", send))
        strip.add_send.connect(self._add_send)
        strip.selected.connect(
            lambda insert: self.selection.select(Kind.INSERTS, [insert], Context())
        )

    # --- edits ----------------------------------------------------------------------

    def add_insert(self) -> int:
        """A new insert routed to the master: one step. Returns its id."""
        project = self.session.require()
        with self.session.group("Add insert"):
            count = len(project.mixer_info()["inserts"])
            self.session.build_and_execute("AddInsertNew", f"Insert {count}")
            info = project.mixer_info()
            new = max(int(i["id"]) for i in info["inserts"])
            self.session.build_and_execute("AddRoute", new, int(info["master"]))
        return new

    def set_output(self, insert: int, target: int) -> None:
        """Route ``insert`` to ``target`` alone (0: nowhere): one step."""
        project = self.session.require()
        with self.session.group("Set output"):
            for route_id, frm, _to in project.mixer_info()["routes"]:
                if int(frm) == insert:
                    self.session.build_and_execute("RemoveRoute", int(route_id))
            if target:
                self.session.build_and_execute("AddRoute", insert, target)

    def toggle_route(self, source: int, target: int) -> None:
        """Add the route, or remove it if it exists (the matrix's click)."""
        project = self.session.require()
        existing = [
            int(r[0])
            for r in project.mixer_info()["routes"]
            if int(r[1]) == source and int(r[2]) == target
        ]
        if existing:
            self.session.build_and_execute("RemoveRoute", existing[0])
        else:
            self.session.build_and_execute("AddRoute", source, target)

    def _slot_clicked(self, insert: int, _index: int, slot: int) -> None:
        if slot:
            self.selection.select(Kind.SLOTS, [slot], Context(insert=insert))
            self.open_effect.emit(insert, slot)
            return
        menu = effect_menu(self._effects, self)
        chosen = menu.exec(self.mapToGlobal(QPoint(0, 0)))
        if chosen is not None:
            self.session.build_and_execute("AddSlot", insert, str(chosen.data()))

    def _slot_menu(self, insert: int, _index: int, slot: int, at: QPoint) -> None:
        if not slot:
            return
        menu = effect_menu(self._effects, self)
        if menu.actions():
            menu.insertSeparator(menu.actions()[0])
        remove = menu.addAction("Remove")
        duplicate = menu.addAction("Duplicate")
        chosen = menu.exec(at)
        project = self.session.require()
        if chosen is remove:
            self.session.build_and_execute("RemoveSlot", slot)
        elif chosen is duplicate:
            self.session.call(project.duplicate_slot, slot)
        elif chosen is not None and chosen.data():
            self.session.call(project.set_slot_effect, slot, str(chosen.data()))

    def _add_send(self, insert: int, at: QPoint) -> None:
        menu = send_menu(self.session.require(), insert, self)
        chosen = menu.exec(at)
        if chosen is not None and chosen.data():
            self.session.build_and_execute("AddSend", insert, int(chosen.data()), 0.5, False)

    # --- the frame ------------------------------------------------------------------

    def attach(self, clock: FrameClock) -> None:
        """Have the frame clock build this panel's meters in its one call per frame."""
        clock.meters = self.meters

    def frame(self, clock: FrameClock) -> None:
        """Upload the meters the frame clock just built: a Qt call, no engine call."""
        address, count, revision = clock.meter_upload
        if count:
            self.meter_view.upload_raw(0, address, count, revision)

    def apply_theme(self, theme: Theme) -> None:
        """Recolour."""
        self.meter_view.apply_theme(theme)
        self.matrix.set_theme(theme)
        for strip in self.strips:
            strip.fader.set_theme(theme)

    def selected_inserts(self) -> list[Any]:
        """The inserts selected in the global selection."""
        return list(self.selection.ids(Kind.INSERTS))
