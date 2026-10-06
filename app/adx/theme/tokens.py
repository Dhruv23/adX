"""Colour and metric tokens: defined once, used everywhere (phase_5.md 4.10).

Three consumers read these and nothing else defines a colour:

- the widget stylesheet (:func:`stylesheet`), for the Qt Widgets chrome;
- QML, through :func:`qml_palette` set as the ``adxTheme`` context property;
- the scene-graph items, through :func:`role_colors` - one colour per
  ``engine/geometry/ColorPalette.h`` role, resolved at upload. Switching theme
  recolours the piano roll without rebuilding a single vertex.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from PySide6.QtGui import QColor

#: engine/geometry/ColorPalette.h's roles, index for index. Copied rather than
#: imported so this module stays free of the engine; test_bridge_contract.py checks the
#: two lists agree.
ROLE_NAMES: tuple[str, ...] = (
    "background",
    "row_white",
    "row_black",
    "row_in_scale",
    "row_root",
    "grid_sub",
    "grid_beat",
    "grid_bar",
    "note",
    "note_selected",
    "note_muted",
    "ghost",
    "density",
    "lane",
    "lane_selected",
    "curve",
    "playhead",
    "wave_fill",
    "wave_pending",
    "selection_rect",
)


@dataclass(frozen=True, slots=True)
class Metrics:
    """Sizes shared by widgets and QML."""

    spacing: int = 6
    radius: int = 4
    toolbar_icon: int = 18
    font_pt: float = 9.5
    keyboard_width: int = 56
    ruler_height: int = 22
    lane_height: int = 84


@dataclass(frozen=True, slots=True)
class Theme:
    """A complete theme: chrome colours, role colours and metrics."""

    name: str
    window: str
    panel: str
    raised: str
    border: str
    text: str
    text_dim: str
    accent: str
    danger: str
    roles: dict[str, str]
    metrics: Metrics = field(default_factory=Metrics)


DARK = Theme(
    name="dark",
    window="#1b1d21",
    panel="#23262b",
    raised="#2d3137",
    border="#3a3f47",
    text="#e3e6ea",
    text_dim="#8e959f",
    accent="#5fb2f0",
    danger="#ff5a50",
    roles={
        "background": "#1e2024",
        "row_white": "#2c2f35",
        "row_black": "#24262b",
        "row_in_scale": "#323842",
        "row_root": "#3e4656",
        "grid_sub": "#383b42",
        "grid_beat": "#484c55",
        "grid_bar": "#696e7a",
        "note": "#60b2f0",
        "note_selected": "#ffc45c",
        "note_muted": "#606874",
        "ghost": "#8c96aa46",
        "density": "#60b2f0c8",
        "lane": "#60b2f0",
        "lane_selected": "#ffc45c",
        "curve": "#ff78a0",
        "playhead": "#ff5a50",
        "wave_fill": "#78c896",
        "wave_pending": "#78808c",
        "selection_rect": "#ffc45c3c",
    },
)

LIGHT = Theme(
    name="light",
    window="#eef0f3",
    panel="#f7f8fa",
    raised="#ffffff",
    border="#c9ced6",
    text="#1d2026",
    text_dim="#5d6470",
    accent="#1f78c8",
    danger="#d23c32",
    roles={
        "background": "#f4f5f7",
        "row_white": "#fbfbfc",
        "row_black": "#e7e9ed",
        "row_in_scale": "#dfe8f4",
        "row_root": "#c9daf0",
        "grid_sub": "#e2e4e8",
        "grid_beat": "#cdd1d8",
        "grid_bar": "#9aa1ac",
        "note": "#1f78c8",
        "note_selected": "#e08a00",
        "note_muted": "#a8afb9",
        "ghost": "#5a647850",
        "density": "#1f78c8c8",
        "lane": "#1f78c8",
        "lane_selected": "#e08a00",
        "curve": "#d0306a",
        "playhead": "#d23c32",
        "wave_fill": "#2f9a5a",
        "wave_pending": "#9aa1ac",
        "selection_rect": "#e08a003c",
    },
)

THEMES: dict[str, Theme] = {DARK.name: DARK, LIGHT.name: LIGHT}


def color(text: str) -> QColor:
    """A QColor from ``#rrggbb`` or ``#rrggbbaa`` (CSS order, not Qt's ``#aarrggbb``)."""
    if len(text) == 9:
        result = QColor(text[:7])
        result.setAlpha(int(text[7:9], 16))
        return result
    return QColor(text)


def role_colors(theme: Theme) -> list[QColor]:
    """One colour per scene-graph role, in role-index order."""
    return [color(theme.roles[name]) for name in ROLE_NAMES]


def qml_palette(theme: Theme) -> dict[str, str]:
    """The tokens QML reads, as the ``adxTheme`` context property."""
    return {
        "window": theme.window,
        "panel": theme.panel,
        "raised": theme.raised,
        "border": theme.border,
        "text": theme.text,
        "textDim": theme.text_dim,
        "accent": theme.accent,
        "danger": theme.danger,
        "noteSelected": theme.roles["note_selected"][:7],
    }


def stylesheet(theme: Theme) -> str:
    """The application stylesheet for the Widgets chrome."""
    m = theme.metrics
    t = theme
    box = f"background: {t.raised}; border: 1px solid {t.border}; border-radius: {m.radius}px"
    rules = [
        ("QWidget", f"background: {t.window}; color: {t.text}; font-size: {m.font_pt}pt"),
        ("QMainWindow::separator", f"background: {t.border}; width: 3px; height: 3px"),
        (
            "QDockWidget::title",
            f"background: {t.panel}; padding: 4px 8px; border-bottom: 1px solid {t.border}",
        ),
        ("QToolBar", f"background: {t.panel}; border: none; spacing: {m.spacing}px; padding: 3px"),
        (
            "QToolButton",
            "background: transparent; border: 1px solid transparent; "
            f"border-radius: {m.radius}px; padding: 3px 6px",
        ),
        ("QToolButton:hover", f"background: {t.raised}; border-color: {t.border}"),
        (
            "QToolButton:checked",
            f"background: {t.raised}; border-color: {t.accent}; color: {t.accent}",
        ),
        ("QToolButton:disabled", f"color: {t.text_dim}"),
        ("QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit", f"{box}; padding: 2px 6px"),
        ("QComboBox:hover, QSpinBox:hover, QDoubleSpinBox:hover", f"border-color: {t.accent}"),
        ("QMenuBar, QMenu", f"background: {t.panel}"),
        (
            "QMenu::item:selected, QMenuBar::item:selected",
            f"background: {t.raised}; color: {t.accent}",
        ),
        ("QStatusBar", f"background: {t.panel}; color: {t.text_dim}"),
        ("QTreeView", f"background: {t.panel}; border: none"),
        ("QTreeView::item:selected", f"background: {t.raised}; color: {t.accent}"),
        (
            "QHeaderView::section",
            f"background: {t.panel}; border: none; padding: 3px; color: {t.text_dim}",
        ),
        ("QPushButton", f"{box}; padding: 4px 12px"),
        ("QPushButton:hover", f"border-color: {t.accent}"),
        (
            "QLabel#position",
            f"font-family: Consolas, monospace; font-size: {m.font_pt + 2}pt; color: {t.accent}",
        ),
    ]
    return "\n".join(f"{selector} {{ {body}; }}" for selector, body in rules)
