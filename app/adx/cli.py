"""Command-line entry point.

One binary, over the bindings (phase_2.md 4.13):

    adx validate FILE...          report diagnostics; exit 1 if any is an error
    adx fmt [--check] [-i] FILE   canonical formatting
    adx fmt --upgrade IN -o OUT   v1 -> v2 migration, with a report
    adx diff A B                  semantic diff: entities added, removed, changed
    adx info FILE                 counts, duration, tempo range
    adx render FILE -o OUT [...]  offline render and export, headless (no Qt)
    adx gui [FILE]                the application (also: no command at all)
    adx version                   the app and engine versions

Agent-run testing (phase_10.5.md 4.4), headless:

    adx analyze key|chords|bpm FILE   adx describe FILE      adx compare A B
    adx glitch FILE [--reference R]   adx plugins scan|list|probe
    adx midi ports|send|play          adx drive OP [ARG...] --control-file F

A v1 file is never rewritten in place. ``--upgrade`` with an explicit ``-o`` is the
only path from v1 text to v2 text on disk (docs/adx-format-v2.md 12).
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from adx import __version__, engine_version, git_sha

#: Exit statuses (2, a usage error, is argparse's own).
_OK = 0
_FAILED = 1


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
    from adx.engine_bridge import Project

    project, diagnostics = Project.load(str(args.file))
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
    channels = project.channels()
    if channels:
        print("channel list:    " + ", ".join(channels))
        width = max(len(name) for name in channels)
        print("instruments:")
        for name, (kind, known) in zip(channels, project.instruments(), strict=True):
            note = "" if known else "  (unknown type: plays silence)"
            print(f"  {name:<{width}}  {kind}{note}")
    return _OK


def _cmd_render(args: argparse.Namespace) -> int:
    # Headless by construction: export_settings and the bridge import no Qt, so this
    # runs on a CI runner with no display (phase_8.md 3.6).
    from adx.engine_bridge import Project, export_project
    from adx.export_settings import ExportSettings, format_for, parse_normalize, parse_tail

    try:
        settings = ExportSettings(output=args.output, format=args.format or format_for(args.output))
        settings.range = args.range
        settings.from_marker = args.from_marker or ""
        settings.to_marker = args.to_marker or ""
        settings.split_at_markers = args.split_at_markers
        if args.stems:
            settings.stems = args.stems
            settings.master = not args.stems_only
        settings.tail_seconds, settings.tail_until_silence = parse_tail(args.tail)
        if args.normalize:
            settings.normalize, settings.normalize_target = parse_normalize(args.normalize)
        settings.dither = args.dither
        settings.block = args.block
        settings.render_rate = args.rate
        settings.sample_rate = args.sample_rate
        settings.mp3_bitrate = args.bitrate
        settings.flac_level = args.flac_level
        settings.ogg_quality = args.ogg_quality
        settings.midi = args.midi
        request = settings.request()
    except ValueError as error:
        print(f"adx render: {error}", file=sys.stderr)
        return _FAILED

    project, diagnostics = Project.load(str(args.file))
    errors = [item for item in diagnostics if str(item["severity"]) == "error"]
    for item in errors:
        print(
            f"{args.file}:{item['line']}:{item['column']}: error {item['code']}: {item['message']}",
            file=sys.stderr,
        )
    if errors:
        return _FAILED

    result = export_project(project, request)
    if not result["ok"]:
        print(f"adx render: {result['error']}", file=sys.stderr)
        return _FAILED
    for item in result["files"]:
        seconds = int(item["frames"]) / max(1, int(item["sample_rate"]))
        print(f"{item['path']}  {seconds:.3f} s  peak {float(item['peak']):.4f}  {item['hash']}")
    if settings.normalize != "off":
        print(f"normalized by {float(result['gain_db']):+.2f} dB")
    return _OK


def _cmd_gui(args: argparse.Namespace) -> int:
    from adx.application import run

    return run(
        project=args.file,
        null_audio=bool(args.null_audio),
        control=bool(args.control),
        control_file=args.control_file,
    )


def _cmd_version(_args: argparse.Namespace) -> int:
    print(f"adx {__version__}")
    print(f"engine {engine_version()} ({git_sha()})")
    return _OK


# --- agent-run testing, headless (phase_10.5.md 4.4). None of these imports Qt. --------


def _json(value: object) -> None:
    print(json.dumps(value, indent=2, default=str))


def _audio_path(name: str, keep: list[tempfile.TemporaryDirectory[str]]) -> str:
    """An audio file as is; a project rendered to a temporary float WAV first."""
    if not name.lower().endswith(".adx"):
        return name
    from adx.engine_bridge import Project, render_to_file

    project, diagnostics = Project.load(name)
    if any(str(d["severity"]) == "error" for d in diagnostics):
        raise ValueError(f"{name} has errors; adx validate shows them")
    folder = tempfile.TemporaryDirectory(prefix="adx-cli-")
    keep.append(folder)
    out = str(Path(folder.name) / "render.wav")
    render_to_file(project, out, tail_seconds=1.0)
    return out


def _cmd_analyze(args: argparse.Namespace) -> int:
    from adx.engine_bridge import analyze

    keep: list[tempfile.TemporaryDirectory[str]] = []
    try:
        params: dict[str, object] = {"path": _audio_path(args.file, keep)}
        if args.kind == "chords":
            params["frame_seconds"] = args.frame_seconds
        result = analyze(args.kind, params)
    except (ValueError, RuntimeError) as error:
        print(f"adx analyze: {error}", file=sys.stderr)
        return _FAILED
    finally:
        for folder in keep:
            folder.cleanup()
    _json(result)
    return _OK if result.get("ok") else _FAILED


def _read_audio(name: str, keep: list[tempfile.TemporaryDirectory[str]]) -> tuple[Any, int]:
    from adx.testing.wav import read_wav

    return read_wav(_audio_path(name, keep))


def _cmd_describe(args: argparse.Namespace) -> int:
    from adx.testing.describe import describe

    keep: list[tempfile.TemporaryDirectory[str]] = []
    try:
        audio, rate = _read_audio(args.file, keep)
    except (OSError, ValueError) as error:
        print(f"adx describe: {error}", file=sys.stderr)
        return _FAILED
    finally:
        for folder in keep:
            folder.cleanup()
    _json(describe(audio, rate).as_dict())
    return _OK


def _cmd_compare(args: argparse.Namespace) -> int:
    from adx.testing.describe import compare, describe

    keep: list[tempfile.TemporaryDirectory[str]] = []
    try:
        a = describe(*_read_audio(args.a, keep))
        b = describe(*_read_audio(args.b, keep))
    except (OSError, ValueError) as error:
        print(f"adx compare: {error}", file=sys.stderr)
        return _FAILED
    finally:
        for folder in keep:
            folder.cleanup()
    tolerances = {
        k: v
        for k, v in (
            ("band_db", args.band_db),
            ("integrated_lufs", args.lufs),
            ("peak_db", args.peak_db),
        )
        if v is not None
    }
    differences = compare(a, b, tolerances)
    for line in differences:
        print(line)
    if not differences:
        print("within tolerance")
    return _FAILED if differences else _OK


def _cmd_glitch(args: argparse.Namespace) -> int:
    from adx.testing.glitch import find_glitches

    keep: list[tempfile.TemporaryDirectory[str]] = []
    try:
        audio, rate = _read_audio(args.file, keep)
        reference = None
        if args.reference:
            reference, reference_rate = _read_audio(args.reference, keep)
            if reference_rate != rate:
                print(
                    f"adx glitch: {args.reference} is {reference_rate} Hz, not {rate}",
                    file=sys.stderr,
                )
                return _FAILED
    except (OSError, ValueError) as error:
        print(f"adx glitch: {error}", file=sys.stderr)
        return _FAILED
    finally:
        for folder in keep:
            folder.cleanup()
    found = find_glitches(audio, rate, reference=reference)
    for finding in found:
        print(f"{finding}  ({finding.seconds(rate):.4f} s)")
    print(f"{len(found)} finding(s)")
    return _FAILED if found else _OK


def _plugin_format(path: str) -> str:
    suffix = Path(path).suffix.lower()
    if suffix not in (".vst3", ".clap"):
        raise ValueError(f"{path}: a .vst3 or .clap")
    return suffix[1:]


def _cmd_plugins(args: argparse.Namespace) -> int:
    from adx.engine_bridge import Plugins

    if args.host:
        Plugins.set_host(str(Path(args.host).resolve()))
    if not Plugins.available():
        print(f"adx plugins: no plugin host at {Plugins.host_path()}", file=sys.stderr)
        return _FAILED
    Plugins.load_cache(args.cache or "")
    if args.action == "scan":
        scanned = Plugins.scan(args.paths, force=args.force)
        failed = [r for r in Plugins.scan_results() if not r.get("ok", True)]
        print(
            f"{scanned} file(s) scanned, {len(Plugins.listing())} plugin(s), {len(failed)} failed"
        )
        return _OK
    if args.action == "list":
        listing = Plugins.listing()
        if args.json:
            _json(listing)
        else:
            for d in listing:
                kind = "instrument" if d["instrument"] else "effect"
                print(f"{d['format']:<5} {d['name']:<32} {kind:<10} {d['vendor']}  {d['path']}")
        return _OK
    if args.action == "probe":
        try:
            path = args.paths[0]
            probed = Plugins.rescan(_plugin_format(path), path)
        except (IndexError, ValueError) as error:
            print(f"adx plugins probe: {error or 'a plugin path'}", file=sys.stderr)
            return _FAILED
        found = [p for p in Plugins.listing() if Path(p["path"]) == Path(path)]
        described = [Plugins.describe(d["format"], d["uid"], d["path"]) for d in found]
        _json({"scan": probed, "plugins": [d for d in described if d is not None]})
        return _OK if described else _FAILED
    print("adx plugins render: use adx render, which plays plugins", file=sys.stderr)
    return _FAILED


def _cmd_midi(args: argparse.Namespace) -> int:
    from adx.testing import midi

    try:
        if args.action == "ports":
            _json(midi.ports())
            return _OK
        if args.action == "send":
            data = midi.message(args.words[0], args.words[1:]) if args.words else b""
            with midi.MidiOut(args.port) as out:
                out.send(data)
            print(f"sent {data.hex(' ')} to {out.name}")
            return _OK
        count, late = midi.play_file(args.port, args.script)
        print(f"played {count} message(s) to {args.port}, worst lateness {late:.2f} ms")
        return _OK
    except (midi.MidiError, OSError, IndexError) as error:
        print(f"adx midi: {error}", file=sys.stderr)
        return _FAILED


def _value(word: str) -> object:
    try:
        return json.loads(word)
    except json.JSONDecodeError:
        return word


def _cmd_drive(args: argparse.Namespace) -> int:
    from adx.testing.driver import Driver, DriverError

    try:
        if args.op == "launch":
            app = Driver.launch(
                args.project, via=args.via, null_audio=not args.real_audio, detach=True
            )
            app._channel.close()  # leave it running; later calls connect by the file
            print(app.control_file)
            return _OK
        if not args.control_file:
            print("adx drive: --control-file (printed by adx drive launch)", file=sys.stderr)
            return _FAILED
        fields: dict[str, Any] = {}
        positional: list[str] = []
        for word in args.args:
            key, sep, value = word.partition("=")
            if sep and key.isidentifier():
                fields[key] = _value(value)
            else:
                positional.append(word)
        if positional:
            first = {
                "trigger": "action",
                "wait_for": "path",
                "key": "keys",
                "type": "target",
                "screenshot": "path",
                "script": "code",
                "open": "path",
                "answer": "button",
            }
            fields.setdefault(first.get(args.op, "target"), positional[0])
            if args.op == "type" and len(positional) > 1:
                fields.setdefault("text", positional[1])
        app = Driver(args.control_file)
        try:
            result = app.request(args.op, **fields)
        finally:
            app._channel.close()
    except DriverError as error:
        print(f"adx drive: {error}", file=sys.stderr)
        return _FAILED
    except OSError as error:
        print(f"adx drive: cannot reach the app: {error}", file=sys.stderr)
        return _FAILED
    _json(result)
    return _OK


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

    gui = sub.add_parser("gui", help="open the application (the default with no command)")
    gui.add_argument("file", nargs="?", metavar="FILE", help="a project to open")
    gui.add_argument(
        "--null-audio", action="store_true", help="render on a timer instead of an audio device"
    )
    gui.add_argument(
        "--control",
        action="store_true",
        help="open the agent control channel (off by default; ADX_CONTROL=1 does the same)",
    )
    gui.add_argument(
        "--control-file", metavar="PATH", help="where to write the control pipe's name and token"
    )
    gui.set_defaults(handler=_cmd_gui)

    version = sub.add_parser("version", help="print the app and engine versions")
    version.set_defaults(handler=_cmd_version)

    render = sub.add_parser("render", help="offline render and export, headless")
    render.add_argument("file", metavar="FILE")
    render.add_argument("-o", "--output", required=True, metavar="OUT")
    render.add_argument(
        "--format",
        choices=("wav16", "wav24", "wav32f", "mp3", "flac", "ogg"),
        help="default: from OUT's extension (.wav is wav24)",
    )
    render.add_argument("--range", choices=("project", "loop", "markers"), default="project")
    render.add_argument("--from-marker", metavar="NAME", help="with --range markers")
    render.add_argument("--to-marker", metavar="NAME", help="with --range markers")
    render.add_argument(
        "--split-at-markers", action="store_true", help="one file per stretch between markers"
    )
    render.add_argument("--stems", choices=("channels", "inserts", "patterns"))
    render.add_argument("--stems-only", action="store_true", help="with --stems: skip the master")
    render.add_argument("--tail", default="0s", help="4s, 500ms, or 'silence'")
    render.add_argument("--normalize", metavar="TARGET", help="-14LUFS, or -1dB for peak")
    render.add_argument("--dither", choices=("tpdf", "shaped", "off"), default="tpdf")
    render.add_argument(
        "--block", type=int, default=256, help="render block size; renders are per-block exact"
    )
    render.add_argument("--rate", type=int, default=48000, help="render sample rate")
    render.add_argument("--sample-rate", type=int, default=0, help="resample the file to this rate")
    render.add_argument("--bitrate", type=int, default=320, help="MP3 kbit/s (CBR)")
    render.add_argument("--flac-level", type=int, default=5)
    render.add_argument("--ogg-quality", type=float, default=0.6, help="-0.1 to 1")
    render.add_argument("--midi", action="store_true", help="also write OUT's name as .mid")
    render.set_defaults(handler=_cmd_render)

    analyze = sub.add_parser("analyze", help="key, chords or tempo of audio or a project, as JSON")
    analyze.add_argument("kind", choices=("key", "chords", "bpm"))
    analyze.add_argument("file", metavar="FILE", help="audio, or a .adx (rendered first)")
    analyze.add_argument("--frame-seconds", type=float, default=0.5, help="chords: segment grid")
    analyze.set_defaults(handler=_cmd_analyze)

    describe = sub.add_parser("describe", help="audio descriptors (bands, loudness, f0...), JSON")
    describe.add_argument("file", metavar="FILE")
    describe.set_defaults(handler=_cmd_describe)

    compare = sub.add_parser("compare", help="descriptor differences; exit 1 over tolerance")
    compare.add_argument("a", metavar="A")
    compare.add_argument("b", metavar="B")
    compare.add_argument("--band-db", type=float, help="per third-octave band (default 3)")
    compare.add_argument("--lufs", type=float, help="integrated loudness (default 1)")
    compare.add_argument("--peak-db", type=float, help="peak (default 1)")
    compare.set_defaults(handler=_cmd_compare)

    glitch = sub.add_parser("glitch", help="dropouts, repeats, clicks; exit 1 on any")
    glitch.add_argument("file", metavar="FILE")
    glitch.add_argument("--reference", metavar="REF", help="also compare against this render")
    glitch.set_defaults(handler=_cmd_glitch)

    plugins = sub.add_parser("plugins", help="scan, list or probe VST3/CLAP plugins")
    plugins.add_argument("action", choices=("scan", "list", "probe", "render"))
    plugins.add_argument(
        "paths", nargs="*", metavar="PATH", help="scan: folders; probe: one plugin"
    )
    plugins.add_argument("--force", action="store_true", help="scan: rescan unchanged files")
    plugins.add_argument("--json", action="store_true", help="list: as JSON")
    plugins.add_argument("--cache", metavar="FILE", help="a plugin cache other than the user's")
    plugins.add_argument(
        "--host", metavar="EXE", help="the sandbox child, if not beside the engine"
    )
    plugins.set_defaults(handler=_cmd_plugins)

    midi = sub.add_parser("midi", help="list MIDI ports, send a message, play a script")
    midi_sub = midi.add_subparsers(dest="action", required=True)
    midi_sub.add_parser("ports", help="inputs and outputs")
    send = midi_sub.add_parser(
        "send", help="one message: note_on 1 60 100, cc 1 7 90, raw 90 3C 64"
    )
    send.add_argument("port", help="part of an output's name")
    send.add_argument("words", nargs="+", metavar="MESSAGE")
    play = midi_sub.add_parser("play", help="a timed script (adx/testing/midi.py)")
    play.add_argument("port", help="part of an output's name")
    play.add_argument("script", metavar="SCRIPT")
    midi.set_defaults(handler=_cmd_midi)

    drive = sub.add_parser("drive", help="one control-channel request to a running adX")
    drive.add_argument("op", help="launch, or a request: click, trigger, state, wait_for, tree...")
    drive.add_argument("args", nargs="*", metavar="ARG", help="a target, then key=value fields")
    drive.add_argument("--control-file", metavar="F", help="the file adx drive launch printed")
    drive.add_argument("--project", metavar="FILE", help="launch: a project to open")
    drive.add_argument("--via", choices=("adX.cmd", "gui"), default="adX.cmd", help="launch: how")
    drive.add_argument("--real-audio", action="store_true", help="launch: the default device")
    drive.set_defaults(handler=_cmd_drive)

    return parser


def _glue_negative_values(argv: Sequence[str]) -> list[str]:
    """``--normalize -14LUFS`` as ``--normalize=-14LUFS``: argparse takes a value that
    starts with a dash and is not a plain number for another option."""
    out: list[str] = []
    for arg in argv:
        if out and out[-1] == "--normalize" and arg.startswith("-"):
            out[-1] = f"--normalize={arg}"
        else:
            out.append(arg)
    return out


def main(argv: Sequence[str] | None = None) -> int:
    """Run the CLI. Returns the process exit status."""
    parser = build_parser()
    args = parser.parse_args(_glue_negative_values(sys.argv[1:] if argv is None else argv))

    handler = getattr(args, "handler", None)
    if handler is None:
        # No command: the application itself (phase_5.md 2, __main__.py).
        args.file = None
        args.null_audio = False
        handler = _cmd_gui
    result: int = handler(args)
    return result
