"""FINAL_PLAN 2.2's three rules, made mechanical (phase_5.md 3, 5).

    python tools/lint.py rules

Four checks, each a function from a repository root to a list of findings, so the
tests can point them at a planted violation in a temporary tree and see them fire:

- :func:`bridge_importers`   Rule 2's chokepoint: only app/adx/engine_bridge.py imports
                             adx_engine (phase_5.md 4.8).
- :func:`per_note_returns`   Rule 2: no binding returns a list of a per-note or
                             per-event type, or binds such a type as a Python class.
- :func:`gil_unreleased`     Rule 3: every ``def(`` in bindings/ releases the GIL -
                             ``py::call_guard<py::gil_scoped_release>()``, a
                             ``gil_scoped_release`` in its body (or in the body of the
                             function it binds), or a ``// GIL: trivial`` comment saying
                             why it need not. Properties are getters and exempt.
- :func:`engine_pybind`      Rule 1: no pybind11 header is reachable from engine/.

And one limit the plan gives Python as well as C++ (phase_5.md 9, Phase 0 4.5):

- :func:`long_python_functions`  no function in app/ over 150 lines.
"""

from __future__ import annotations

import ast
import re
from pathlib import Path

MAX_FUNCTION_LINES = 150

_ENGINE_IMPORT = re.compile(
    r"^\s*(?:import\s+adx_engine\b|from\s+adx_engine\b)|import_module\(\s*['\"]adx_engine"
)
_BRIDGE = "app/adx/engine_bridge.py"


def _rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def bridge_importers(root: Path) -> list[str]:
    """Modules under app/ other than the bridge that import adx_engine."""
    findings: list[str] = []
    for path in sorted((root / "app").rglob("*.py")):
        if _rel(path, root) == _BRIDGE:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            if _ENGINE_IMPORT.search(line):
                findings.append(
                    f"{_rel(path, root)}:{number}: imports adx_engine (use adx.engine_bridge)"
                )
    return findings


_PYBIND_INCLUDE = re.compile(r"#\s*include\s*[<\"]pybind11/")


def engine_pybind(root: Path) -> list[str]:
    """pybind11 includes or links anywhere under engine/."""
    findings: list[str] = []
    base = root / "engine"
    for path in sorted(base.rglob("*")):
        if path.suffix in (".h", ".cpp", ".hpp", ".inl"):
            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
                if _PYBIND_INCLUDE.search(line):
                    findings.append(f"{_rel(path, root)}:{number}: includes pybind11")
        elif path.name == "CMakeLists.txt" and "pybind11" in path.read_text(encoding="utf-8"):
            findings.append(f"{_rel(path, root)}: mentions pybind11")
    return findings


def _calls(text: str, opener: re.Pattern[str]) -> list[tuple[int, int, str]]:
    """Every call opened by ``opener``: (start offset, end offset, text with parens)."""
    out: list[tuple[int, int, str]] = []
    for match in opener.finditer(text):
        depth = 0
        start = match.end() - 1
        for index in range(start, len(text)):
            char = text[index]
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    out.append((match.start(), index + 1, text[match.start() : index + 1]))
                    break
    return out


_DEF = re.compile(r"(?<![\w>])(?:\.|\b[a-z_]\w*\.)def(?:_static)?\(")
_GUARD = re.compile(r"call_guard\s*<\s*py::gil_scoped_release\s*>|gil_scoped_release")
_TRIVIAL = re.compile(r"//\s*GIL:\s*trivial")
_DEFAULT_INIT = re.compile(r"def\(\s*py::init<>\(\)\s*\)")
_BOUND_FUNCTION = re.compile(r"&\s*([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*[,)]")


def _function_body(text: str, name: str) -> str:
    """The body of a free function or method ``name`` defined in ``text``, or ""."""
    short = name.split("::")[-1]
    signature = re.compile(
        rf"\b{re.escape(short)}\s*\([^;{{]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{{"
    )
    match = signature.search(text)
    if match is None:
        return ""
    depth = 0
    for index in range(match.end() - 1, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[match.end() : index]
    return ""


def gil_unreleased(root: Path) -> list[str]:
    """``def(`` bindings with neither a GIL release nor a justified exemption."""
    findings: list[str] = []
    for path in sorted((root / "bindings").glob("*.cpp")):
        text = path.read_text(encoding="utf-8")
        for start, end, call in _calls(text, _DEF):
            line_end = text.find("\n", end)
            trailing = text[end : line_end if line_end >= 0 else len(text)]
            # The justification may also sit on the line just above the def.
            line_start = text.rfind("\n", 0, start)
            above = text[text.rfind("\n", 0, max(0, line_start)) + 1 : max(0, line_start)]
            # A comment above counts only when the line is the comment and nothing else -
            # not an exemption trailing the previous def.
            above = above if above.strip().startswith("//") else ""
            if _GUARD.search(call) or any(_TRIVIAL.search(s) for s in (call, trailing, above)):
                continue
            # A default constructor does no work.
            if _DEFAULT_INIT.match(call[call.index("def") :]):
                continue
            bound = _BOUND_FUNCTION.search(call)
            if bound is not None and _GUARD.search(_function_body(text, bound.group(1))):
                continue
            name = re.search(r"\(\s*\"([^\"]+)\"", call)
            label = name.group(1) if name else call[:40]
            number = text.count("\n", 0, start) + 1
            findings.append(
                f"{_rel(path, root)}:{number}: def '{label}' neither releases the GIL nor says "
                "'// GIL: trivial'"
            )
    return findings


_PER_NOTE_TYPE = (
    r"(?:adx::)?(?:project::|bindings::|graph::)?"
    r"(?:Note|NoteExtras|NoteRecord|ScheduledEvent|Breakpoint|PitchPoint)\b"
)
_BOUND_CLASS = re.compile(rf"py::class_<\s*{_PER_NOTE_TYPE}")
_VECTOR_DECLARED = re.compile(rf"std::vector<\s*{_PER_NOTE_TYPE}[^>]*>\s+(\w+)")
_LIST_PER_NOTE = re.compile(
    r"for\s*\([^)]*\b(?:Note|ScheduledEvent)\s*&[^)]*\)[^}]*\.append\(", re.S
)


def per_note_returns(root: Path) -> list[str]:
    """Bindings that hand Python one object per note or event."""
    findings: list[str] = []
    for path in sorted((root / "bindings").glob("*.cpp")):
        text = path.read_text(encoding="utf-8")
        for match in _BOUND_CLASS.finditer(text):
            number = text.count("\n", 0, match.start()) + 1
            findings.append(f"{_rel(path, root)}:{number}: binds a per-note type as a Python class")
        for start, _end, call in _calls(text, _DEF):
            number = text.count("\n", 0, start) + 1
            for declared in _VECTOR_DECLARED.finditer(call):
                returned = re.compile(rf"return\s+(?:std::move\(\s*)?{declared.group(1)}\s*\)?\s*;")
                if returned.search(call):
                    findings.append(
                        f"{_rel(path, root)}:{number}: "
                        "a binding returns a vector of notes or events"
                    )
            if _LIST_PER_NOTE.search(call):
                findings.append(
                    f"{_rel(path, root)}:{number}: a binding builds a Python list per note"
                )
    return findings


def long_python_functions(root: Path) -> list[str]:
    """Functions in app/ longer than MAX_FUNCTION_LINES."""
    findings: list[str] = []
    for path in sorted((root / "app").rglob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if (
                isinstance(node, ast.FunctionDef | ast.AsyncFunctionDef)
                and node.end_lineno is not None
            ):
                length = node.end_lineno - node.lineno + 1
                if length > MAX_FUNCTION_LINES:
                    findings.append(
                        f"{_rel(path, root)}:{node.lineno}: {node.name} is {length} lines"
                    )
    return findings


CHECKS = {
    "bridge_is_only_importer": bridge_importers,
    "no_per_note_python_objects": per_note_returns,
    "gil_released_on_heavy_calls": gil_unreleased,
    "engine_has_no_pybind_include": engine_pybind,
    "python_function_length": long_python_functions,
}


def run_all(root: Path) -> dict[str, list[str]]:
    """Every check's findings, by check name."""
    return {name: check(root) for name, check in CHECKS.items()}
