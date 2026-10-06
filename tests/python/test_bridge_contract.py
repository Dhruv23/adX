"""The four Rule-1/2/3 checks, each shown to pass on the tree and to fire on a planted
violation (phase_5.md 3, 5; DoD "observed to fire"), plus the contracts the bridge
and the theme share with the engine."""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import lint_rules

from adx import engine_bridge
from adx.theme import tokens


def test_tree_is_clean(repo_root: pathlib.Path) -> None:
    """`tools/lint.py rules` passes on the repository as it stands."""
    result = subprocess.run(
        [sys.executable, str(repo_root / "tools" / "lint.py"), "rules"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr


def _tree(tmp_path: pathlib.Path, files: dict[str, str]) -> pathlib.Path:
    for name, text in files.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return tmp_path


def test_bridge_is_only_importer(tmp_path: pathlib.Path) -> None:
    root = _tree(
        tmp_path,
        {
            "app/adx/engine_bridge.py": "import adx_engine\n",
            "app/adx/panels/sneaky.py": "from adx_engine import Project\n",
            "app/adx/also.py": "import importlib\nx = importlib.import_module('adx_engine')\n",
        },
    )
    findings = lint_rules.bridge_importers(root)
    assert len(findings) == 2
    assert all("engine_bridge" not in f.split(":")[0] for f in findings)


def test_no_per_note_python_objects(tmp_path: pathlib.Path) -> None:
    bad = """
void f(py::module_& m) {
    py::class_<adx::project::Note>(m, "Note");
    m.def("notes", [](const ProjectHandle& h) {
        std::vector<adx::project::Note> out = h.clip().notes;
        return out;
    });
    m.def("listed", [](const ProjectHandle& h) {
        py::list out;
        for (const adx::project::Note& note : h.clip().notes) { out.append(note.pitch); }
        return out;
    });
    m.def("fine", [](const ProjectHandle& h) {  // GIL: trivial
        std::vector<adx::project::Note> input = h.clip().notes;
        return input.size();
    });
}
"""
    findings = lint_rules.per_note_returns(_tree(tmp_path, {"bindings/bad.cpp": bad}))
    assert len(findings) == 3, findings


def test_gil_released_on_heavy_calls(tmp_path: pathlib.Path) -> None:
    bad = """
void heavy(Thing& t) { const py::gil_scoped_release released; t.work(); }
void f(py::module_& m) {
    m.def("released", [](Thing& t) { const py::gil_scoped_release released; t.work(); });
    m.def("guarded", &other, py::call_guard<py::gil_scoped_release>());
    m.def("bound", &heavy);
    // GIL: trivial - reads a field
    m.def("getter", [](Thing& t) { return t.x; });
    m.def("inline", [](Thing& t) { return t.y; }); // GIL: trivial - a field
    m.def("forgot", [](Thing& t) { t.work(); });
    cls.def(py::init<>());
}
"""
    findings = lint_rules.gil_unreleased(_tree(tmp_path, {"bindings/bad.cpp": bad}))
    assert len(findings) == 1
    assert "'forgot'" in findings[0]


def test_engine_has_no_pybind_include(tmp_path: pathlib.Path) -> None:
    root = _tree(
        tmp_path,
        {
            "engine/core/Fine.h": "#include <vector>\n",
            "engine/geometry/Bad.cpp": '#include "pybind11/numpy.h"\n',
            "engine/graph/CMakeLists.txt": "target_link_libraries(x pybind11::module)\n",
        },
    )
    assert len(lint_rules.engine_pybind(root)) == 2


def test_python_function_length(tmp_path: pathlib.Path) -> None:
    body = "\n".join(f"    x{i} = {i}" for i in range(160))
    root = _tree(tmp_path, {"app/adx/long.py": f"def long() -> None:\n{body}\n"})
    assert len(lint_rules.long_python_functions(root)) == 1


def test_color_roles_agree() -> None:
    """tokens.py names every engine colour role, index for index."""
    assert tokens.ROLE_NAMES == engine_bridge.COLOR_ROLES
    for theme in tokens.THEMES.values():
        assert set(theme.roles) == set(tokens.ROLE_NAMES), theme.name


def test_quick_plugin_matches_pyside() -> None:
    """adx_quick is built against exactly PySide6's Qt (tools/fetch_qt.py)."""
    if not engine_bridge.quick_plugin_path().exists():
        if os.environ.get("CI"):
            pytest.fail("adx_quick was not built in CI")
        pytest.skip("adx_quick not built")
    from PySide6.QtCore import qVersion

    assert engine_bridge.quick_plugin_qt_version() == qVersion()


def test_fetch_qt_pins_pysides_version() -> None:
    """The SDK version fetch_qt.py downloads is the PySide6 version installed."""
    import fetch_qt
    import PySide6

    assert PySide6.__version__ == fetch_qt.DEFAULT_VERSION


def test_cli_does_not_need_qt_quick(repo_root: pathlib.Path) -> None:
    """The CLI runs where the GUI plugin is absent: it never loads adx_quick."""
    result = subprocess.run(
        [sys.executable, "-m", "adx", "info", str(repo_root / "docs/examples/suffocation.adx")],
        capture_output=True,
        text=True,
        check=True,
    )
    assert "channels:" in result.stdout
    assert shutil.which(sys.executable)
