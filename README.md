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
python tools/fetch_qt.py
pip install -e ".[dev]"
cmake --preset windows-x64-debug && cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
pytest
```

`windows-x64-release` and `windows-x64-relwithdebinfo` are the other two presets.

`python tools/fetch_qt.py` puts the Qt C++ SDK that matches PySide6 into `.qt/`
(gitignored, about 2 GB extracted). The piano roll and waveform view draw through a
small C++ Qt Quick plugin (`bindings/qml/`), and PySide6 ships Qt's DLLs but not the
headers and import libraries a C++ plugin compiles against. Without the SDK, everything
except that plugin still builds, and the application says at start-up what is missing.

`python -m adx` opens the application (`adx gui [FILE]`, with `--null-audio` to render
on a timer instead of an audio device); `adx version` prints the app and engine
versions. `pytest -m gpu` runs the frame-rate measurement, which needs a real GPU and
display (CI deselects it).

`ctest` includes the slow gates: the realtime gates, which run 60-second audio
streams under the allocator hook, the 100-seed undo test, the 100k-case parser fuzz,
and the render gate - a 200-channel, 100k-note project rendered offline and captured
from a live stream at three block sizes, which must hash identically. For a fast inner
loop use `ctest --preset windows-x64-debug -LE slow`; CI always runs the full set.

The golden corpus (`tests/golden/`) pins what adX renders: `golden_corpus_stable`
compares every fixture's render hash with `tests/golden/hashes.txt`. When a change is
*meant* to alter the sound, regenerate the file with `ADX_UPDATE_GOLDEN=1` set and say
why in the commit.

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
adx gui [FILE]                the application (the default with no command)
adx version                   app and engine versions
```

The format is specified in [docs/adx-format-v2.md](docs/adx-format-v2.md).

## Rendering from Python

```python
import adx_engine
project, diagnostics = adx_engine.Project.load("docs/examples/suffocation.adx")
stats = adx_engine.render_offline(project, "out.wav")   # 32-bit float WAV

engine = adx_engine.Engine()             # null_backend=True for no audio device
engine.set_project(project)
engine.transport.play()
engine.start()
engine.transport.position_ticks()        # poll it; one atomic read
```

Inside the application, only `app/adx/engine_bridge.py` imports `adx_engine`
(`tools/lint.py rules` enforces it); everything else goes through the bridge.

## Lint

The same gates CI runs, at the same versions:

```
python tools/lint.py format --check
python tools/lint.py tidy --build-dir build/windows-x64-debug
python tools/lint.py headers
python tools/lint.py format-safety
python tools/lint.py positions
python tools/lint.py rules
ruff check app tests/python tools && ruff format --check app tests/python tools
mypy --strict app
```

`python tools/lint.py format` without `--check` fixes the C++ formatting in place.
