# Phase 5 — Frontend foundation · L

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `app/adx/` shell, `engine/geometry/`, the pybind11 zero-copy surface, the QML scene-graph pipeline |
| **FINAL_PLAN refs** | §2.2 in full (all three rules), §3.1 (ClipPeakCache), §3.3.10, §5.1 piano roll, §7 Phase 5 |
| **Entry criteria** | [phase_4.md](phase_4.md) Tranche A complete (§3 of that file). Tranches B and C may run in parallel with this phase. |
| **Next** | [phase_6.md](phase_6.md) |
| **§5 coverage owned** | §5.1 piano roll (all editing tools, all per-note lanes, ghost notes, scale highlighting, chord/arp tools, quantize/humanize) · §5.10 zoomable clip waveforms from mipmapped peaks |

---

## 1. Objective

Prove the architecture FINAL_PLAN §2.2 bets on, on the hardest surface in the
application, before building thirty panels on top of an assumption.

The bet is that Python + Qt can drive a DAW UI **if and only if** the three
rules hold. The piano roll is where that bet is won or lost: it is the densest,
most interactive, most frequently redrawn surface in any DAW. If it holds 60 fps
over 10,000 notes while panning and zooming, every other panel is easier. If it
does not, the right time to discover that is now — with one panel written — not
in Phase 6 with twenty.

So this phase deliberately builds **one** hot surface end to end rather than
many partially. The CI performance gate is the deliverable, not a nice-to-have.

---

## 2. Deliverables — exact file manifest

```
engine/geometry/
  GeometryBuffer.h/.cpp    C++-owned vertex storage + revision counter (§4.1)
  Viewport.h               the view rect in musical/pitch space + zoom
  PianoRollGeometry.h/.cpp notes, grid, ghost notes, lanes, selection (§4.4)
  WaveformGeometry.h/.cpp  mipmapped min/max peaks -> vertices (§4.5)
  ClipPeakCache.h/.cpp     PORTED: tiered peaks, invalidation key (§4.5)
  HitTest.h/.cpp           point/rect -> ids, in C++ (§4.6)
  ColorPalette.h           semantic color indices; theme resolves them

bindings/
  geometry.cpp             buffer protocol exposure, GeometryLease, hit tests
  meters.cpp               one-call-per-frame ring reads

app/adx/
  __main__.py              launches the shell
  application.py           QApplication, engine lifetime, global error handling
  mainwindow.py            QMainWindow, dock layout, menus, persistence
  engine_bridge.py         the ONLY module that touches adx_engine (§4.8)
  transport_bar.py         play/stop/record/loop/tempo/position/metronome
  panels/
    base.py                DockPanel base: title, state save/restore
    piano_roll/
      panel.py             the dock host + toolbar + Python-side tool logic
      view.qml             QQuickItem host, pan/zoom transform, overlays
      PianoRollItem.h/.cpp C++ QQuickItem + QSGGeometryNode (§4.3)
      tools.py             draw, paint, select, slice, glue, strum, mute
      quantize.py          quantize + humanize dialogs and logic
      scales.py            scale highlighting + snapping
    waveform/
      widget.py            reusable zoomable waveform view
      WaveformItem.h/.cpp  QQuickItem + QSGGeometryNode
    browser.py             file/preset tree (content in Phase 6)
  widgets/
    knob.py  fader.py  meter.py  scope.py  spectrum.py
    ruler.py  keyboard.py  timeline.py
  theme/
    tokens.py              color/metric tokens
    dark.qml  light.qml
  scripting/
    __init__.py            placeholder; Phase 7 fills it

bindings/qml/              the C++ QQuickItem subclasses, built into a Qt plugin
  CMakeLists.txt
  AdxQuickPlugin.cpp       registers PianoRollItem, WaveformItem

tests/python/
  test_geometry_zero_copy.py
  test_piano_roll_tools.py
  test_hit_test.py
  test_bridge_contract.py
  test_perf_geometry.py     the blocking CI gate (§4.9)
  test_perf_frames.py       GPU-marked, non-blocking (§4.9)
tests/cpp/geometry/
  test_geometry_buffer.cpp  test_peak_cache.cpp  test_hittest.cpp
  bench_piano_roll.cpp      the C++ budget the CI gate enforces
```

---

## 3. The three rules, made concrete

FINAL_PLAN §2.2 states them. This is what each one *is*, in this phase:

**Rule 1 — the audio thread never touches Python.** In this phase that is
automatic: nothing here runs on the audio thread. It becomes enforceable by a
CI grep that no pybind11 header is included from `engine/`, and that
`engine/geometry/` is main-thread-only by contract (documented in its header).

**Rule 2 — the FFI boundary is crossed O(interactions), never O(notes).** Every
API added in this phase is audited against a list of forbidden shapes:

- returning `list[Note]`, `list[dict]`, or any per-note Python object — **bug**
- a Python loop that calls the engine once per note — **bug**
- rebuilding geometry on pan or zoom — **bug** (it is a transform)

Legal shapes: one call returning one numpy view; one call per user interaction;
one call per frame for meters.

**Rule 3 — every engine call doing real work releases the GIL.** A CI check
greps `bindings/` for `def(`/`m.def(` without `py::call_guard<py::gil_scoped_release>()`
and requires either the guard or an explicit `// GIL: trivial` comment
justifying the exemption. Trivial getters are exempt; nothing else is.

---

## 4. Design

### 4.1 `GeometryBuffer` — the zero-copy contract

```cpp
namespace adx::geometry {

class GeometryBuffer {
public:
    // Main thread only. Grows by reallocation; never called during a Qt paint.
    std::span<float> resize(size_t vertexCount, size_t floatsPerVertex);

    const float* data() const noexcept;
    size_t vertexCount() const noexcept;
    size_t floatsPerVertex() const noexcept;

    // Bumped on every content change. Python compares it to decide whether to
    // re-upload; Qt compares it to decide whether to mark the node dirty.
    uint64_t revision() const noexcept;

private:
    std::vector<float> m_storage;
    uint64_t m_revision = 0;
};

} // namespace adx::geometry
```

Exposed through pybind11's buffer protocol:

```cpp
py::class_<GeometryBuffer>(m, "GeometryBuffer", py::buffer_protocol())
    .def_buffer([](GeometryBuffer& b) {
        return py::buffer_info(
            const_cast<float*>(b.data()), sizeof(float),
            py::format_descriptor<float>::format(), 2,
            { b.vertexCount(), b.floatsPerVertex() },
            { sizeof(float) * b.floatsPerVertex(), sizeof(float) });
    })
    .def_property_readonly("revision", &GeometryBuffer::revision);
```

`np.asarray(buf)` is then a view with zero copies. 10,000 notes cost one Python
object.

**Lifetime is the sharp edge.** A numpy view over C++ storage dangles if the
buffer reallocates while Python holds it. Two defenses:

1. **`GeometryLease`** — a context manager that pins the buffer. `resize()`
   inside an active lease is an error, not a reallocation.

   ```python
   with engine.piano_roll_geometry(pattern_id, viewport) as lease:
       verts = np.asarray(lease)      # zero-copy
       item.upload(verts)             # consumed entirely inside the with-block
   ```

2. **Revision check on exit** — the lease asserts the revision is unchanged when
   it closes, catching any path that mutated behind its back.

The rule, stated once and enforced by review: **a numpy view never outlives its
`with` block.** The C++ side asserts it in debug via a lease counter.

### 4.2 Python owns layout, C++ owns geometry

FINAL_PLAN §2.2's practical consequence, spelled out as a division of labor:

| Concern | Owner | Why |
|---|---|---|
| Where the panel sits, its size, its docking | Python | changes at human speed |
| What the viewport is (musical range, pitch range, zoom) | Python | it is UI state |
| Turning the viewport + pattern into vertices | **C++** | O(notes) |
| Uploading vertices to the GPU | C++ (`QSGGeometryNode`) | must not touch Python |
| Pan / zoom | **neither** — a transform matrix | no rebuild at all |
| Which note is under the cursor | **C++** (`HitTest`) | O(notes) |
| What a click *means* (tool state, modifiers, drag) | Python | tens of events/sec |
| Applying an edit | Python calls **one** command | O(interactions) |

The load-bearing line: **pan and zoom rebuild nothing.** They set a
`QSGTransformNode` matrix. Geometry is rebuilt only when the *data* changes, or
when the viewport crosses a level-of-detail boundary (§4.4).

### 4.3 The QML scene-graph item

`PianoRollItem` is a C++ `QQuickItem` compiled into a small Qt plugin alongside
the bindings. It is C++ rather than Python because `updatePaintNode()` runs on
the Qt **render thread**, and touching Python from the render thread would take
the GIL on the frame path — the UI-side analogue of Rule 1, and just as
non-negotiable.

```cpp
class PianoRollItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QMatrix4x4 viewTransform READ ... WRITE ... NOTIFY ...)
    Q_PROPERTY(quint64 geometryRevision READ ... NOTIFY ...)
public:
    // Called from Python with the lease's buffer. Copies into a staging buffer
    // owned by this item; the copy is one memcpy, not per-vertex work.
    Q_INVOKABLE void upload(const adx::geometry::GeometryBuffer&);
protected:
    QSGNode* updatePaintNode(QSGNode*, UpdatePaintNodeData*) override;
};
```

Node tree:

```
QSGTransformNode              <- pan/zoom lives here; the ONLY thing a pan touches
  +- QSGGeometryNode  grid    <- rebuilt only on zoom LOD change
  +- QSGGeometryNode  ghosts  <- other channels' notes, dimmed
  +- QSGGeometryNode  notes   <- 6 verts/note, ColoredPoint2D
  +- QSGGeometryNode  lanes   <- velocity/pan/cutoff bars
  +- QSGGeometryNode  overlay <- selection rect, playhead, snap guides
```

Selection is a per-vertex color index, not a separate pass — selecting 500 notes
must not rebuild geometry, so `updateSelection(ids)` patches the color component
of the affected vertices in place and bumps the revision.

**The playhead is its own tiny node** driven by a 60 Hz timer reading one
atomic. It must never cause a geometry rebuild; a playhead that dirties the note
node is a 60 fps rebuild of 60,000 vertices, which is exactly the failure mode
this architecture exists to avoid.

### 4.4 `PianoRollGeometry`

```cpp
struct Viewport {
    core::Ticks tickStart, tickEnd;
    uint8_t pitchLow, pitchHigh;
    float pixelsPerTick, pixelsPerSemitone;
    float widthPx, heightPx;
};

struct BuildOptions {
    PatternId pattern; ChannelId channel;
    std::span<const NoteId> selected;
    std::span<const ChannelId> ghostChannels;
    LaneKind lane;              // Velocity | Pan | Cutoff | Resonance | Pitch
    ScaleHighlight scale;       // root + mode, or none
    uint32_t gridDivision;      // snap grid for guide lines
};

class PianoRollGeometry {
public:
    // Main thread. Fills note/grid/lane/ghost buffers. Culls to the viewport.
    void build(const Project&, const Viewport&, const BuildOptions&);
    const GeometryBuffer& notes() const noexcept;
    // ... grid(), ghosts(), lanes(), overlay()
};
```

Three things that make this fast enough:

1. **Viewport culling** with a binary search into notes sorted by start tick, so
   cost is O(visible notes), not O(all notes). A 100k-note pattern zoomed to one
   bar builds ~40 notes' worth of vertices.
2. **Level-of-detail.** Below ~2 px per note, notes render as a density strip
   rather than individual quads. This is what makes a fully zoomed-out 100k-note
   pattern cheap instead of a 600,000-vertex upload nobody can see anyway.
3. **Buffer reuse.** `build()` writes into existing storage; steady-state
   editing performs zero allocations.

### 4.5 Waveforms and `ClipPeakCache` — the port

`_archive/src-cpp/src/ClipPeakCache.cpp` is ported nearly verbatim. FINAL_PLAN
§3.1 credits it correctly: mipmap-tiered min/max peaks with invalidation keyed
on `(path, pitch, stretch, reversed)` is exactly the right structure, and it
feeds vertex buffers directly.

Changes on port:
- the invalidation key gains `sourceOffset` and `gain` (Phase 8 adds slip
  editing and clip gain, and a stale cache there is a visible wrong waveform);
- tiers are powers of 4 from 1 to 65,536 samples per peak, so any zoom level has
  a tier within 4× and no zoom level ever scans raw samples;
- peaks compute on a worker thread; the view draws the coarsest available tier
  meanwhile, so opening a project never blocks on waveform analysis.

`WaveformGeometry` emits a triangle strip of min/max pairs — 2 vertices per
pixel column, so cost is O(pixels), not O(samples). A 10-minute stereo file at
1000 px wide is 2,000 vertices regardless of length.

### 4.6 Hit testing is C++

```cpp
NoteId  hitTestNote(const Project&, PatternId, const Viewport&, float x, float y);
size_t  hitTestRect(const Project&, PatternId, const Viewport&, Rect, std::span<NoteId> out);
LaneHit hitTestLane(const Project&, PatternId, const Viewport&, float x, float y);
```

Rubber-band selection over 10,000 notes is one call returning a count and
filling a caller-provided span — not 10,000 Python comparisons. This is Rule 2
applied to input as well as output, which is the half people forget.

### 4.7 Piano roll editing — the full §5.1 surface

Tools (`tools.py`), each a small state machine over press/move/release:

| Tool | Behavior |
|---|---|
| Draw | click-drag creates a note with grid snap; drag right edge resizes; drag body moves |
| Paint | drag paints a run of notes at the grid division |
| Select | rubber band, shift-add, ctrl-toggle; arrow keys nudge |
| Slice | click splits a note at the cursor tick |
| Glue | merges adjacent selected notes on the same pitch |
| Strum | distributes a chord's onsets over a drag-defined span, forward/reverse/curved |
| Mute | toggles note mute without deleting |

Per-note lanes: velocity, pan, cutoff, resonance, fine pitch. One lane visible
at a time, edited by dragging bars; multi-select drag scales proportionally.
These are the `Note` fields Phase 2 §4.6 already defined.

Ghost notes: other channels' notes in the same pattern, dimmed, non-interactive,
built into their own buffer.

Scale highlighting and snapping: `scales.py` owns the scale table; highlighting
is a grid-buffer color index; snapping constrains drawn and dragged pitches to
the scale.

Chord tool: builds triads/sevenths/extensions from a root at the cursor. Arp
tool: converts a selected chord into an arpeggiated run (this is a *pattern
edit*, distinct from the per-channel arp of Phase 2's `Channel.arp`, and the UI
must label them differently or users will conflate them).

Quantize: strength, swing, grid division, quantize-ends option. Humanize:
seeded timing and velocity jitter — **seeded**, because an un-seeded humanize
makes a project unreproducible, which breaks golden renders.

Riff generator: seeded generation over a scale and rhythm template, emitting one
`AddNotes` command so it undoes in a single step.

**Every one of these ends in exactly one command** (Phase 2 §4.9), which is what
makes them all undoable without any per-tool undo code.

### 4.8 `engine_bridge.py` — the single chokepoint

Exactly one Python module imports `adx_engine`. Everything else imports
`engine_bridge`.

*Why:* it makes Rule 2 auditable (every FFI call is in one file, greppable and
reviewable), it gives tests one seam to mock, and it stops convenience wrappers
that return per-note lists from appearing organically in twenty panels.
Iteration one's equivalent failure (FINAL_PLAN §3.3.10) was ~15 mutable globals
in `main.cpp` interleaved with draw calls; one chokepoint is the structural
answer.

`engine_bridge` also owns: the 60 Hz `QTimer` that reads meters/playhead/scope
in one call, the command submission helpers, and error translation from engine
diagnostics to Qt signals.

### 4.9 The performance gate — what is actually enforced

FINAL_PLAN §7: *"the piano roll holds 60 fps while panning and zooming a
10,000-note pattern, measured, with a CI performance gate."* Being precise about
what CI can and cannot measure:

**Blocking CI gate — `bench_piano_roll.cpp` + `test_perf_geometry.py`.** Pure
C++ and pure FFI, headless, deterministic, no GPU:

| Measurement | Budget |
|---|---|
| `PianoRollGeometry::build`, 10k-note pattern, full view | **< 2.0 ms** |
| `build`, zoomed to 1 bar (~40 visible) | **< 0.10 ms** |
| `build` steady-state allocations | **0** |
| `hitTestRect` over 10k notes | **< 0.20 ms** |
| Python→C++→numpy round trip for 10k notes | **< 0.50 ms** |
| FFI calls per simulated pan frame | **exactly 0** |
| FFI calls per simulated meter frame | **exactly 1** |

The last two are the real architectural assertions. A pan that costs an FFI call
is an architecture violation and it is caught as a *count*, which is
deterministic and cannot flake.

**Non-blocking GPU benchmark — `test_perf_frames.py`.** Marked
`@pytest.mark.gpu`, skipped where no GPU is present. Drives real pan and zoom
over a 10,000-note pattern and records the 99th-percentile frame time with a
16.6 ms threshold. Run locally and on any self-hosted runner; its results are
recorded in the phase log.

**Why split it this way, honestly:** GitHub's Windows runners have no usable
GPU, and a frame-rate test under a software rasterizer measures the rasterizer,
not adX. A gate that cannot run is not a gate, and a flaky gate gets disabled.
So CI enforces the parts that are deterministic and architectural, and the
frame-rate number is measured where it is meaningful. The 60 fps claim in
FINAL_PLAN is verified before this phase is marked done (§6) — it is simply not
verified by GitHub Actions.

### 4.10 Shell and theming

`QMainWindow` + `QDockWidget` for the shell (real docking, tabbed docks, saved
layouts, floating panels) with `QQuickWidget` embedding QML for hot surfaces.

*Why not full QML:* Qt Quick has no docking framework, and writing one is weeks
of work to reimplement something `QDockWidget` already does correctly. Widgets
for chrome, Quick for the scene graph, is the pragmatic split — and it is the
one that keeps `QSGGeometryNode` available, which is the entire point.

Theme tokens in Python, consumed by both the widget stylesheet and the QML
palette, so a color is defined once. `ColorPalette.h` gives geometry builders
*semantic indices* (`kNoteSelected`, `kGridBar`) and the theme resolves indices
to RGBA at upload — so switching theme does not rebuild geometry.

Layout persists to `QSettings`, with a "reset layout" action, because a corrupted
saved layout that cannot be reset is a bug report per week.

---

## 5. Tests

| Test | Asserts |
|---|---|
| `geometry_buffer_revision` | revision bumps on content change, not on read |
| `geometry_zero_copy` | `np.asarray(lease)` shares memory with the C++ buffer (verified by mutating C++ and reading Python) |
| `geometry_lease_prevents_resize` | `resize()` during an active lease raises rather than reallocating |
| `geometry_no_alloc_steady_state` | 1000 rebuilds after warm-up perform zero allocations |
| `geometry_culling_correct` | visible-note set matches a brute-force reference at 20 random viewports |
| `geometry_lod_threshold` | crossing the LOD boundary changes the primitive count and nothing else |
| `peak_cache_invalidation` | changing any key field invalidates; changing nothing does not |
| `peak_cache_tier_selection` | the chosen tier is within 4× of the requested resolution at every zoom |
| `peak_cache_matches_bruteforce` | tiered peaks equal a direct min/max scan |
| `hit_test_matches_bruteforce` | 10k random points and 1k random rects agree with a reference implementation |
| `piano_roll_tools_emit_one_command` | each of the 7 tools produces exactly 1 command per gesture (table-driven) |
| `piano_roll_undo_restores` | a gesture with each tool, then undo, restores the pattern byte-identically |
| `quantize_strength` | strength 0 is a no-op; 1.0 is exact; 0.5 is exactly halfway, in ticks |
| `humanize_seeded` | the same seed produces the same result across runs and processes |
| `scale_snap` | drawing outside the scale snaps to the nearest scale degree |
| `bridge_is_only_importer` | CI check: no module except `engine_bridge.py` imports `adx_engine` |
| `no_per_note_python_objects` | CI check: no binding signature returns `list` of a note/event type |
| `gil_released_on_heavy_calls` | CI check: every non-trivial `m.def` has the release guard or a justified exemption |
| `engine_has_no_pybind_include` | CI check: no pybind11 header is reachable from `engine/` |
| `perf_geometry_budgets` | all seven §4.9 budgets |
| `perf_frames_gpu` | `[gpu]` p99 frame time < 16.6 ms panning and zooming 10k notes |

---

## 6. Definition of done

- [ ] The app launches, shows a dockable shell with a transport bar, opens
      `suffocation.adx`, plays it, and shows a moving playhead.
- [ ] The piano roll renders a real pattern with grid, ghost notes, one lane and
      scale highlighting.
- [ ] All seven editing tools work and each is one undoable command.
- [ ] Every `perf_geometry_budgets` budget met in CI, on Release.
- [ ] **The 60 fps claim verified on real hardware**, over a 10,000-note pattern,
      panning and zooming, with the p99 frame time recorded in the phase log and
      in FINAL_PLAN §10's notes column. This is a human-run acceptance step, and
      it is the one that decides whether the §2.2 bet paid off.
- [ ] Waveform view renders a 10-minute file at every zoom without stutter and
      without blocking on peak computation.
- [ ] All four Rule-1/2/3 CI checks wired and observed to fire.
- [ ] FINAL_PLAN.md §10 Phase 5 row updated.

**If the frame-rate acceptance fails**, that is a finding, not a failure to hide.
Record the measured number, identify which of the three rules is being violated
(it will be one of them), and fix it here — before Phase 6 multiplies the
problem by twenty panels. That is the entire reason this phase exists as a
separate phase.

---

## 7. Out of scope (and who owns it)

| Thing | Owner |
|---|---|
| Playlist, channel rack, mixer, browser content, automation editing, preferences, history panel | Phase 6 |
| Instrument and effect editor UIs | Phase 6 |
| The text editor panel | Phase 7 |
| Recording UI, export dialog | Phase 8 |
| Plugin windows | Phase 9 |
| Spectrum/waterfall/particle panels (the `widgets/` stubs land here; content in Phase 10) | Phase 10 |
| Session view | Phase 11 |

---

## 8. Handoff to Phase 6

Phase 6 inherits a proven pattern and copies it per panel:

1. add a geometry builder in `engine/geometry/` for anything O(data);
2. add a `QQuickItem` + `QSGGeometryNode` if the surface is hot, or a plain
   widget if it is not;
3. add the panel in `app/adx/panels/`, deriving from `DockPanel`;
4. route every FFI call through `engine_bridge`;
5. add the panel's budgets to `perf_geometry_budgets`.

Also inherited: the theme tokens, the docking shell and layout persistence, the
60 Hz frame timer (one FFI call, already reading meters and playhead), the
`GeometryLease` idiom, the reusable waveform widget (the playlist's audio clips
are the same widget at a different scale), `HitTest`, and the four CI checks —
so a Rule 2 violation in Phase 6 fails the build rather than being discovered by
profiling later.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| **Python/Qt cannot hold 60 fps and the §2.2 bet is wrong** | This is precisely why the phase exists and why it builds one surface. The acceptance step (§6) is explicit and early. If it fails, the fallback — a C++ QQuickItem taking over more of the interaction path, with Python retaining only layout and tool logic — is available *because* the geometry and hit testing are already in C++ |
| numpy views dangle and crash the app | `GeometryLease` + debug lease counter + the never-outlive-the-`with`-block rule; `geometry_lease_prevents_resize` tests it |
| `QQuickWidget` in a `QDockWidget` has known compositing quirks | Validated in the first week of the phase with a spike, before the piano roll is built on it. If it does not hold up, `QQuickView` in a container window is the fallback and the panel API absorbs the difference |
| The GPU frame gate cannot run in CI, so performance rots between releases | The deterministic budgets (§4.9) *do* run every PR and catch the causes (geometry build cost, FFI call counts) rather than only the symptom |
| Building a C++ QQuickItem plugin alongside pybind11 complicates the build | Both are CMake targets against the same Qt found by PySide6's own Qt; the spike in week one proves the toolchain before any UI is written |
| The piano roll grows into a 1,200-line file, repeating §3.3.10 | Tools are separate modules with a shared state-machine base; the 150-line function limit from Phase 0 §4.5 applies to Python via a ruff rule |
