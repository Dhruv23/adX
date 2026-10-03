"""Render an .adx through the engine of a CMake build tree.

python tools/ab/render.py docs/examples/suffocation.adx out.wav [--preset P] [--tail S]
"""

from __future__ import annotations

import argparse
import sys

import engine


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("adx")
    parser.add_argument("wav")
    parser.add_argument("--preset", default="windows-x64-debug")
    parser.add_argument("--tail", type=float, default=4.0, help="seconds after the content")
    args = parser.parse_args(argv)

    module = engine.load(args.preset)
    project, _diagnostics = module.Project.load(args.adx)
    stats = module.render_offline(project, args.wav, tail_seconds=args.tail)
    keys = ("frames", "peak", "rms", "rt_violations", "wall_seconds", "hash")
    print({key: stats[key] for key in keys})
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
