"""The three lint gates that need more than a single command line.

CI calls this (phase_0.md 4.7 steps 6, 7 and 10) and so should you, because it
resolves the same pinned clang-format and clang-tidy binaries CI uses rather than
whatever LLVM happens to be on PATH.

    python tools/lint.py format [--check]
    python tools/lint.py tidy --build-dir build/windows-x64-debug [--changed-since REF]
    python tools/lint.py headers
    python tools/lint.py format-safety
    python tools/lint.py positions
    python tools/lint.py rules

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


#: A file may say which thread it runs on, in a `// adx-thread: <which>` line among
#: its first few. The marker overrides the directory in both directions, which is what
#: closes Phase 1's P1-2: realtime safety used to be decided by directory alone, so
#: AudioThread::render - the one function in engine/audio/ that runs on every callback
#: - was outside the ban because the backends beside it allocate at open time.
#:
#:   realtime  the ban applies wherever the file lives (engine/audio/CallbackCore.*)
#:   main      the file is main-thread code inside a realtime directory - a graph
#:             *builder* in engine/graph/ - and may use std::vector. Nothing realtime
#:             can include it without the include walk below reporting it.
_THREAD_MARKER = re.compile(r"^\s*//\s*adx-thread:\s*(realtime|main)\s*$")
_MARKER_SCAN_LINES = 5

_marker_cache: dict[str, str | None] = {}


def _thread_marker(relative_path: str) -> str | None:
    """The file's declared thread, if it declares one."""
    if relative_path not in _marker_cache:
        found: str | None = None
        path = REPO_ROOT / relative_path
        if path.is_file():
            with path.open(encoding="utf-8-sig") as handle:
                for _, line in zip(range(_MARKER_SCAN_LINES), handle, strict=False):
                    match = _THREAD_MARKER.match(line)
                    if match:
                        found = match.group(1)
                        break
        _marker_cache[relative_path] = found
    return _marker_cache[relative_path]


def _is_rt_path(relative_path: str) -> bool:
    marker = _thread_marker(relative_path)
    if marker is not None:
        return marker == "realtime"
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
                # clang-tidy echoes source lines, and those may be any UTF-8 (the Voice
                # instrument's kana): the console code page cannot decode them.
                encoding="utf-8",
                errors="replace",
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


#: Changes that can alter clang-tidy's verdict on files nobody touched: its
#: configuration, this driver, the compile flags, and the pinned clang-tidy version
#: (pyproject.toml). Any of them makes a --changed-since run a full one.
_TIDY_FULL_RUN_TRIGGERS = (
    ".clang-tidy",
    ".clang-tidy-rt",
    "tools/lint.py",
    "CMakeLists.txt",
    "CMakePresets.json",
    "pyproject.toml",
    "cmake/",
)


def _changed_paths(ref: str) -> list[str] | None:
    """Repo-relative paths that differ between `ref` and the working tree, or None
    when `ref` does not name a commit here (a new branch's all-zero `before`, or a
    force-push that dropped it), which callers treat as "check everything"."""
    resolved = subprocess.run(
        ["git", "rev-parse", "--verify", "--quiet", ref + "^{commit}"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if resolved.returncode != 0:
        return None
    diff = subprocess.run(
        ["git", "diff", "--name-only", resolved.stdout.strip()],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return [line for line in diff.stdout.splitlines() if line]


def _local_include_closure(source: Path, cache: dict[Path, frozenset[str]]) -> frozenset[str]:
    """Every first-party file `source` reaches through #include "...", itself included.

    Includes resolve from the repository root, as the rest of this script assumes,
    falling back to the including file's own directory.
    """
    if source in cache:
        return cache[source]
    seen: set[Path] = set()
    pending = [source]
    while pending:
        current = pending.pop()
        if current in seen or not current.is_file():
            continue
        seen.add(current)
        for line in current.read_text(encoding="utf-8").splitlines():
            local = _INCLUDE_LOCAL.match(line)
            if not local:
                continue
            for candidate in (REPO_ROOT / local.group(1), current.parent / local.group(1)):
                if candidate.is_file():
                    pending.append(candidate.resolve())
                    break
    closure = frozenset(_relative(path) for path in seen)
    cache[source] = closure
    return closure


def _tidy_selection(files: list[Path], ref: str) -> tuple[list[Path], str]:
    """The translation units a change since `ref` can affect, and why.

    A unit is selected when it, or any first-party header it includes, changed - a
    header's findings are reported through the units that compile it.
    """
    changed = _changed_paths(ref)
    if changed is None:
        return files, f"{ref} is not a commit here; checking everything"
    for path in changed:
        for trigger in _TIDY_FULL_RUN_TRIGGERS:
            if path == trigger or (trigger.endswith("/") and path.startswith(trigger)):
                return files, f"{path} changed; checking everything"
    changed_cpp = {path for path in changed if path.endswith(_CPP_SUFFIXES)}
    cache: dict[Path, frozenset[str]] = {}
    selected = [path for path in files if _local_include_closure(path, cache) & changed_cpp]
    return selected, f"{len(changed_cpp)} changed C++ file(s) since {ref}"


def cmd_tidy(args: argparse.Namespace) -> int:
    """Run clang-tidy over the first-party half of the compile database.

    The realtime ban list is appended for files under RT_PATHS. clang-tidy's
    --checks argument appends to the value read from .clang-tidy, so the
    project-wide set stays in force and the bans are added on top of it.

    With --changed-since, only the translation units a change can affect are
    checked; the banned-include walk below always covers the whole tree.
    """
    clang_tidy = _find_tool("clang-tidy")
    build_dir = (REPO_ROOT / args.build_dir).resolve()
    files = _compile_database_entries(build_dir)
    if not files:
        return _fail(f"no first-party sources in {_relative(build_dir)}/compile_commands.json")
    total = len(files)
    if args.changed_since:
        files, reason = _tidy_selection(files, args.changed_since)
        print(f"clang-tidy: {len(files)} of {total} files selected ({reason})")

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

    failures = _run_parallel(commands, labels) if commands else 0
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


#: phase_2.md 6: "Zero std::stof/try/catch in engine/format/". Iteration one called
#: std::stof inside try/catch per token, and std::exception::what() carries no
#: position - which is why every one of its diagnostics degraded to "Malformed X on
#: line N". Numbers here go through std::from_chars and report their own column.
_FORMAT_ROOT = "engine/format"
_FORMAT_BANNED = re.compile(r"\b(std::sto[a-z]+|try|catch)\b")


def _strip_comments(line: str) -> str:
    """Drop a // comment, so explaining why stof is banned is not itself a finding."""
    at = line.find("//")
    return line if at < 0 else line[:at]


def cmd_format_safety(_args: argparse.Namespace) -> int:
    """Enforce the engine/format ban on exception-based number parsing."""
    findings: list[str] = []
    files = [path for path in _first_party_cpp_files() if _relative(path).startswith(_FORMAT_ROOT)]
    for path in files:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            match = _FORMAT_BANNED.search(_strip_comments(line))
            if match:
                findings.append(f"{_relative(path)}:{number}: '{match.group(1)}'")
    if findings:
        for finding in findings:
            print(finding, file=sys.stderr)
        return _fail(
            f"{len(findings)} use(s) of std::sto*/try/catch in {_FORMAT_ROOT}. Parse numbers "
            "with std::from_chars and report the column (docs/adx-format-v2.md 3.3)."
        )
    print(f"format safety: {len(files)} files in {_FORMAT_ROOT} use no std::sto*, try or catch")
    return 0


#: phase_3.md 2, the Phase 11 checkpoint made mechanical: "a grep over engine/
#: outside engine/transport/ finds zero occurrences of any engine-owned global
#: position." Time is a set of sources; scheduling takes its source as a parameter,
#: and anything that stores a playback position of its own has quietly made time a
#: scalar again - which is what would make clip launching a transport rewrite.
_TRANSPORT_ROOT = "engine/transport"
_POSITION_BANNED = re.compile(
    r"\b("
    # Iteration one's global, by name, in case it is ever ported back.
    r"(?:m_)?currentSamplePosition"
    # TimeSource's own storage. Nobody else keeps a position the way it does.
    r"|m_positionSamples|m_fractionalSample"
    # Any member, global, static or thread-local named for a playback position.
    r"|(?:m|g|s|t)_\w*(?:[Pp]osition|[Pp]layhead|[Ss]ongPos)\w*"
    # A transport "now" is exactly the global read this gate exists to stop.
    r"|Transport::now"
    r")\b"
)


def cmd_positions(_args: argparse.Namespace) -> int:
    """Enforce that no engine code outside engine/transport/ owns a playback position."""
    findings: list[str] = []
    files = [
        path
        for path in _first_party_cpp_files()
        if _relative(path).startswith("engine/")
        and not _relative(path).startswith(_TRANSPORT_ROOT + "/")
    ]
    for path in files:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            match = _POSITION_BANNED.search(_strip_comments(line))
            if match:
                findings.append(f"{_relative(path)}:{number}: '{match.group(1)}'")
    if findings:
        for finding in findings:
            print(finding, file=sys.stderr)
        return _fail(
            f"{len(findings)} engine-owned playback position(s) outside {_TRANSPORT_ROOT}. "
            "Take a `const TimeSource&` instead (phase_3.md 2)."
        )
    print(f"positions: {len(files)} engine files outside {_TRANSPORT_ROOT} own no position")
    return 0


#: phase_4.md: the audio a render produces is pinned by golden hashes, and they must
#: hold in every build configuration. Library transcendentals do not allow that - a
#: vectorising optimiser may call a different std::sin in Release than in Debug, and
#: two standard libraries disagree in the last bit - so DSP code uses engine/dsp/Math.h's
#: deterministic ones. sqrt, floor, fabs and friends are exactly rounded by IEEE 754
#: and stay allowed.
_DSP_MATH_ROOTS = (
    "engine/dsp",
    "engine/instruments",
    "engine/effects",
    "engine/mixer",
    "engine/graph/nodes",
)
_DSP_MATH_BANNED = re.compile(
    r"\bstd::(sin|cos|tan|sinh|cosh|tanh|asin|acos|atan|atan2|exp|exp2|expm1|log|log2|log10"
    r"|log1p|pow|cbrt|hypot|erf|tgamma|lgamma)[fl]?\s*\("
)


def cmd_dsp_math(_args: argparse.Namespace) -> int:
    """Ban library transcendentals in DSP code: use engine/dsp/Math.h."""
    findings: list[str] = []
    files = [
        path
        for path in _first_party_cpp_files()
        if any(_relative(path).startswith(root + "/") for root in _DSP_MATH_ROOTS)
    ]
    for path in files:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            match = _DSP_MATH_BANNED.search(_strip_comments(line))
            if match:
                findings.append(f"{_relative(path)}:{number}: 'std::{match.group(1)}'")
    if findings:
        for finding in findings:
            print(finding, file=sys.stderr)
        return _fail(
            f"{len(findings)} library transcendental(s) in DSP code. Use engine/dsp/Math.h, "
            "whose results are identical in every build configuration (golden hashes)."
        )
    print(f"dsp-math: {len(files)} DSP files use only deterministic math")
    return 0


def cmd_rules(_args: argparse.Namespace) -> int:
    """FINAL_PLAN 2.2's three rules, mechanically (tools/lint_rules.py, phase_5.md 3)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import lint_rules

    failed = 0
    for name, findings in lint_rules.run_all(REPO_ROOT).items():
        for finding in findings:
            print(f"{name}: {finding}", file=sys.stderr)
        failed += len(findings)
    if failed:
        return _fail(f"{failed} rule finding(s) (FINAL_PLAN 2.2; phase_5.md 3).")
    print(f"rules: {len(lint_rules.CHECKS)} checks clean")
    return 0


_GATES = {
    "rules": cmd_rules,
    "format": cmd_format,
    "tidy": cmd_tidy,
    "headers": cmd_headers,
    "format-safety": cmd_format_safety,
    "positions": cmd_positions,
    "dsp-math": cmd_dsp_math,
}


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
    tidy_parser.add_argument(
        "--changed-since",
        metavar="REF",
        help="check only the files a change since REF can affect (CI's per-push run)",
    )

    subparsers.add_parser("headers", help="the 500-line header limit")
    subparsers.add_parser("format-safety", help="no std::sto*/try/catch in engine/format")
    subparsers.add_parser(
        "positions", help="no engine-global playback position outside engine/transport"
    )
    subparsers.add_parser("dsp-math", help="no library transcendentals in DSP code")
    subparsers.add_parser("rules", help="FINAL_PLAN 2.2's three rules (phase_5.md 3)")

    args = parser.parse_args(argv)
    return _GATES[str(args.gate)](args)


if __name__ == "__main__":
    sys.exit(main())
