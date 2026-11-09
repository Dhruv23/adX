"""Write docs/keyboard.md from the action registry (phase_10.md 4.7).

    py -3.12 tools/gen_keyboard.py [--check]

The keymap is the registry's, so the page is generated rather than written: every
action with a default shortcut, by menu. ``--check`` exits 1 if the page is stale
(test_docs.py runs it).
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGE = ROOT / "docs" / "keyboard.md"


def render() -> str:
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    os.environ.setdefault("ADX_NO_QUICKSTART", "1")
    import tempfile

    os.environ.setdefault("ADX_SETTINGS_DIR", tempfile.mkdtemp())
    from PySide6.QtWidgets import QApplication

    from adx.mainwindow import MainWindow

    app = QApplication.instance() or QApplication([])
    from adx.engine_bridge import Engine, load_quick_plugin

    load_quick_plugin()

    window = MainWindow(Engine(null_backend=True))
    rows: dict[str, list[tuple[str, str]]] = {}
    for spec in window.registry.specs():
        action = window.registry.action(spec.id)
        keys = action.shortcut().toString()
        if keys:
            # A panel's tool keys have no menu: they go under the panel ("Piano roll tools").
            panel = spec.scope.replace("_", " ").capitalize()
            menu = spec.menu.replace("&", "") or f"{panel} tools"
            rows.setdefault(menu, []).append((keys, action.text().replace("&", "")))
    window.close()
    del app
    lines = [
        "# Keyboard",
        "",
        "The default keymap: every menu action that has a shortcut. Generated from the",
        "action registry by `tools/gen_keyboard.py`; a test fails when it falls behind.",
        "Every key can be changed in Preferences > Shortcuts. Inside a panel, Delete",
        "removes what is selected and Ctrl+A selects everything in view.",
        "",
    ]
    for menu, entries in rows.items():
        lines += [f"## {menu}", "", "| Keys | Action |", "|---|---|"]
        lines += [f"| `{keys}` | {text} |" for keys, text in entries]
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = render()
    if args.check:
        current = PAGE.read_text(encoding="utf-8") if PAGE.exists() else ""
        if current != text:
            print("docs/keyboard.md is stale: py -3.12 tools/gen_keyboard.py", file=sys.stderr)
            return 1
        return 0
    PAGE.write_text(text, encoding="utf-8", newline="\n")
    print(PAGE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
