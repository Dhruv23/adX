"""Solo each channel of a v1 file in both engines and compare broad-band levels.

Low is bands 0-11 (20-250 Hz), mid 12-21 (315 Hz-2.5 kHz), high 22-30. The archived
engine is v1fixed.exe, or v1render.exe with --archived (build_v1.cmd); its solo argument
is the track index, which is the v2 channel order the v1 shim produces.

    python tools/ab/pertrack.py docs/examples/suffocation.adx [--preset P] [--out DIR]
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import engine
import numpy as np
from bands import file_levels
from wav import FloatArray


def broad(levels: FloatArray) -> tuple[float, float, float]:
    def total(part: FloatArray) -> float:
        return float(10.0 * np.log10((10.0 ** (part / 10.0)).sum()))

    return total(levels[:12]), total(levels[12:22]), total(levels[22:])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("adx")
    parser.add_argument("--preset", default="windows-x64-debug")
    parser.add_argument("--out", type=Path, default=Path(tempfile.gettempdir()) / "adx_ab")
    parser.add_argument(
        "--archived", action="store_true", help="compare with v1 as it was, not v1fixed"
    )
    args = parser.parse_args(argv)
    reference = engine.V1_RENDER if args.archived else engine.V1_FIXED
    if not reference.exists():
        raise SystemExit(f"{reference} missing: run tools/ab/build_v1.cmd")
    args.out.mkdir(parents=True, exist_ok=True)

    module = engine.load(args.preset)
    project, _diagnostics = module.Project.load(args.adx)
    text = project.dumps()
    for index, name in enumerate(engine.channel_names(text)):
        ours, theirs = args.out / f"v2_{name}.wav", args.out / f"v1_{name}.wav"
        soloed, _diagnostics = module.Project.loads(engine.solo(text, name))
        module.render_offline(soloed, str(ours), tail_seconds=4.0)
        subprocess.run(
            [str(reference), args.adx, str(theirs), "4", str(index)],
            check=True,
            capture_output=True,
        )
        a, b = broad(file_levels(theirs)), broad(file_levels(ours))
        cells = "  ".join(
            f"{label} {x:6.1f}/{y:6.1f} ({y - x:+5.1f})"
            for label, x, y in zip(("low", "mid", "high"), a, b, strict=True)
        )
        print(f"{name:10s} {cells}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
