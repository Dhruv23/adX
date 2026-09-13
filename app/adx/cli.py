"""Command-line entry point.

Phase 0 ships the skeleton and one flag. The real subcommands - ``adx render``,
``adx validate``, ``adx fmt``, ``adx diff`` (FINAL_PLAN 5.8) - belong to Phases 2
and 8, and attach as subparsers of the parser built here.
"""

from __future__ import annotations

import argparse
from collections.abc import Sequence

from adx import __version__, engine_version, git_sha


def build_parser() -> argparse.ArgumentParser:
    """Build the top-level argument parser.

    Separate from :func:`main` so later phases can add subcommands without the
    tests having to run a process to inspect them.
    """
    parser = argparse.ArgumentParser(
        prog="adx",
        description="adX - a music production environment whose project format is a text file.",
    )
    parser.add_argument("--version", action="version", version=f"adx {__version__}")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Run the CLI. Returns the process exit status."""
    parser = build_parser()
    parser.parse_args(argv)

    print(f"adx {__version__}")
    print(f"engine {engine_version()} ({git_sha()})")
    return 0
