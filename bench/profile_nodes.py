"""Where the benchmark's audio time goes, per node type (phase_10.md 4.5).

    py -3.12 bench/profile_nodes.py [ENGINE_DIR]

One offline render of the one-minute cut with every graph step timed
(adx_engine.profile_render); prints each instrument, effect and the graph's plumbing as
a share of one core.
"""

from __future__ import annotations

import sys
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

import adx_engine  # noqa: E402
from adx.engine_bridge import Project  # noqa: E402

folder = ROOT / "build" / "bench" / "nodes"
folder.mkdir(parents=True, exist_ok=True)
project, _ = Project.load(str(make_project.build(folder, bars=30)))
seconds = float(project.info()["length_seconds"])
rows, wall = adx_engine.profile_render(project.native)
rows = sorted(rows, key=lambda r: -r[1])
timed = sum(r[1] for r in rows) / 1e9
print(f"wall {wall / seconds * 100:.1f} % of one core; timed nodes {timed / seconds * 100:.1f} %")
for name, ns, _blocks in rows:
    print(f"{name:24} {ns / 1e9 / seconds * 100:6.2f} %")
