"""Where the benchmark's audio-thread time goes, by elimination (phase_10.md 4.5).

    py -3.12 bench/profile_parts.py

Renders a one-minute cut of the benchmark project offline, then variants of it with one
kind of work removed (all effects, the hosted plugins, the automation, the audio clips,
every channel but one instrument type). The render runs the realtime engine's code, so
render time over audio time is the audio thread's load; the drop each variant shows is
what that part costs.
"""

from __future__ import annotations

import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))
sys.path.insert(0, str(ROOT / "bench"))

import make_project  # noqa: E402

from adx.engine_bridge import Project, render_to_file  # noqa: E402

BARS = 30  # one minute


def timed(text: str, folder: Path, name: str) -> float:
    path = folder / f"{name}.adx"
    path.write_text(text, encoding="utf-8")
    project, diagnostics = Project.load(str(path))
    errors = [d for d in diagnostics if d["severity"] == "error"]
    assert not errors, (name, errors[:2])
    seconds = float(project.info()["length_seconds"])
    started = time.perf_counter()
    render_to_file(project, str(folder / f"{name}.wav"), tail_seconds=0.0)
    return (time.perf_counter() - started) / seconds * 100.0


def without_slots(text: str, keep_plugins: bool) -> str:
    out = []
    for line in text.splitlines():
        if line.startswith("  SLOT "):
            if keep_plugins and " clap plugin=" in line:
                out.append(line)
            continue
        out.append(line)
    return "\n".join(out)


def only_type(text: str, kind: str) -> str:
    """Every channel's notes kept, but only `kind`'s channels make sound."""
    return re.sub(
        r"INSTRUMENT=(\w+)\n",
        lambda m: m.group(0) if m.group(1) == kind else "INSTRUMENT=testtone\nMUTE=yes\n",
        text,
    )


def main() -> int:
    folder = ROOT / "build" / "bench" / "parts"
    folder.mkdir(parents=True, exist_ok=True)
    base = make_project.build(folder, bars=BARS).read_text(encoding="utf-8")
    rows = [("full", base)]
    rows.append(("no effects (plugins kept)", without_slots(base, keep_plugins=True)))
    rows.append(("no plugins", "\n".join(l for l in base.splitlines() if " clap plugin=" not in l)))
    rows.append(
        (
            "no automation",
            re.sub(
                r"TRACK \d+ name=\"Lane \d+\"\n  AUTOMATION.*?(?=\nTRACK|\n\n)",
                "",
                base,
                flags=re.S,
            ),
        )
    )
    rows.append(("no audio clips", re.sub(r"  AUDIO .*?\n(    WARP .*?\n)*", "", base)))
    rows.append(("no effects, no plugins", without_slots(base, keep_plugins=False)))
    kinds = sorted(set(re.findall(r"INSTRUMENT=(\w+)", base)))
    for kind in kinds:
        rows.append(
            (f"only {kind} sounding, no effects", only_type(without_slots(base, False), kind))
        )
    for name, text in rows:
        load = timed(text, folder, re.sub(r"\W+", "_", name))
        print(f"{name:40} {load:6.1f} % of one core")
    return 0


if __name__ == "__main__":
    sys.exit(main())
