"""Command-line entry point.

One binary, over the bindings (phase_2.md 4.13):

    adx validate FILE...          report diagnostics; exit 1 if any is an error
    adx fmt [--check] [-i] FILE   canonical formatting
    adx fmt --upgrade IN -o OUT   v1 -> v2 migration, with a report
    adx diff A B                  semantic diff: entities added, removed, changed
    adx info FILE                 counts, duration, tempo range
    adx render ...                Phase 8; declared now so it is not absent

A v1 file is never rewritten in place. ``--upgrade`` with an explicit ``-o`` is the
only path from v1 text to v2 text on disk (docs/adx-format-v2.md 12).
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path

from adx import __version__, engine_version, git_sha

#: Exit statuses. 2 is argparse's own for a usage error, and "not implemented yet"
#: is closer to a usage error than to a failed check.
_OK = 0
_FAILED = 1
_UNAVAILABLE = 2


def _read(path: Path) -> str:
    # newline="" keeps CRLF intact: the formatter preserves a file's line endings,
    # and text mode would have normalised them before it ever saw them.
    with path.open(encoding="utf-8", newline="") as handle:
        return handle.read()


def _write(path: Path, text: str) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def _cmd_validate(args: argparse.Namespace) -> int:
    from adx.format import validate_text

    failed = False
    for name in args.files:
        path = Path(name)
        diagnostics = validate_text(
            _read(path), str(path.parent), check_sample_files=args.check_samples
        )
        for item in diagnostics:
            if item.severity == "info" and not args.verbose:
                continue
            print(item.render(name))
        errors = sum(1 for item in diagnostics if item.is_error)
        warnings = sum(1 for item in diagnostics if item.severity == "warning")
        print(f"{name}: {errors} error(s), {warnings} warning(s)")
        failed = failed or errors > 0
    return _FAILED if failed else _OK


def _cmd_fmt(args: argparse.Namespace) -> int:
    from adx.format import format_text

    path = Path(args.file)
    original = _read(path)
    formatted, diagnostics, version = format_text(original)

    errors = [item for item in diagnostics if item.is_error]
    if errors:
        for item in errors:
            print(item.render(args.file), file=sys.stderr)
        print(f"{args.file}: not formatted, it has errors", file=sys.stderr)
        return _FAILED

    if args.upgrade:
        if not args.output:
            print(
                "adx fmt --upgrade needs -o OUT; v1 files are never rewritten in place",
                file=sys.stderr,
            )
            return _FAILED
        for item in diagnostics:
            print(item.render(args.file), file=sys.stderr)
        _write(Path(args.output), formatted)
        print(f"upgraded {args.file} -> {args.output}", file=sys.stderr)
        return _OK

    if version < 2:
        if args.check:
            print(f"{args.file}: v1 file; would be migrated")
            return _FAILED
        print(
            f"{args.file} is a v1 file. Migrate it explicitly with: "
            f"adx fmt --upgrade {args.file} -o OUT",
            file=sys.stderr,
        )
        return _FAILED

    if args.check:
        if formatted != original:
            print(f"{args.file}: would reformat")
            return _FAILED
        return _OK
    if args.in_place:
        if formatted != original:
            _write(path, formatted)
        return _OK
    # Bytes, not text: stdout is a text stream that translates every LF to CRLF on
    # Windows, so a CRLF file written through it would come out with CR CR LF -
    # every line ending doubled, and no longer the canonical form it claims to be.
    sys.stdout.flush()
    sys.stdout.buffer.write(formatted.encode("utf-8"))
    sys.stdout.buffer.flush()
    return _OK


def _cmd_diff(args: argparse.Namespace) -> int:
    from adx.format import diff_text

    changes = diff_text(_read(Path(args.before)), _read(Path(args.after)))
    for change in changes:
        print(change.render())
    # diff(1)'s convention: 1 means "they differ", which is what makes this usable in
    # a script as a question rather than only as a report.
    return _FAILED if changes else _OK


def _cmd_info(args: argparse.Namespace) -> int:
    import adx_engine

    project, diagnostics = adx_engine.Project.load(str(args.file))
    info = project.info()
    for item in diagnostics:
        if str(item["severity"]) == "error":
            print(
                f"{args.file}:{item['line']}:{item['column']}: error {item['code']}: "
                f"{item['message']}",
                file=sys.stderr,
            )
    title = str(info["title"]) or "(untitled)"
    seconds = float(info["length_seconds"])
    print(f"title:           {title}")
    print(f"format:          v{int(info['source_version'])}")
    print(f"length:          {int(seconds // 60)}:{seconds % 60:05.2f}")
    bpm_min = float(info["bpm_min"])
    bpm_max = float(info["bpm_max"])
    tempo = f"{bpm_min:g}" if bpm_min == bpm_max else f"{bpm_min:g}-{bpm_max:g}"
    print(f"tempo:           {tempo} bpm")
    for key in ("channels", "patterns", "notes", "playlist_tracks", "inserts", "markers"):
        print(f"{key + ':':<17}{int(info[key])}")
    channels = [str(name) for name in project.channels]
    if channels:
        print("channel list:    " + ", ".join(channels))
    return _OK


def _cmd_render(_args: argparse.Namespace) -> int:
    print("adx render is not available until Phase 8 (plans/phase_8.md).", file=sys.stderr)
    return _UNAVAILABLE


def build_parser() -> argparse.ArgumentParser:
    """Build the top-level argument parser.

    Separate from :func:`main` so tests can inspect it without running a process.
    """
    parser = argparse.ArgumentParser(
        prog="adx",
        description="adX - a music production environment whose project format is a text file.",
    )
    parser.add_argument("--version", action="version", version=f"adx {__version__}")
    sub = parser.add_subparsers(dest="command")

    validate = sub.add_parser("validate", help="report diagnostics; exit 1 on any error")
    validate.add_argument("files", nargs="+", metavar="FILE")
    validate.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="also print info diagnostics (the v1 migration report)",
    )
    validate.add_argument(
        "--check-samples", action="store_true", help="check that referenced sample files exist"
    )
    validate.set_defaults(handler=_cmd_validate)

    fmt = sub.add_parser("fmt", help="canonical formatting")
    fmt.add_argument("file", metavar="FILE")
    mode = fmt.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="exit 1 if formatting would change")
    mode.add_argument("-i", "--in-place", action="store_true", help="rewrite FILE")
    mode.add_argument("--upgrade", action="store_true", help="migrate a v1 file (needs -o)")
    fmt.add_argument("-o", "--output", metavar="OUT", help="where --upgrade writes")
    fmt.set_defaults(handler=_cmd_fmt)

    diff = sub.add_parser("diff", help="semantic diff; exit 1 if the projects differ")
    diff.add_argument("before", metavar="A")
    diff.add_argument("after", metavar="B")
    diff.set_defaults(handler=_cmd_diff)

    info = sub.add_parser("info", help="counts, duration, tempo range")
    info.add_argument("file", metavar="FILE")
    info.set_defaults(handler=_cmd_info)

    render = sub.add_parser("render", help="offline render (Phase 8)")
    render.add_argument("args", nargs=argparse.REMAINDER)
    render.set_defaults(handler=_cmd_render)

    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Run the CLI. Returns the process exit status."""
    parser = build_parser()
    args = parser.parse_args(argv)

    handler = getattr(args, "handler", None)
    if handler is None:
        print(f"adx {__version__}")
        print(f"engine {engine_version()} ({git_sha()})")
        return _OK
    result: int = handler(args)
    return result
