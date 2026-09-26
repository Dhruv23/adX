# adX

A music production environment whose project format is a human-readable,
hot-reloadable, git-diffable text file.

What adX is and where it is going lives in [FINAL_PLAN.md](FINAL_PLAN.md); how each
phase gets built lives in [plans/](plans/). This file is only how to run it.

## Prerequisites

- Windows x64
- Visual Studio 2022 Build Tools or later, with the **Desktop development with C++**
  workload. Run the commands below from a *x64 Native Tools Command Prompt* (or any
  shell where `cl` is on `PATH`) — the presets use the Ninja generator, which needs
  the compiler already in the environment.
- Python 3.12
- CMake >= 3.26 and Ninja (both ship with the VS C++ workload)

## Build and test

```
pip install -e ".[dev]"
cmake --preset windows-x64-debug && cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
pytest
```

`windows-x64-release` and `windows-x64-relwithdebinfo` are the other two presets.
`python -m adx` prints the app and engine versions.

`ctest` includes the slow gates: the realtime gate, which runs a 60-second audio
stream under the allocator hook, the 100-seed undo test and the 100k-case parser
fuzz. For a fast inner loop use `ctest --preset windows-x64-debug -LE slow`; CI
always runs the full set.

> **After changing C++, re-run `pip install -e ".[dev]"` before `pytest`.**
> The preset builds into `build/windows-x64-*`; the Python extension that
> `import adx_engine` resolves to is built separately by scikit-build-core into
> `build/skbuild-*`. `cmake --build` does not update the second one, so `pytest`
> will keep testing the previous `.pyd` and say nothing. Auto-rebuild on import is
> deliberately off: it would need the MSVC environment present at import time and
> would break `pytest` in any ordinary shell.

## The command line

```
adx validate FILE...          diagnostics with line:column; exit 1 on any error
adx fmt [--check | -i] FILE   canonical formatting
adx fmt --upgrade IN -o OUT   migrate a v1 file (v1 files are never rewritten in place)
adx diff A B                  semantic diff; exit 1 if the projects differ
adx info FILE                 counts, duration, tempo range
```

The format is specified in [docs/adx-format-v2.md](docs/adx-format-v2.md).

## Lint

The same gates CI runs, at the same versions:

```
python tools/lint.py format --check
python tools/lint.py tidy --build-dir build/windows-x64-debug
python tools/lint.py headers
python tools/lint.py format-safety
ruff check app tests/python tools && ruff format --check app tests/python tools
mypy --strict app
```

`python tools/lint.py format` without `--check` fixes the C++ formatting in place.
