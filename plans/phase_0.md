# Phase 0 — Foundation · S

| | |
|---|---|
| **Status** | In progress — every local box in §6 observed to pass 2026-09-12; the two CI boxes are outstanding because the workflow has never run. See §10 and [STATE.md](STATE.md). |
| **Governs** | build system, packaging, CI, lint, test harness, project-wide conventions |
| **FINAL_PLAN refs** | §2.3, §7 Phase 0, §8, §9 |
| **Entry criteria** | Repo is in its post-archive state: `FINAL_PLAN.md`, `plans/`, `docs/`, `_archive/`, `.gitignore`. Nothing else. |
| **Next** | [phase_1.md](phase_1.md) |
| **§5 coverage owned** | none (infrastructure only) |

---

## 1. Objective

Produce the skeleton every later phase builds inside: one build command, one
install command, one test command, all green in CI on Windows x64, with
formatting and static analysis enforced rather than suggested. Phase 0 writes
**no DSP, no parsing, no UI** — its entire value is that Phase 1 can start
writing real code without making any structural decision.

Phase 0 also fixes the project-wide conventions (§4). Those conventions bind
every later phase; they live here rather than in FINAL_PLAN.md because they are
*how*, not *what*.

---

## 2. Deliverables — exact file manifest

```
CMakeLists.txt                    root project, C++20, options, subdirs
CMakePresets.json                 windows-x64-{debug,release,relwithdebinfo}
pyproject.toml                    scikit-build-core, deps, ruff/mypy/pytest config, console script
README.md                         build + run + test, nothing else
.gitattributes                    * text=auto eol=lf ; *.adx text eol=lf ; *.wav binary
.clang-format                     LLVM base, 4-space indent, 100 col, pointer-left
.clang-tidy                       enabled check set + the RT ban list (§4.6)
.editorconfig                     matches .clang-format for editors that ignore it

cmake/
  AdxOptions.cmake                all ADX_* cache options in one place
  AdxWarnings.cmake               adx_set_warnings(target)
  AdxDependencies.cmake           FetchContent declarations, pinned tags, one per dep
  AdxTesting.cmake                adx_add_cpp_test(name SOURCES ...)

engine/
  CMakeLists.txt                  add_library(adx_engine_static STATIC) + include dirs
  core/
    CMakeLists.txt
    Version.h.in                  configured -> Version.h (ADX_VERSION_*, ADX_GIT_SHA)
    Version.cpp                   adx::version() -> std::string_view
    Config.h                      ADX_ASSERT, ADX_UNREACHABLE, ADX_FORCE_INLINE

bindings/
  CMakeLists.txt                  pybind11_add_module(adx_engine)
  module.cpp                      PYBIND11_MODULE(adx_engine, m) — exposes version() only

app/
  adx/
    __init__.py                   __version__ read from adx_engine
    __main__.py                   python -m adx -> prints engine + app version, exits 0
    cli.py                        argparse skeleton; adx --version
    py.typed                      PEP 561 marker (mypy --strict needs it)

tests/
  cpp/
    CMakeLists.txt
    test_smoke.cpp
  python/
    conftest.py
    test_import.py
  golden/
    .gitkeep                      populated in P3/P8

.github/workflows/ci.yml          the gate (§4.7)
```

> **Corrected in §10:** two files not listed above were required by §4.7 and had
> to be added — `tools/lint.py` and `.clang-tidy-rt`.

**Nothing else.** No `engine/dsp/`, no `engine/rt/` — a directory is created by
the phase that first puts a file in it.

---

## 3. Design

### 3.1 One static library, many directories

`engine/` builds exactly **one** target: `adx_engine_static` (alias
`adx::engine`). Each subdirectory has a `CMakeLists.txt` that calls
`target_sources(adx_engine_static PRIVATE ...)`. There is no per-subdirectory
library.

*Why:* twelve small static libs produce link-order problems and
circular-dependency workarounds for no benefit on a single-binary project.
Directory structure is for humans; the linker does not need it.

### 3.2 Include convention

```cmake
target_include_directories(adx_engine_static PUBLIC ${CMAKE_SOURCE_DIR})
```

Every include is repo-root-relative and quoted:

```cpp
#include "engine/rt/SpscRing.h"
#include "engine/core/Config.h"
```

*Why:* headers and sources stay co-located exactly as FINAL_PLAN.md §2.3 draws
them; there is no `include/` vs `src/` split to keep in sync (iteration one had
one, and the two drifted); and every include site names the subsystem it reaches
into, which makes layering violations visible in review.

### 3.3 Namespaces

`adx` is the root, with one nested namespace per `engine/` subdirectory, named
identically: `adx::core`, `adx::rt`, `adx::dsp`, `adx::instruments`,
`adx::effects`, `adx::graph`, `adx::transport`, `adx::project`, `adx::format`,
`adx::analysis`, `adx::render`, `adx::geometry`. No `using namespace` in any
header, ever.

### 3.4 Dependencies — acquisition policy

**FetchContent with pinned tags, not vcpkg.** Iteration one proved FetchContent
resolves every dependency this project needs; vcpkg would add a toolchain
bootstrap and a manifest to maintain for zero gain on one target platform.
Escape hatch, recorded so it is not relitigated: if a future dependency
(realistically the VST3 SDK in Phase 9) is painful under FetchContent, that one
dependency may be vendored or moved to `find_package`, with the reason written
into FINAL_PLAN.md §8.

Cache downloads outside the build tree so deleting `build/` does not re-download
hundreds of megabytes:

```cmake
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH "")
```

> **Corrected in §10:** one shared directory breaks the second build tree. The
> implemented form is `.deps/<build-tree-name>/`.

Add `.deps/` to `.gitignore` in this phase.

**Rule: a dependency is declared in the phase that first needs it, not before.**
Phase 0 declares exactly two:

| Dep | Tag | Why, in Phase 0 |
|---|---|---|
| `pybind11` | `v2.13.6` | the bridge; needed for the trivial module |
| `Catch2` | `v3.7.1` | the C++ test harness |

Everything else (`rtaudio`, `miniaudio`, `rubberband`, `shine`, `flac`,
`onnxruntime`) is declared later. The known-good tags iteration one used are in
`_archive/src-cpp/CMakeLists.txt` and are the starting points.

### 3.5 Build options

All in `cmake/AdxOptions.cmake`, all prefixed `ADX_`:

| Option | Default | Meaning |
|---|---|---|
| `ADX_BUILD_TESTS` | `ON` | build `adx_tests`, register with ctest |
| `ADX_BUILD_BINDINGS` | `ON` | build the `adx_engine` Python module |
| `ADX_ENABLE_RT_GUARD` | `ON` in Debug/RelWithDebInfo | Phase 1's allocator hook |
| `ADX_ENABLE_ONNX` | `OFF` | FINAL_PLAN §8.2 — melody extraction compiles out |
| `ADX_WARNINGS_AS_ERRORS` | `ON` | CI never turns this off |
| `ADX_ENABLE_ASAN` | `OFF` | `/fsanitize=address`; used by P1/P3 nightly jobs |

Declaring `ADX_ENABLE_RT_GUARD` and `ADX_ENABLE_ONNX` now, with no
implementation behind them, is deliberate: it means Phase 1 and Phase 10 add
code, not build plumbing.

### 3.6 Python packaging

```toml
[build-system]
requires = ["scikit-build-core>=0.10", "pybind11>=2.13"]
build-backend = "scikit_build_core.build"

[project]
name = "adx"
requires-python = ">=3.12"
dynamic = ["version"]
dependencies = ["PySide6>=6.7", "numpy>=1.26"]

[project.optional-dependencies]
dev = ["pytest>=8", "pytest-qt>=4.4", "ruff>=0.6", "mypy>=1.11"]

[project.scripts]
adx = "adx.cli:main"

[tool.scikit-build]
wheel.packages = ["app/adx"]
cmake.version = ">=3.26"
cmake.args = ["-DADX_BUILD_TESTS=OFF"]
```

The Python package directory is `app/adx/` — `app/` is the frontend *source
root*, `adx` is the importable package name. An import name of `app` would
collide with half the ecosystem; `adx` will not.

Version has exactly one source of truth: `project(adx VERSION x.y.z)` in the
root `CMakeLists.txt`. It reaches `Version.h` via `configure_file`, the wheel
via scikit-build-core's regex metadata provider reading that same line, and
Python via `adx_engine.version()`. A test asserts the two agree, so drift fails
CI.

### 3.7 One command each

```
pip install -e ".[dev]"
cmake --preset windows-x64-debug && cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
pytest
```

`README.md` contains these four lines plus prerequisites (VS 2022 Build Tools
with the C++ workload, Python 3.12, CMake >= 3.26, Ninja) and nothing else.
FINAL_PLAN.md owns the "why"; README owns the "how do I run it".

---

## 4. Project-wide conventions established here

### 4.1 Compiler flags

MSVC: `/W4 /WX /permissive- /utf-8 /Zc:preprocessor /Zc:__cplusplus /EHsc /std:c++20`.
Release adds `/O2`.

> **Corrected in §10:** the flag named here in the original plan,
> `/fp:contract=off`, does not exist. MSVC's spelling is `/fp:precise`.

**`/fp:fast` is banned project-wide.** It permits reassociation of float
arithmetic, which breaks the bit-identical offline-vs-realtime guarantee
(Phase 3) and the golden-hash tests (FINAL_PLAN §9). `/fp:contract=off` for the
same reason: an FMA contraction that fires in one build and not another changes
the hash. This is not a micro-optimization question — it is a correctness
constraint, and it is cheap because the audio path is not FMA-bound.

### 4.2 Error handling

- **Non-RT code** (parsing, file I/O, project mutation, analysis) may throw, but
  the binding layer catches and translates. Prefer `std::expected<T, Error>` for
  anything with an expected failure mode; reserve exceptions for genuinely
  exceptional conditions.
- **RT code** never throws. Enforced by `noexcept` on every function reachable
  from `process()` and by the clang-tidy ban list in §4.6.

### 4.3 Assertions

`ADX_ASSERT(cond)` in `engine/core/Config.h`: active in Debug and
RelWithDebInfo, compiled out in Release. It must **not** be plain `assert()` —
Phase 1 needs it to route to the RT violation recorder rather than `abort()`
when it fires inside an RT section.

### 4.4 Naming

`PascalCase` types · `camelCase` functions and variables · `m_` member prefix ·
`k` prefix for compile-time constants (`kMaxVoices`) · `SCREAMING_CASE` macros ·
`snake_case` Python. One class, or one coherent free-function group, per file;
the file name is the thing it declares.

### 4.5 Hard limits (FINAL_PLAN §9, made mechanical)

- No function over 150 lines → clang-tidy `readability-function-size`.
- No module-level mutable globals → clang-tidy
  `cppcoreguidelines-avoid-non-const-global-variables`.
- No 500-line headers → a CI script check, because no linter does this one.

### 4.6 The clang-tidy RT ban list

Path-scoped so the bans apply only where they should — under `engine/rt`,
`engine/dsp`, `engine/graph`, `engine/transport`, `engine/instruments`,
`engine/effects`. Banned in those paths: `std::mutex`, `std::lock_guard`,
`std::shared_ptr`, `std::function`, `std::string`, `std::vector`, `new`,
`delete`, `malloc`, `printf`, `std::cout`, `throw`.

Phase 0's obligation is that the file exists with the paths declared and the
mechanism proven to fire. Phase 1 completes the identifier list once the RT
headers exist to test it against.

*This is the static half of Rule 1 (FINAL_PLAN §2.2). Phase 1 adds the dynamic
half.*

### 4.7 CI

One workflow, on push and PR, `windows-latest`, matrix over `Debug` and
`Release`:

1. `actions/setup-python@v5` (3.12); MSVC via `ilammy/msvc-dev-cmd`.
2. Cache `.deps/` keyed on the hash of `cmake/AdxDependencies.cmake`.
3. `cmake --preset` + `cmake --build --preset`.
4. `ctest --preset --output-on-failure`.
5. `pip install -e ".[dev]"` + `pytest -q`.
6. `clang-format --dry-run --Werror` over `engine/ bindings/`.
7. `clang-tidy` over the compile database (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`).
8. `ruff check app tests/python` + `ruff format --check`.
9. `mypy --strict app`.
10. Header-length check script.

Every step is a hard failure. There is no warning tier — a warning tier is how
iteration one accumulated 9,205 untested lines.

---

## 5. Tests

| Test | Asserts |
|---|---|
| `cpp/test_smoke.cpp :: version_is_populated` | `adx::version()` is non-empty, parses as `MAJOR.MINOR.PATCH`, and matches the `ADX_VERSION_*` macros |
| `python/test_import.py :: test_module_imports` | `import adx_engine` succeeds |
| `python/test_import.py :: test_version_agrees` | `adx_engine.version() == adx.__version__` |
| `python/test_import.py :: test_cli_version` | `adx --version` as a subprocess exits 0 and prints the same string |

Four tests is the right number here. Their job is to prove the pipeline works,
not to prove code correct — there is no code yet.

---

## 6. Definition of done

Observed on 2026-09-12, Windows 11 x64, MSVC 19.51 (VS 18), Python 3.12.10,
CMake 4.4.0-rc2, clang-format/clang-tidy 20.1.8/20.1.0.

- [x] `cmake --preset windows-x64-debug` configures from a clean clone with no
      manual steps, under 2 minutes on a warm `.deps/` cache.
      *Observed: 2.8 s to configure after deleting the whole build tree, 5.4 s to
      rebuild. A genuinely cold cache adds two shallow git clones.*
- [x] The build produces `adx_engine_static.lib`, `adx_engine.pyd`,
      `adx_tests.exe`.
      *Observed: `engine/adx_engine_static.lib`,
      `bindings/adx_engine.cp312-win_amd64.pyd`, `tests/cpp/adx_tests.exe`.*
- [x] `ctest` reports 1/1 passing. *Observed on both Debug and Release.*
- [x] `pip install -e ".[dev]"` succeeds and
      `python -c "import adx_engine; print(adx_engine.version())"` prints the version.
      *Observed: prints `0.1.0`.*
- [x] `python -m adx` prints both versions and exits 0.
      *Observed: `adx 0.1.0` / `engine 0.1.0 (d835d5eb0446)`, exit 0.*
- [x] `pytest` reports 3/3 passing.
- [ ] All ten CI steps green on Debug and Release.
      **Outstanding.** The workflow is written but has never executed: nothing has
      been pushed. `ilammy/msvc-dev-cmd`, the `.deps` cache key and whether `ninja`
      is on the `windows-latest` image are all unverified. Tracked as **P0-6** in
      [STATE.md](STATE.md).
- [x] **Each gate observed to fire.** Deliberately break formatting, a
      clang-tidy check, a ruff rule, and a mypy annotation, confirming CI goes
      red for each, then revert. A gate that has never been seen to fail is not
      a gate — it is a hope.
      *Observed locally against the same pinned binaries and the same
      `tools/lint.py` entry points CI invokes — six gates, each broken, each seen
      to fail, each reverted:*
      | Gate | Break | Diagnostic |
      |---|---|---|
      | clang-format | collapsed whitespace in `Version.cpp` | `formatting differs from .clang-format` |
      | clang-tidy (project-wide) | namespace-scope mutable `int` | `cppcoreguidelines-avoid-non-const-global-variables` |
      | clang-tidy (RT ban list) | `#include <vector>` + `return new int` under `engine/rt/` | `portability-restrict-system-includes`, `cppcoreguidelines-owning-memory` |
      | ruff | unused `import os` | `F401` |
      | mypy --strict | `-> int` returning `str` | `Incompatible return value type` |
      | header length | 502-line header | `over 500 lines` |
      *The RT row also proves the **scoping**, not just the checks: byte-identical
      probe files were placed under `engine/rt/` and `engine/core/`, and only the
      `engine/rt/` one drew the two ban-list diagnostics.*
      **Not yet observed through CI itself** — see the box above.
- [ ] FINAL_PLAN.md §10 Phase 0 row set to Done with the date.
      Set to *In progress* rather than Done, because the two boxes above are open.

---

## 10. Corrections made while executing this plan

Recorded here rather than silently applied, because §4 binds every later phase and
three of its statements were wrong.

**§4.1 — `/fp:contract=off` does not exist.** MSVC rejects it (`command line
warning D9002: ignoring unknown option`), and so does `/fp:contract-`; there is no
negative form of `/fp:contract`. The flag that actually disables FMA contraction on
MSVC 2022 and later is **`/fp:precise`**, which `adx_set_warnings()` now passes
explicitly. The intent in §4.1 is unchanged and still correct: contraction is
banned because a contraction that fires in one build and not another changes the
golden hash. Non-MSVC toolchains get `-ffp-contract=off`.

**§3.4 — a single shared `.deps/` cannot work.** `FETCHCONTENT_BASE_DIR` holds each
dependency's `-subbuild` and `-build` directory as well as its sources, and those
carry a `CMakeCache.txt` bound to one binary directory. There are always at least
two build trees — the preset tree and the one scikit-build-core creates for the
wheel — so the second configure fails with *"CMakeCache.txt was created for a
different binary directory"*. This is not theoretical: it is how the first
`pip install -e ".[dev]"` failed here. The cache is now
`.deps/<build-tree-name>/`, keyed on the build tree rather than the build type
(the wheel build is also a Release tree and would otherwise collide with
`windows-x64-release`). §3.4's actual requirement — deleting `build/` must not
re-download — is preserved and was measured.

**§4.1 — the language standard cannot be per-target.** §9 is right that
`adx_set_warnings()` must be per-target so `/WX` never reaches third-party sources,
but the *standard* is different: Catch2 declares its own sources as `cxx_std_14`,
so while it compiles `_MSVC_LANG` reads 201402, so it never emits
`StringMaker<std::string_view>::convert`, so a C++20 test TU that compares a
`string_view` fails to link with `LNK2019`. `CMAKE_CXX_STANDARD 20` is therefore set
globally in the root `CMakeLists.txt`, before any dependency is populated. Warnings
stay per-target; the standard is part of the ABI and cannot.

### Additions to the §2 manifest

`tools/lint.py` — §4.7 step 10 requires a header-length script that §2 does not
list, and steps 6 and 7 need a driver too: clang-tidy must be filtered to
first-party files (the compile database also contains Catch2 and pybind11), and the
RT ban list has to be applied by path. One file with three subcommands rather than
three scripts.

`.clang-tidy-rt` — §4.6 requires the RT ban list to be path-scoped, and
clang-tidy's native mechanism for that is a `.clang-tidy` inside each realtime
directory. None of those directories exists yet, and §2 forbids creating them. So
the ban list lives in this one file and `tools/lint.py` appends it for files under
`RT_PATHS`. Phase 1 creates `engine/rt/` and can collapse this back onto the native
mechanism — tracked as **P0-3** in [STATE.md](STATE.md).

Two dependency-handling details also had to be decided and are recorded in the
files that make them: `PYBIND11_FINDPYTHON` plus an explicit
`find_package(Python 3.12)` in `AdxDependencies.cmake` (left to itself, pybind11's
legacy finder picked the Microsoft Store Python 3.14 stub while pip, pytest and the
console script were all 3.12 — a `.pyd` that builds and then cannot be imported),
and exact version pins for clang-format and clang-tidy in the `dev` extra, since
both tools' behaviour changes between LLVM releases and an unpinned binary means CI
and a developer's machine disagree about whether the tree is clean.

---

## 7. Out of scope (and who owns it)

| Thing | Owner |
|---|---|
| Any `engine/rt/` code; the allocator hook implementation | Phase 1 |
| RtAudio; any audio device | Phase 1 |
| Any project model; any parsing | Phase 2 |
| Any Qt/QML beyond PySide6 being a declared dependency | Phase 5 |
| macOS/Linux presets | post-Phase 11; platform flags are isolated in `AdxWarnings.cmake` for exactly this |

---

## 8. Handoff to Phase 1

Phase 1 can rely on:

- `adx::engine` exists; adding a source file is one `target_sources` line.
- `ADX_ENABLE_RT_GUARD` exists and is `ON` in Debug — Phase 1 writes the code
  behind the flag, not the plumbing.
- `adx_add_cpp_test()` exists — adding a Catch2 test is one CMake line.
- `ADX_ASSERT` exists and is documented as routable, which is precisely what the
  RT violation recorder needs.
- `.clang-tidy` already declares the RT-path scope; Phase 1 fills the identifier
  list.
- CI is *proven* to fail on violations, so Phase 1's allocator gate will
  actually block merges rather than decorating them.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| scikit-build-core editable install fights a CMake target that also builds tests | `cmake.args = ["-DADX_BUILD_TESTS=OFF"]` in the wheel build; C++ tests build only via the CMake preset path |
| `/WX` applied to FetchContent third-party sources breaks the build | `adx_set_warnings()` is applied **per target**, never globally; dependency targets never receive it |
| clang-tidy on Windows needs a compile database the VS generator does not emit | presets use the Ninja generator exclusively |
| Version drift between CMake, wheel metadata and `Version.h` | `test_version_agrees` makes it a CI failure on the first divergence |
