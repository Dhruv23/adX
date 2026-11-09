"""The Hardware panel: the M-Vave FM-1 (phase_11.5.md 4.6-4.11). Status - connected,
in update mode, in ROM recovery, or not found - with its firmware, profile and what
the probes found; then the librarian, parts, effects and firmware tabs.
"""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QComboBox,
    QFormLayout,
    QLabel,
    QScrollArea,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from adx.hardware.fm1 import profile
from adx.hardware.fm1.manager import Fm1Manager
from adx.hardware.fm1.recovery import DeviceState, guide
from adx.panels.base import DockPanel
from adx.panels.hardware.firmware_tab import FirmwareTab
from adx.panels.hardware.fx_tab import FxTab
from adx.panels.hardware.librarian_tab import LibrarianTab
from adx.panels.hardware.parts_tab import PartsTab


def _yes_no(value: bool | None) -> str:
    return "not probed" if value is None else ("yes" if value else "no")


class HardwarePanel(DockPanel):
    panel_id = "hardware"
    title = "Hardware"
    # Tabbed with Generate and MIDI in the left column (MainWindow._place_panels).
    default_area = Qt.DockWidgetArea.LeftDockWidgetArea

    def __init__(
        self,
        manager: Fm1Manager,
        parent: QWidget | None = None,
        match: Callable[[dict[str, int]], bool] | None = None,
        set_clock: Callable[[bool], None] | None = None,
        set_pads: Callable[[bool], None] | None = None,
        audition: Callable[[str], None] | None = None,
    ) -> None:
        super().__init__(parent)
        self.manager = manager
        body = QWidget()
        layout = QVBoxLayout(body)
        form = QFormLayout()
        self.state = QLabel()
        self.state.setObjectName("fm1.state")
        form.addRow("M-Vave FM-1", self.state)
        self.model = QLabel()
        self.model.setObjectName("fm1.model")
        form.addRow("Firmware", self.model)
        self.profile_box = QComboBox()
        self.profile_box.setObjectName("fm1.profile")
        self.profiles = profile.shipped()
        self.profile_box.addItems([p.name for p in self.profiles])
        self.profile_box.activated.connect(self._profile_chosen)
        form.addRow("Profile", self.profile_box)
        self.probes = QLabel()
        self.probes.setObjectName("fm1.probes")
        self.probes.setWordWrap(True)
        form.addRow("Probed", self.probes)
        layout.addLayout(form)
        self.guide = QLabel()
        self.guide.setObjectName("fm1.guide")
        self.guide.setWordWrap(True)
        layout.addWidget(self.guide)
        self.tabs = QTabWidget()
        self.librarian = LibrarianTab(manager, audition)
        self.parts = PartsTab(manager, set_clock, set_pads)
        self.fx = FxTab(manager, match)
        self.firmware = FirmwareTab(manager)
        self.tabs.addTab(self.librarian, "Librarian")
        self.tabs.addTab(self.parts, "Parts")
        self.tabs.addTab(self.fx, "Effects")
        self.tabs.addTab(self.firmware, "Firmware")
        layout.addWidget(self.tabs, 1)
        # In a scroll area, so the panel never pins its splitter to its content's height.
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(body)
        self.setWidget(scroll)
        manager.changed.connect(self.refresh)
        self.refresh()

    def refresh(self) -> None:
        m = self.manager
        where = f" on {m.port_in}" if m.port_in else ""
        self.state.setText(f"{m.state.value}{where}")
        if m.identity is not None:
            self.model.setText(m.identity.model)
        elif m.port_in:
            self.model.setText("no identity reply (assumed V15; choose the profile below)")
        else:
            self.model.setText("—")
        if m.profile is not None and m.profile in self.profiles:
            self.profile_box.setCurrentIndex(self.profiles.index(m.profile))
        elif m.profile is not None:
            names = [p.name for p in self.profiles]
            if m.profile.name in names:
                self.profile_box.setCurrentIndex(names.index(m.profile.name))
        p = m.probe
        self.probes.setText(
            f"answers dump requests: {_yes_no(p.answers_dump)}; sends velocity: "
            f"{_yes_no(p.sends_velocity)}; USB playback: {_yes_no(p.usb_playback)}"
        )
        self.guide.setText(guide(m.state) if m.state is not DeviceState.NORMAL else "")

    def _profile_chosen(self, index: int) -> None:
        if 0 <= index < len(self.profiles):
            self.manager.use_profile(self.profiles[index])
