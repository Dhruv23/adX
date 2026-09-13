"""The three lint gates that need more than a single command line.

CI calls this (phase_0.md 4.7 steps 6, 7 and 10) and so should you, because it
resolves the same pinned clang-format and clang-tidy binaries CI uses rather than
whatever LLVM happens to be on PATH.

    python tools/lint.py format [--check]
    python tools/lint.py tidy --build-dir build/windows-x64-debug
    python tools/lint.py headers

`ruff` and `mypy` need no driver and are run directly.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import sysconfig
from collections.abc import Iterable, Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

#: Where first-party C++ lives. Everything else in the compile database belongs to
#: a dependency and is none of our business.
CPP_ROOTS = ("engine", "bindings", "tests/cpp")

#: The realtime paths .clang-tidy-rt's ban list applies to (phase_0.md 4.6).
#: Adding a realtime subsystem in a later phase means adding it here.
RT_PATHS = (
    "engine/rt",
    "engine/dsp",
    "engine/graph",
    "engine/transport",
    "engine/instruments",
    "engine/effects",
)

#: FINAL_PLAN 9: "Headers declare; implementation files implement. No 500-line
#: headers." No linter checks this one, which is why it is here.
MAX_HEADER_LINES = 500

_CPP_SUFFIXES = (".cpp", ".h")

#: clang-tidy drives clang-cl over an MSVC compile database, and clang-cl has no
#: equivalent for some of the /Zc: conformance switches adx_set_warnings passes.
#: Their being unused is a fact about clang, not a finding about our code, and
#: WarningsAsErrors: '*' would otherwise turn it into a failed gate.
_TIDY_EXTRA_ARGS = ("--extra-arg=-Wno-unused-command-line-argument",)


def _fail(message: str) -> int:
    print(f"lint: {message}", file=sys.stderr)
    return 1


def _find_tool(name: str) -> Path:
    """Locate a pinned LLVM binary.

    The clang-format and clang-tidy wheels in the dev extra install their binaries
    into the environment's scripts directory, which is on PATH inside an activated
    venv but usually not for a bare `python tools/lint.py`. Checking that directory
    explicitly is what stops this script from silently using a different LLVM
    version than CI.
    """
    scripts = sysconfig.get_path("scripts")
    candidates: list[Path] = []
    if scripts:
        candidates.append(Path(scripts) / (name + ".exe"))
        candidates.append(Path(scripts) / name)
    found = shutil.which(name)
    if found:
        candidates.append(Path(found))
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise SystemExit(
        f"lint: {name} not found. Install the pinned version with: pip install -e .[dev]"
    )


def _first_party_cpp_files() -> list[Path]:
    """Every first-party C++ file, excluding configure_file templates.

    Version.h.in is not C++ - @VAR@ is not a token - and the header it generates is
    machine-written, so neither is formatted or linted.
    """
    files: list[Path] = []
    for root in CPP_ROOTS:
        base = REPO_ROOT / root
        if not base.is_dir():
            continue
        for suffix in _CPP_SUFFIXES:
            files.extend(base.rglob("*" + suffix))
    return sorted(path for path in files if path.is_file())


def _relative(path: Path) -> str:
    try:
        return path.relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def _is_rt_path(relative_path: str) -> bool:
    return any(relative_path.startswith(rt + "/") for rt in RT_PATHS)


def _run_parallel(commands: Sequence[Sequence[str]], labels: Sequence[str]) -> int:
    """Run independent lint invocations concurrently, reporting every failure.

    Every failure, not the first: a gate that stops at the first problem makes one
    lint run per problem the normal workflow.
    """
    failures = 0
    workers = min(len(commands), os.cpu_count() or 4)
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {
            pool.submit(
                subprocess.run,
                list(command),
                capture_output=True,
                text=True,
                check=False,
            ): label
            for command, label in zip(commands, labels, strict=True)
        }
        for future in concurrent.futures.as_completed(futures):
            label = futures[future]
            result = future.result()
            if result.returncode != 0:
                failures += 1
                print("--- " + label, file=sys.stderr)
                sys.stderr.write(result.stdout)
                sys.stderr.write(result.stderr)
    return failures


def cmd_format(args: argparse.Namespace) -> int:
    """Check or apply C++ formatting."""
    clang_format = _find_tool("clang-format")
    files = _first_party_cpp_files()
    if not files:
        return _fail("no C++ sources found")

    flags = ["--dry-run", "--Werror"] if args.check else ["-i"]
    command = [str(clang_format), "--style=file:" + str(REPO_ROOT / ".clang-format"), *flags]
    result = subprocess.run(
        [*command, *(str(path) for path in files)],
        cwd=REPO_ROOT,
        check=False,
    )
    if result.returncode != 0:
        return _fail(
            "formatting differs from .clang-format. Fix it with: python tools/lint.py format"
        )
    print(f"clang-format: {len(files)} files clean")
    return 0


def _tidy_checks_from(config: Path) -> str:
    """Read the Checks list out of .clang-tidy-rt.

    Parsed by hand rather than with a YAML library so this script needs nothing
    beyond the standard library. The file is one `Checks: >` block followed by
    indented lines; anything else is rejected rather than guessed at.
    """
    lines = config.read_text(encoding="utf-8").splitlines()
    start = -1
    for index, line in enumerate(lines):
        if line.startswith("Checks:"):
            start = index
            break
    if start < 0:
        raise SystemExit(f"lint: {config} has no Checks: block")

    collected: list[str] = []
    for line in lines[start + 1 :]:
        if line.strip() in {"...", "---"} or (line and not line[0].isspace()):
            break
        collected.append(line.strip())
    checks = ",".join(part for part in " ".join(collected).split(",") if part.strip())
    if not checks:
        raise SystemExit(f"lint: {config} has an empty Checks: block")
    return checks


def _compile_database_entries(build_dir: Path) -> list[Path]:
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        raise SystemExit(
            f"lint: {_relative(database)} not found. "
            f"Configure first: cmake --preset {build_dir.name}"
        )
    entries = json.loads(database.read_text(encoding="utf-8"))
    seen: dict[str, Path] = {}
    for entry in entries:
        path = Path(entry["file"]).resolve()
        relative_path = _relative(path)
        if not any(relative_path.startswith(root + "/") for root in CPP_ROOTS):
            continue
        seen.setdefault(relative_path, path)
    return [seen[key] for key in sorted(seen)]


def _banned_system_headers(config: Path) -> set[str]:
    """The banned <header> list, read from .clang-tidy so there is one copy of it.

    The option is a folded YAML block of comma-separated globs where a leading `-`
    means disallowed - `*,-mutex,-vector,...`.
    """
    lines = config.read_text(encoding="utf-8").splitlines()
    start = -1
    for index, line in enumerate(lines):
        if "portability-restrict-system-includes.Includes:" in line:
            start = index
            break
    if start < 0:
        raise SystemExit(f"lint: {config} has no portability-restrict-system-includes.Includes")

    indent = len(lines[start]) - len(lines[start].lstrip())
    collected: list[str] = []
    for line in lines[start + 1 :]:
        if not line.strip():
            break
        if len(line) - len(line.lstrip()) <= indent:
            break
        collected.append(line.strip())
    banned = {
        part.strip()[1:] for part in "".join(collected).split(",") if part.strip().startswith("-")
    }
    if not banned:
        raise SystemExit(f"lint: no banned headers parsed from {config}")
    return banned


_INCLUDE_LOCAL = re.compile(r'^\s*#\s*include\s*"([^"]+)"')
_INCLUDE_SYSTEM = re.compile(r"^\s*#\s*include\s*<([^>]+)>")
_SUPPRESSION = "portability-restrict-system-includes"


def _suppressed_lines(lines: list[str]) -> set[int]:
    """Line numbers whose diagnostics a NOLINT comment waives.

    Honours the same three spellings clang-tidy does - same-line NOLINT,
    NOLINTNEXTLINE, and a NOLINTBEGIN/NOLINTEND region - so a file that is already
    exempt from the clang-tidy check (engine/rt/LockGuardCheck.h's deliberate
    <mutex>, engine/rt/AllocGuard.cpp's <new>) is exempt from this one too.
    """
    suppressed: set[int] = set()
    in_region = False
    for index, line in enumerate(lines):
        if "NOLINTBEGIN" in line and _SUPPRESSION in line:
            in_region = True
        if "NOLINTEND" in line and _SUPPRESSION in line:
            in_region = False
        if in_region:
            suppressed.add(index)
        if "NOLINT" in line and _SUPPRESSION in line:
            suppressed.add(index)
            if "NOLINTNEXTLINE" in line:
                suppressed.add(index + 1)
    return suppressed


def _transitive_banned_includes(banned: set[str]) -> list[tuple[str, str, str]]:
    """(offending file, banned header, a realtime source that reaches it).

    Keyed on the file that actually contains the bad include, not on every realtime
    source that reaches it - one header included by twenty realtime files is one
    defect in one place, and reporting it twenty times buries it.

    clang-tidy's portability-restrict-system-includes only inspects the file being
    compiled, not the headers it pulls in - verified, not assumed. Without this walk,
    an engine/core header that included <vector> would hand std::vector to realtime
    code with nothing to say about it.
    """
    findings: dict[tuple[str, str], str] = {}
    for source in _first_party_cpp_files():
        source_rel = _relative(source)
        if not _is_rt_path(source_rel):
            continue

        seen: set[Path] = set()
        pending = [source]
        while pending:
            current = pending.pop()
            if current in seen or not current.is_file():
                continue
            seen.add(current)

            lines = current.read_text(encoding="utf-8").splitlines()
            waived = _suppressed_lines(lines)
            for index, line in enumerate(lines):
                local = _INCLUDE_LOCAL.match(line)
                if local:
                    pending.append(REPO_ROOT / local.group(1))
                    continue
                system = _INCLUDE_SYSTEM.match(line)
                if system and system.group(1) in banned and index not in waived:
                    findings.setdefault((_relative(current), system.group(1)), source_rel)
    return sorted((where, banned_header, root) for (where, banned_header), root in findings.items())


def cmd_tidy(args: argparse.Namespace) -> int:
    """Run clang-tidy over the first-party half of the compile database.

    The realtime ban list is appended for files under RT_PATHS. clang-tidy's
    --checks argument appends to the value read from .clang-tidy, so the
    project-wide set stays in force and the bans are added on top of it.
    """
    clang_tidy = _find_tool("clang-tidy")
    build_dir = (REPO_ROOT / args.build_dir).resolve()
    files = _compile_database_entries(build_dir)
    if not files:
        return _fail(f"no first-party sources in {_relative(build_dir)}/compile_commands.json")

    rt_checks = _tidy_checks_from(REPO_ROOT / ".clang-tidy-rt")

    commands: list[list[str]] = []
    labels: list[str] = []
    rt_count = 0
    for path in files:
        relative_path = _relative(path)
        command = [str(clang_tidy), "-p", str(build_dir), "--quiet", *_TIDY_EXTRA_ARGS]
        if _is_rt_path(relative_path):
            command.append("--checks=" + rt_checks)
            rt_count += 1
        command.append(str(path))
        commands.append(command)
        labels.append(relative_path)

    failures = _run_parallel(commands, labels)
    if failures:
        return _fail(f"clang-tidy: {failures} of {len(files)} files have findings")

    reachable = _transitive_banned_includes(_banned_system_headers(REPO_ROOT / ".clang-tidy"))
    if reachable:
        for where, banned_header, root in reachable:
            reached = "" if where == root else f" (reached from {root})"
            print(f"{where}: banned include <{banned_header}>{reached}", file=sys.stderr)
        return _fail(
            f"{len(reachable)} banned system header(s) reachable from realtime code. "
            "A type banned below the audio callback must not arrive through an engine "
            "header either."
        )

    print(f"clang-tidy: {len(files)} files clean ({rt_count} under the realtime ban list)")
    return 0


def _long_headers() -> Iterable[tuple[str, int]]:
    for path in _first_party_cpp_files():
        if path.suffix != ".h":
            continue
        with path.open(encoding="utf-8") as handle:
            count = sum(1 for _ in handle)
        if count > MAX_HEADER_LINES:
            yield _relative(path), count


def cmd_headers(_args: argparse.Namespace) -> int:
    """Enforce the 500-line header limit."""
    offenders = sorted(_long_headers())
    if offenders:
        for name, count in offenders:
            print(f"{name}: {count} lines (limit {MAX_HEADER_LINES})", file=sys.stderr)
        return _fail(
            f"{len(offenders)} header(s) over {MAX_HEADER_LINES} lines. "
            "Headers declare; implementation files implement (FINAL_PLAN 9)."
        )
    headers = sum(1 for path in _first_party_cpp_files() if path.suffix == ".h")
    print(f"header length: {headers} headers within {MAX_HEADER_LINES} lines")
    return 0


_GATES = {"format": cmd_format, "tidy": cmd_tidy, "headers": cmd_headers}


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="lint", description="adX lint gates.")
    subparsers = parser.add_subparsers(dest="gate", required=True)

    format_parser = subparsers.add_parser("format", help="clang-format over first-party C++")
    format_parser.add_argument(
        "--check", action="store_true", help="report differences instead of fixing them"
    )

    tidy_parser = subparsers.add_parser("tidy", help="clang-tidy over the compile database")
    tidy_parser.add_argument(
        "--build-dir",
        default="build/windows-x64-debug",
        help="directory holding compile_commands.json (default: %(default)s)",
    )

    subparsers.add_parser("headers", help="the 500-line header limit")

    args = parser.parse_args(argv)
    return _GATES[str(args.gate)](args)


if __name__ == "__main__":
    sys.exit(main())
