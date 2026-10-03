"""Load adx_engine from a CMake build tree, and the text edits the A/B relies on.

`import adx_engine` returns whichever build `pip install -e .` last produced, and the
editable install's finder outranks sys.path - so a check run after a C++ change but
before a reinstall silently measures the old engine. Loading the .pyd by path from the
build tree measures the code that was just built.
"""

from __future__ import annotations

import importlib.machinery
import importlib.util
import re
import sys
from pathlib import Path
from types import ModuleType

REPO_ROOT = Path(__file__).resolve().parents[2]
#: The archived engine as it was, and the same minus its two scheduling bugs - the
#: reference the Tranche A gate measures against (patch_v1.py).
V1_RENDER = Path(__file__).resolve().parent / "v1render.exe"
V1_FIXED = Path(__file__).resolve().parent / "v1fixed.exe"


def load(preset: str = "windows-x64-debug") -> ModuleType:
    """The adx_engine module built by CMake preset `preset`."""
    bindings = REPO_ROOT / "build" / preset / "bindings"
    candidates = sorted(bindings.glob("adx_engine*.pyd")) + sorted(bindings.glob("adx_engine*.so"))
    if not candidates:
        raise SystemExit(f"no adx_engine module in {bindings}; build preset {preset} first")
    path = candidates[0]
    loader = importlib.machinery.ExtensionFileLoader("adx_engine", str(path))
    spec = importlib.util.spec_from_file_location("adx_engine", path, loader=loader)
    if spec is None:
        raise SystemExit(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    sys.modules["adx_engine"] = module
    return module


def channel_names(text: str) -> list[str]:
    """Channel names of a v2 document, in file order."""
    return re.findall(r"^\[CHANNEL (\S+)\]", text, re.MULTILINE)


def solo(text: str, keep: str) -> str:
    """`text` with every channel but `keep` muted."""
    for name in channel_names(text):
        if name != keep:
            text = text.replace(f"[CHANNEL {name}]\n", f"[CHANNEL {name}]\nMUTE=yes\n")
    return text
