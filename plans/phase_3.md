# Phase 3 — Audio graph & scheduling · L

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `engine/graph/`, `engine/transport/`, `engine/project/Snapshot.*`, `engine/render/` (core) |
| **FINAL_PLAN refs** | §2.2 Rule 1, §3.3 items 1–6, §4 commit protocol, §5.2 engine half, §7 Phase 3 **including the Phase 11 checkpoint** |
| **Entry criteria** | [phase_2.md](phase_2.md) §6 complete |
| **Next** | [phase_4.md](phase_4.md) |
| **§5 coverage owned** | §5.2 engine half (inserts, slots, sends, routing DAG, sidechain routing, PDC) · §5.1 scheduling half · §5.7 the offline-render *path* (the format matrix is Phase 8) |

---

## 1. Objective

Make the Phase 2 model produce sound, correctly, at scale, with zero audio-thread
allocations — and fix, by construction, every scheduling defect in FINAL_PLAN
§3.3 items 1 through 6.

This phase contains the single most consequential architectural decision in the
project: **time is a set of sources, not a scalar.** FINAL_PLAN §7's Phase 3
checkpoint requires it, and §2 below explains why it is non-negotiable.

At the end of this phase adX plays a project through a test-tone instrument,
renders it offline bit-identically, and does both without allocating.

---

## 2. The checkpoint — read this before writing any code

FINAL_PLAN §7 Phase 3:

> **Checkpoint — do not foreclose Phase 11.** Performance Mode (§5.11) needs *N*
> independent playback positions, not one. [...] Phase 3 must model time as a
> **set of time sources** — the arrangement playhead is simply the first one —
> even though Phases 3–10 only ever instantiate one. Concretely: no code outside
> `engine/transport/` may assume a global "current position"; scheduling takes
> its time source as a parameter.

The archived engine has `double m_currentSamplePosition`
(`_archive/src-cpp/include/AudioEngine.h:296`) and a single loop region, read
directly at 14 sites across `AudioEngine.cpp`. Every one of those sites would
have to change to support clip launching. That is what "transport rewrite"
means, and it is why Phase 11 is cheap if this is done now and expensive if it
is not.

**The mechanical rule, enforced by CI:** a grep over `engine/` outside
`engine/transport/` finds zero occurrences of any engine-owned global position.
`Scheduler::render` takes `const TimeSource&`. There is no `Transport::now()`.

Cost now: one indirection and one parameter. Cost later if skipped: the phase.

---

## 3. Deliverables — exact file manifest

```
engine/transport/
  TimeSource.h/.cpp      one independent playback position (§4.1)
  TransportSet.h/.cpp    fixed array of TimeSources; id 0 is the arrangement
  LoopRegion.h           start/end/enabled, in ticks
  PlayState.h            Stopped | Playing | Recording | Paused
  StreamTime.h           sample clock + block metadata handed to render
  Seek.h/.cpp            seek semantics: cursor invalidation + voice policy

engine/graph/
  NodeId.h
  Node.h                 the audio node interface (§4.3)
  PortSpec.h             channel counts, buffer kinds (audio / control / event)
  Graph.h/.cpp           nodes + connections; built on the main thread
  GraphBuilder.h/.cpp    Project -> Graph; owns cycle detection
  TopoSort.h/.cpp        Kahn with deterministic tie-breaking (§4.4)
  BufferPool.h/.cpp      block buffers from the Phase 1 BlockArena
  DelayLine.h/.cpp       integer-sample delay used by PDC
  Pdc.h/.cpp             latency propagation + compensation insertion (§4.6)
  Scheduler.h/.cpp       event dispatch + node execution (§4.5, §4.7)
  nodes/
    ChannelNode.h/.cpp   an instrument instance + its voice pool
    InsertNode.h/.cpp    gain/pan/mute/solo/polarity/width + slot chain
    SlotNode.h/.cpp      one effect + wet/dry + bypass
    SumNode.h/.cpp       N-in 1-out mixing point
    SendNode.h/.cpp      tap with level, pre/post fader
    MeterNode.h/.cpp     writes into a Phase 1 OverwriteRing
    TestToneNode.h/.cpp  the only instrument this phase ships (§4.9)

engine/project/
  Snapshot.h/.cpp        immutable flattened render projection (§4.2)
  SnapshotBuilder.h/.cpp Project + DirtyMask -> Snapshot, incremental
  EventTrack.h           pre-sorted per-channel events + resume cursor (§4.5)

engine/render/
  OfflineRender.h/.cpp   drives OfflineBackend through the identical graph
  RenderHash.h/.cpp      stable hash of a rendered buffer (golden tests)

bindings/
  transport.cpp          play/stop/seek/loop/tempo, position readout
  render.cpp             render_offline(project, path|buffer)

tests/cpp/graph/         test_toposort, test_cycle, test_pdc, test_buffers,
                         test_scheduler, test_voice_identity, test_routing
tests/cpp/transport/     test_timesource, test_two_sources, test_seek, test_loop
tests/cpp/render/        test_bit_identity, test_no_alloc_under_load
tests/golden/            first entries in the render-hash corpus
```

---

## 4. Design

### 4.1 `TimeSource`

```cpp
namespace adx::transport {

struct TimeSourceId { uint32_t v = 0; };            // 0 == the arrangement
inline constexpr uint32_t kMaxTimeSources = 256;    // Phase 11 clip grid ceiling

class TimeSource {
public:
    // --- audio thread: the only mutating call ---
    void advance(uint32_t frames, uint32_t sampleRate) noexcept;

    // --- audio thread: queries ---
    [[nodiscard]] core::Ticks   positionTicks() const noexcept;
    [[nodiscard]] int64_t       positionSamples() const noexcept;
    [[nodiscard]] PlayState     state() const noexcept;
    [[nodiscard]] const LoopRegion& loop() const noexcept;
    [[nodiscard]] double        rate() const noexcept;      // 1.0 normal; Phase 11 nudge
    // Frames until the next loop wrap or stop within this block, or `frames`.
    [[nodiscard]] uint32_t framesToNextBoundary(uint32_t frames, uint32_t sr) const noexcept;

    // --- main thread, applied at a block boundary via the command queue ---
    void requestSeek(core::Ticks) noexcept;
    void requestState(PlayState) noexcept;
    void requestLoop(LoopRegion) noexcept;

private:
    const core::TempoMap* m_tempo;   // borrowed from the Snapshot; never owned
    int64_t  m_positionSamples = 0;
    double   m_fractionalSample = 0.0;
    PlayState m_state = PlayState::Stopped;
    LoopRegion m_loop;
    double   m_rate = 1.0;
    uint64_t m_seekGeneration = 0;   // bumped on seek; invalidates event cursors
};

class TransportSet {
public:
    TimeSource& arrangement() noexcept { return m_sources[0]; }
    TimeSource& get(TimeSourceId) noexcept;
    TimeSourceId acquire() noexcept;   // Phase 11; returns invalid when exhausted
    void release(TimeSourceId) noexcept;
    std::span<TimeSource> active() noexcept;
private:
    std::array<TimeSource, kMaxTimeSources> m_sources{};
    std::bitset<kMaxTimeSources> m_inUse;
};

} // namespace adx::transport
```

Notes on the shape, each load-bearing:

- **Position is `int64_t` samples plus a `double` fraction**, not a bare
  `double`. A `double` sample counter loses sub-sample precision after a few
  hours at 48 kHz; more importantly, integer sample positions make the
  bit-identity test in §4.10 meaningful. Ticks are derived from samples through
  the `TempoMap`, never stored independently — two stored representations drift.
- **`m_seekGeneration`** is what makes cursor invalidation (§4.5) correct without
  the scheduler needing to know *why* the position jumped.
- **`m_rate`** exists now, unused until Phase 11's tempo nudge, because adding a
  rate multiplier to an established position-advance later means re-auditing
  every call site.
- **`kMaxTimeSources = 256` is preallocated.** `acquire()` never allocates —
  Phase 11 launches clips from the audio thread.
- **Phases 3–10 only ever touch `arrangement()`.** The rest of the array is
  inert. That is the point: the shape is right and the cost is one array.

### 4.2 The render `Snapshot` — the commit protocol

FINAL_PLAN §4:

> A command mutates the main-thread project, then publishes an immutable render
> snapshot to the audio thread via the lock-free queue. The audio thread swaps
> the pointer between blocks and hands the retired snapshot to the reaper.

```cpp
struct Snapshot {                      // POD-only; no std::string, no vector, no shared_ptr
    uint64_t revision;                 // CommandStack::revision() it was built from
    core::TempoMap tempoFlat;          // flattened: arrays + prefix sums

    std::span<const ChannelPlan> channels;
    std::span<const InsertPlan>  inserts;
    std::span<const RoutePlan>   routes;
    std::span<const uint32_t>    executionOrder;   // topologically sorted NodeIds
    std::span<const EventTrack>  eventTracks;      // one per channel, pre-sorted
    std::span<const ScheduledEvent> eventStorage;  // the arena the spans point into
    std::span<const float>       paramStorage;     // all resolved parameter values

    std::byte* arenaBase; size_t arenaBytes;       // single owning allocation
};
```

One allocation per snapshot, on the main thread, holding everything. The audio
thread receives `const Snapshot*` through a `SpscRing<const Snapshot*, 64>`,
swaps it **between blocks only**, and pushes the old pointer to the Phase 1
`Reaper`. The audio thread never dereferences the `Project`.

**Incremental rebuild.** `SnapshotBuilder::build(project, dirtyMask, previous)`
reuses spans from `previous` for subsystems the `DirtyMask` says are unchanged,
with the arena refcounted so a reused span outlives the snapshot that created
it. Dragging one note must not re-flatten the mixer graph. Budget:
**< 2 ms to rebuild after a single-note edit in a 100k-note project**, measured
in a test.

**Latency of a parameter change.** A knob turn does not rebuild a snapshot; it
posts a `ParamChange{ParamRef, float}` on the event queue, which the audio
thread applies to `paramStorage`. Structural changes rebuild; value changes do
not. This distinction is the difference between a responsive mixer and a
stuttering one.

### 4.3 `Node`

```cpp
class Node {
public:
    virtual ~Node() = default;

    // Main thread. Called once when the node enters the graph.
    virtual void prepare(const PrepareInfo&) = 0;

    // AUDIO THREAD. noexcept. No allocation, no locks, no exceptions.
    virtual void process(ProcessContext&) noexcept = 0;

    // Main thread. Samples of latency this node introduces. Used by PDC.
    [[nodiscard]] virtual uint32_t latencySamples() const noexcept { return 0; }

    virtual void reset() noexcept = 0;      // on seek/stop; clears DSP state
    [[nodiscard]] virtual PortSpec ports() const noexcept = 0;
};

struct ProcessContext {
    const transport::TimeSource& time;   // <-- THE CHECKPOINT. Not a global.
    std::span<std::span<float>> outputs;
    std::span<std::span<const float>> inputs;
    uint32_t frames;
    uint32_t sampleRate;
    EventView events;                    // events for THIS node, THIS block
    std::span<const float> params;       // resolved values, indexed by ParamRef::index
    rt::BlockArena& arena;
};
```

`ProcessContext` carrying `const TimeSource&` is the checkpoint made structural.
A node physically cannot read a global position, because there is not one to
read.

### 4.4 Graph construction and topological order

`GraphBuilder` turns a `Project` into nodes and connections:

- one `ChannelNode` per `Channel`
- one `InsertNode` per `Insert`, containing one `SlotNode` per `Slot`
- one `SendNode` per `Send`
- `SumNode`s at every convergence point
- one `MeterNode` per insert plus one on master

**Cycle detection** runs on the main thread, in `GraphBuilder`, before anything
is published. Kahn's algorithm; on failure the error carries the actual cycle
path (`insert.2 -> insert.5 -> insert.2`), because "routing cycle detected" with
no path is unactionable. A cycle is rejected at command-validation time, so an
invalid graph is never built — FINAL_PLAN §5.2's "arbitrary insert→insert
routing DAG with cycle detection".

**Deterministic tie-breaking.** Kahn's algorithm has freedom when several nodes
are ready. Break ties by ascending `NodeId`. Without this, execution order can
differ between runs, and summing floats in a different order changes the last
bits — which breaks §4.10's bit-identity guarantee and the golden hashes. This
is a two-line detail that is very expensive to discover late.

**Sidechain routing** (FINAL_PLAN §5.2, "sidechain routing as an explicit
connection") is a normal `Route` into a node's sidechain input port. It
participates in topological order like any other edge, which is exactly why it
must be modeled as an edge rather than as a side channel: a sidechain that is
not in the sort order reads last block's data, and the resulting one-block skew
is audible on a fast ducker.

### 4.5 Scheduling — fixing §3.3 items 2 and 3

Iteration one, per audio callback:

- built `std::vector<ScheduledEvent>` (`AudioEngine.cpp:206`) — an allocation;
- scanned **every note in the project** to find the few starting in this block;
- then, for each of 512 frames, linearly rescanned that vector looking for
  `ev.sampleOffset == i` (`AudioEngine.cpp:394`) — O(frames × events).

The replacement:

```cpp
struct ScheduledEvent {                  // trivially copyable, 24 bytes
    int64_t  tick;                       // absolute, snapshot-relative
    uint32_t target;                     // NodeId
    EventKind kind;                      // NoteOn/NoteOff/Param/Clip*/Mini*
    uint8_t  channel, pitch, velocity;
    uint32_t noteId;                     // <-- voice identity, fixes §3.3.4
    float    value;
};

struct EventTrack {
    std::span<const ScheduledEvent> events;   // sorted by tick, built ONCE
    uint32_t cursor;                          // resume point
    uint64_t seekGeneration;                  // must match TimeSource's
};
```

Per block, per track:

1. If `track.seekGeneration != time.seekGeneration()`, binary-search `events` for
   the current tick and reset `cursor`. **Only on seek.**
2. Walk forward from `cursor` while `events[cursor].tick < blockEndTick`,
   converting each tick to a sample offset within the block via the `TempoMap`.
3. Stop. `cursor` persists to the next block.

Amortized cost is O(events actually occurring in this block). No scan of the
project, no allocation, no per-frame rescan.

**Dispatch** is a single advancing index over the block's events, already sorted
by offset because they were sorted by tick:

```cpp
uint32_t ei = 0;
for (uint32_t i = 0; i < frames; ++i) {
    while (ei < n && evs[ei].offset == i) { apply(evs[ei]); ++ei; }
    renderOneFrame(i);
}
```

O(frames + events), not O(frames × events).

**Sub-block splitting.** Tempo changes, loop wraps and state changes split the
block at their exact sample boundary via
`TimeSource::framesToNextBoundary()`. The split points are a deterministic
function of position and tempo map — which is precisely what makes offline and
realtime render identically (§4.10). Iteration one split blocks too
(`AudioEngine.cpp:235-245`); the difference is that here the split is computed
from the `TimeSource` parameter rather than from engine-global state.

### 4.6 Plugin Delay Compensation

```cpp
class Pdc {
public:
    // Main thread. Annotates each node with the delay it must apply to align
    // all inputs at every convergence point.
    static PdcPlan compute(const Graph&);
};
```

Algorithm:

1. For every node, `latencySamples()` — 0 for everything in Phase 3, non-zero
   for lookahead limiters (Phase 4), convolution (Phase 4) and plugins
   (Phase 9).
2. Longest-path in topological order: `arrival[n] = max over inputs
   (arrival[src] + latency[src])`.
3. At each convergence point, insert a `DelayLine` of
   `arrival[n] - arrival[src]` samples on every input that arrives early.
4. Total reported latency is `arrival[master] + latency[master]`, exposed to the
   UI as the latency readout and used by Phase 8's recording alignment.

`DelayLine` is integer-sample only and preallocated to the plan's maximum.
Fractional compensation is not a thing; a sub-sample offset from a resampler is
that resampler's problem to declare as an integer.

**PDC is computed on the main thread and baked into the `Snapshot`.** The audio
thread never computes latency.

### 4.7 Voices — fixing §3.3 items 4 and 6

Iteration one matched note-offs **by MIDI pitch alone**
(`AudioEngine.cpp:1131`), so two channels playing the same pitch cross-released
each other's voices; the archived code documents this as an accepted
consequence. It is not acceptable.

```cpp
struct VoiceKey { uint32_t channelId; uint32_t noteId; };   // exact identity

class VoicePool {                      // ONE PER CHANNEL — fixes §3.3.6
public:
    explicit VoicePool(uint16_t maxPolyphony, std::span<Voice> storage) noexcept;
    Voice* allocate(VoiceKey, VoiceStealMode) noexcept;   // never nullptr
    Voice* find(VoiceKey) noexcept;                       // exact match only
    void   releaseAll() noexcept;
};
```

Voice storage is a single main-thread allocation sized from the snapshot's
summed `maxPolyphony`. `allocate()` never allocates.

**Steal order,** applied in sequence: a free voice → the oldest voice already in
release → the quietest voice → the oldest voice. A stolen voice fades out over
a fixed 2 ms ramp rather than cutting, because an instantaneous voice cut is a
click, and clicks under load are how a DAW earns a reputation.

Per-channel pools are what stop a busy pad from stealing the kick's voice.

### 4.8 Dynamic bus count — fixing §3.3.5

`kMaxEngineTracks = 16` (`_archive/src-cpp/include/AudioEngine.h:12`) silently
folded track 17 onto bus 16. There is no such constant in the rewrite. Bus count
is a field of the `Snapshot`, sized on the main thread at build time; buffers
come from the `BlockArena`, sized from the snapshot's declared maximum.

The only remaining limits are `kMaxBlockFrames` (2048) and
`kMaxTimeSources` (256), both preallocated and both documented as hard.

### 4.9 `TestToneNode` — the only instrument in this phase

A band-limited sine with a linear AR envelope, ~60 lines. It exists so that
Phase 3 can be validated end to end without waiting for Phase 4, and it stays
permanently as the instrument used by the scheduler's sample-accuracy tests —
its output is analytically predictable, which no real synth's is.

### 4.10 Offline == realtime, bit for bit

FINAL_PLAN §7 Phase 3: *"a synthetic 200-track / 100k-note project renders
offline bit-identically to its realtime capture."*

The conditions that make this achievable, all of which must hold:

1. **Same code path.** `OfflineRender` drives the Phase 1 `OfflineBackend` into
   the same `AudioThread::render` → `Scheduler` → `Graph`. There is no separate
   offline renderer. This is the architecture FINAL_PLAN §3.1 credits
   `_archive/src-cpp/src/ExportRenderer.cpp` with getting right, kept verbatim.
2. **Same block size.** Offline uses the realtime block size; it is a parameter
   of the render, not a fixed 4096.
3. **Same denormal mode.** `OfflineBackend` installs `ScopedFlushDenormals`
   (Phase 1 §3.9). FTZ/DAZ changes results, so it must be on in both or off in
   both.
4. **Same execution order.** Guaranteed by §4.4's deterministic tie-breaking.
5. **No FMA contraction, no fast-math.** Phase 0 §4.1, project-wide.
6. **No time-based or thread-based nondeterminism.** No `rand()`, no
   `steady_clock` in DSP; every stochastic element (noise, mini-notation
   probability) is a seeded xorshift32 stored in the snapshot. This is the same
   discipline `_archive/src-cpp/src/PatternCompiler.cpp` already applies and
   which its header documents as required for hot reload and export to agree.
7. **Same voice-steal decisions.** Steal order must not depend on wall-clock
   time — it depends on voice age in samples, which is deterministic.

The realtime capture for the test runs on `NullBackend`, so it is deterministic
and runs in CI.

**`RenderHash`** is a 128-bit FNV-1a over the raw float bytes of the rendered
buffer. It is the mechanism behind FINAL_PLAN §9's golden-hash tests, and the
corpus starts here rather than in Phase 8 — a golden corpus that starts late has
no history to protect.

### 4.11 Seek semantics

Written down because it is the classic source of stuck notes:

On seek, for the affected `TimeSource`:
1. bump `m_seekGeneration` — every `EventTrack` re-binary-searches on its next
   block;
2. send note-off to **every** voice owned by nodes driven by this time source
   (release, not cut — a hard cut is a click);
3. `reset()` DSP state only on nodes whose output is time-position-dependent
   (delays keep their tails during a seek-while-playing, because cutting a
   reverb tail on a seek sounds broken);
4. audio clips reposition by recomputing their read offset from the new tick,
   never by scrubbing.

Seek while stopped skips step 2. Loop wrap is **not** a seek: the cursor is
already positioned by the sub-block split, so no invalidation occurs and voices
sustain across the wrap.

### 4.12 Bindings

```python
t = engine.transport
t.play(); t.stop(); t.seek_ticks(n); t.set_loop(start, end, enabled)
t.position_ticks()          # arrangement source only, in this phase
t.state()

engine.set_project(project)              # builds + publishes a snapshot
engine.render_offline(path, start, end, block_frames) -> RenderStats
engine.rt_violation_count()
```

`position_ticks()` reads an atomic the audio thread stores once per block. It is
a Rule-2-legal O(1) read, polled at 60 Hz by the Phase 5 UI.

---

## 5. Port map

| Archive source | Destination | Fidelity |
|---|---|---|
| `src/ExportRenderer.cpp` architecture | `engine/render/OfflineRender.cpp` | Verbatim idea — render through a fresh engine on the same DSP path. Format encoders are Phase 8 |
| `src/AudioEngine.cpp:235-245` sub-block splitting | `Scheduler` | Idea kept; the split boundary now comes from the `TimeSource` parameter, not engine state |
| `src/AudioEngine.cpp:206,372` | — | **Deleted.** These are the allocations. Replaced by `BlockArena` |
| `src/AudioEngine.cpp:394` per-frame rescan | — | **Deleted.** Replaced by the advancing cursor |
| `src/AudioEngine.cpp:1131` `handleNoteOff` pitch matching | `VoicePool::find(VoiceKey)` | **Replaced.** Exact `(channelId, noteId)` |
| `include/AudioEngine.h:12` `kMaxEngineTracks` | — | **Deleted.** No equivalent constant exists |
| `include/AudioEngine.h:296` `m_currentSamplePosition` | `TimeSource::m_positionSamples` | Moved behind the transport boundary; no global read site survives |

---

## 6. Tests

| Test | Asserts |
|---|---|
| `toposort_deterministic` | 1000 shuffled builds of the same graph produce byte-identical execution orders |
| `toposort_diamond` | a diamond routes each node exactly once, in a valid order |
| `cycle_detected_with_path` | a 3-insert cycle is rejected and the message names all three |
| `cycle_via_sidechain` | a sidechain edge closing a cycle is caught like any other edge |
| `pdc_aligns_parallel_paths` | two paths with 0 and 512 samples of latency arrive sample-aligned at the sum; verified by cross-correlating an impulse |
| `pdc_reports_total_latency` | reported latency equals the longest path |
| `pdc_zero_latency_inserts_nothing` | no `DelayLine` is created when all latencies are 0 |
| `scheduler_sample_accurate_offsets` | 200 notes at hand-computed tick positions produce onsets at exactly the expected sample offsets, across 3 tempos and 2 block sizes |
| `scheduler_tempo_change_midblock` | a tempo change inside a block splits it at the exact sample; onsets after the change match the new tempo |
| `scheduler_cursor_is_amortized` | with 100k events, per-block event work is O(events in block): measured event-comparison count stays flat as project length grows 100x |
| `scheduler_no_rescan_on_loop_wrap` | a loop wrap causes zero binary searches (counter asserted at 0) |
| **`two_time_sources_independent`** | **the checkpoint gate.** Two `TimeSource`s at different positions, different tempos, different loop regions, driven through one `Scheduler` in one block; each produces its own hand-computed onsets, neither affects the other. Repeated with one looping and one not. |
| `no_global_position_grep` | CI script: zero engine-global position symbols outside `engine/transport/` |
| `voice_identity_cross_channel` | two channels play MIDI 60; note-off on channel A releases only A's voice. **This is iteration one's §3.3.4 defect, reproduced as a test** |
| `voice_pool_per_channel_isolation` | a 64-voice pad channel at full polyphony does not reduce a kick channel's available voices |
| `voice_steal_order` | free → oldest-released → quietest → oldest, table-driven |
| `voice_steal_ramps` | a stolen voice's output is continuous (no sample-to-sample discontinuity > 0.05) |
| `bus_count_dynamic` | a 200-insert project renders with 200 distinct buses; insert 17 is not folded onto 16 |
| `routing_arbitrary_dag` | insert→insert→insert chains, fan-out, fan-in and sends all sum correctly against hand-computed expectations |
| `sends_pre_post_fader` | a pre-fader send is unaffected by the source fader; a post-fader send tracks it |
| **`render_bit_identical_200x100k`** | **the phase gate.** A synthetic 200-channel / 100k-note project: realtime capture on `NullBackend` vs `OfflineRender` → identical `RenderHash`. Run at block sizes 64, 256 and 1024 |
| `render_no_alloc_under_load` | the same render under the Phase 1 allocator hook: violation count 0 |
| `arena_high_water_under_load` | `BlockArena::highWaterMark()` stays under 80 % of capacity during the 200×100k render |
| `snapshot_incremental_budget` | a single-note edit on a 100k-note project rebuilds in < 2 ms and reuses the mixer spans |
| `snapshot_swap_between_blocks` | 10,000 snapshot publishes during playback produce zero torn reads and zero dropouts |
| `reaper_reclaims_snapshots` | after 10,000 publishes, retired snapshots are all destroyed and memory is flat |
| `param_change_without_rebuild` | turning a knob 1000 times triggers zero snapshot rebuilds |
| `seek_releases_not_cuts` | after a seek, sounding voices decay over their release rather than stopping at a discontinuity |
| `seek_no_stuck_notes` | 1000 random seeks during playback leave zero active voices once stopped |
| `loop_wrap_sustains_voices` | a note crossing the loop point is not retriggered or cut |
| `golden_corpus_stable` | the corpus hashes are identical across three consecutive clean builds |

---

## 7. Definition of done

- [ ] `render_bit_identical_200x100k` passes at all three block sizes.
- [ ] `render_no_alloc_under_load` reports zero violations.
- [ ] `two_time_sources_independent` passes.
- [ ] `no_global_position_grep` is wired into CI and observed to fail when a
      global position is deliberately introduced.
- [ ] `voice_identity_cross_channel` passes (§3.3.4 closed).
- [ ] `bus_count_dynamic` passes with 200 inserts (§3.3.5 closed).
- [ ] `voice_pool_per_channel_isolation` passes (§3.3.6 closed).
- [ ] `scheduler_cursor_is_amortized` passes (§3.3.2 and §3.3.3 closed).
- [ ] `arena_high_water_under_load` passes (§3.3.1 closed).
- [ ] `tests/golden/` contains at least 5 fixtures with committed hashes.
- [ ] `python -c` script: load `suffocation.adx`, render 30 s offline to a WAV,
      play it back — it makes sound (test tones, not the real patch; Phase 4
      makes it sound right).
- [ ] FINAL_PLAN.md §10 Phase 3 row updated, and the §3.3 items 1–6 marked
      closed in a short note.

---

## 8. Out of scope (and who owns it)

| Thing | Owner |
|---|---|
| Any real instrument or effect DSP | Phase 4 |
| Audio clip playback from files (the graph has the node shape; decoding does not exist) | Phase 4 decode, Phase 8 editing |
| Export file formats | Phase 8 |
| Any UI | Phase 5 |
| Audio **input** | Phase 8 |
| Actually instantiating a second `TimeSource` | Phase 11 — this phase only proves it works |

---

## 9. Handoff to Phase 4

Phase 4 inherits:

- `Node` — implementing an instrument or effect means one class, one file,
  `prepare` / `process` / `latencySamples` / `reset`.
- `ProcessContext` with pre-resolved `params`, block-local buffers from the
  arena, and `EventView` already filtered to this node and this block. A Phase 4
  instrument never parses, never looks up by string, never allocates.
- `VoicePool` — voice allocation, stealing and identity are solved; an
  instrument implements one voice's DSP, not the pool.
- `Pdc` — a Phase 4 effect with lookahead declares `latencySamples()` and
  compensation happens automatically.
- `RenderHash` + the golden corpus — every Phase 4 DSP change is immediately
  checked against it, which is what makes porting the archived synth safe.
- `TestToneNode` as the reference implementation to copy.

**The constraint Phase 4 must respect:** `process()` is `noexcept`, allocation-
free, and reads its time from `ctx.time`. Any effect needing history buffers
allocates them in `prepare()`, sized from `PrepareInfo::maxBlockFrames`.

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| Bit-identity fails for a subtle reason (denormals, ordering, FMA) | §4.10 enumerates all seven conditions. The test runs at three block sizes, which catches block-size-dependent bugs that a single size hides |
| Incremental snapshot rebuild is a correctness hazard (stale spans) | Arena refcounting plus a debug mode that rebuilds everything every time and asserts the incremental result is byte-identical |
| PDC longest-path is wrong on graphs with sends that also feed forward | Sends are edges in the same DAG; the diamond and fan-in tests cover it; `pdc_aligns_parallel_paths` verifies by cross-correlation rather than by inspection |
| The time-source parameter is threaded everywhere and feels like overhead | It is one reference in `ProcessContext`. The CI grep makes the discipline mechanical rather than a matter of vigilance |
| 200×100k synthetic project is slow enough to make CI painful | Tagged `[.slow]`; runs on every PR in Release, nightly in Debug |
