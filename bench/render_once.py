"""Render the benchmark's one-minute cut once, for a profiler to watch.

    py-spy record --native -o profile.svg -- py -3.12 bench/render_once.py [ENGINE_DIR]

ENGINE_DIR is a folder holding an adx_engine build to use instead of the installed one
(build/windows-x64-relwithdebinfo/bindings has one with symbols).
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if len(sys.argv) > 1:
    # By file: the installed package's import finder would otherwise claim the name.
    import importlib.util

    found = next(Path(sys.argv[1]).glob("adx_engine*.pyd"))
    spec = importlib.util.spec_from_file_location("adx_engine", found)
    assert spec is not None and spec.loader is not None
    sys.modules["adx_engine"] = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sys.modules["adx_engine"])
sys.path.insert(1, str(ROOT / "app"))
sys.path.insert(2, str(ROOT / "bench"))

import make_project  # noqa: E402

from adx.engine_bridge import Project, render_to_file  # noqa: E402

folder = ROOT / "build" / "bench" / "once"
folder.mkdir(parents=True, exist_ok=True)
project, _ = Project.load(str(make_project.build(folder, bars=30)))
started = time.perf_counter()
render_to_file(project, str(folder / "once.wav"), tail_seconds=0.0)
print(f"{(time.perf_counter() - started) / 60 * 100:.1f} % of one core")
