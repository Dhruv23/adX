# Phase 1 — RT core · M

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `engine/rt/`, `engine/audio/`, the realtime-safety gate |
| **FINAL_PLAN refs** | §2.2 Rule 1, §3.1 (`AudioTap`), §3.2 (GC bin), §3.3 items 1–2, §7 Phase 1, §9 |
| **Entry criteria** | [phase_0.md](phase_0.md) §6 complete |
| **Next** | [phase_2.md](phase_2.md) |
| **§5 coverage owned** | §5.6 partial — audio device enumeration, buffer-size selection, latency readout (ASIO itself is Phase 9) |

---

## 1. Objective

Build the realtime substrate and, more importantly, **the mechanism that proves
it is realtime-safe**. Every later phase writes audio-thread code; this phase
decides whether a violation is caught by a test in 30 seconds or by a user
hearing a click in six months.

Iteration one's single worst defect (`_archive/src-cpp/src/AudioEngine.cpp:206`
and `:372` — `std::vector` constructed on every audio callback, 86 times a
second) existed because nothing could detect it. Phase 1's deliverable is not
primarily the containers. It is the detector.

At the end of this phase adX outputs silence through a real audio device, and a
test proves it did so without allocating.

---

## 2. Deliverables — exact file manifest

```
engine/rt/
  CMakeLists.txt
  RtConfig.h            kMaxBlockFrames=2048, kCacheLine=64, ADX_RT_HOT
  ThreadId.h/.cpp       registerAudioThread(), isAudioThread()
  RtSection.h/.cpp      ScopedRtSection RAII; thread-local depth counter
  Violation.h/.cpp      ViolationKind, ViolationRecord, ViolationLog (lock-free, fixed)
  AllocGuard.h/.cpp     global operator new/delete override + _CrtSetAllocHook
  LockGuardCheck.h      TryMutex wrapper; runtime "no lock on RT thread" assertion
  SpscRing.h            bounded SPSC queue, trivially-copyable T, no alloc after ctor
  OverwriteRing.h       generalized AudioTap (port of AudioEngine.h:182-209)
  FixedVector.h         fixed-capacity vector, no growth, ADX_ASSERT on overflow
  FixedString.h         fixed-capacity char buffer for RT-visible names
  BlockArena.h          bump allocator over a preallocated block; reset() per callback
  Reaper.h/.cpp         deferred reclaim (replaces m_sequenceGarbageBin)
  Denormal.h            ScopedFlushDenormals (FTZ/DAZ via _MM_SET_*)

engine/audio/
  CMakeLists.txt
  AudioBackend.h        abstract device interface
  DeviceInfo.h          POD describing one device
  StreamConfig.h        sample rate, block size, channel counts, device ids
  RtAudioBackend.h/.cpp RtAudio implementation
  NullBackend.h/.cpp    no device; callback driven by a steady_clock pacing thread
  OfflineBackend.h/.cpp no device, no clock; callback driven as fast as possible
  AudioThread.h/.cpp    owns the backend, installs RtSection + Denormal on entry

bindings/
  audio.cpp             enumerate_devices(), open_stream(), close_stream(), stream_info()

tests/cpp/rt/
  test_spsc_ring.cpp
  test_overwrite_ring.cpp
  test_fixed_containers.cpp
  test_block_arena.cpp
  test_reaper.cpp
  test_alloc_guard.cpp          <- includes the positive control
  test_stream_silence.cpp

tests/python/
  test_devices.py
```

---

## 3. Design

### 3.1 The RT section

An "RT section" is any span of execution that must obey FINAL_PLAN §2.2 Rule 1.
It is entered once, at the top of the audio callback, and is not a per-function
concept.

```cpp
namespace adx::rt {

class ScopedRtSection {
public:
    ScopedRtSection() noexcept;   // ++t_rtDepth
    ~ScopedRtSection() noexcept;  // --t_rtDepth
    ScopedRtSection(const ScopedRtSection&) = delete;
};

// Cheap enough to call from operator new. thread_local, not atomic.
[[nodiscard]] bool inRtSection() noexcept;   // t_rtDepth > 0

} // namespace adx::rt
```

`t_rtDepth` is a `thread_local int`. Reading it is a TLS access with no
synchronization, which is what makes it affordable inside `operator new`.

Compiled out entirely when `ADX_ENABLE_RT_GUARD=OFF` (Release): `inRtSection()`
becomes `constexpr false` and the constructor/destructor are empty, so the
optimizer removes them.

### 3.2 The violation log

A violation cannot `throw` (Rule: RT code never throws), cannot `printf`, and
cannot allocate. So it records.

```cpp
enum class ViolationKind : uint8_t {
    Allocation, Deallocation, Lock, Throw, Blocking, Unbounded, Assert
};

struct ViolationRecord {          // trivially copyable, 64 bytes
    ViolationKind kind;
    uint8_t       reserved[3];
    uint32_t      sizeBytes;      // for Allocation
    uint64_t      sequence;
    const char*   file;           // string literal; never owned
    uint32_t      line;
    uint32_t      returnAddrCount;
    void*         returnAddrs[4]; // captured via _ReturnAddress / RtlCaptureStackBackTrace
};

class ViolationLog {              // process-wide singleton, fixed 256 slots
public:
    void record(ViolationRecord) noexcept;  // RT-safe: index = m_count++ & 255
    uint64_t count() const noexcept;
    void reset() noexcept;                  // main thread only, between tests
    std::span<const ViolationRecord> snapshot() const noexcept;
};
```

`record()` does an atomic increment and a store into a fixed array. Nothing
else. The stack capture uses `RtlCaptureStackBackTrace`, which does not
allocate, and is compiled in only under `ADX_ENABLE_RT_GUARD` — it is the
difference between "something allocated" and "`Scheduler::dispatch` allocated",
and that difference is worth the cost in a guarded build.

### 3.3 The allocator hook — the centerpiece

Two mechanisms, because one is not sufficient:

**(a) Global `operator new` / `operator delete` replacement.** Catches every C++
allocation in every configuration, including inside the standard library and
inside dependencies compiled against the same CRT.

```cpp
// engine/rt/AllocGuard.cpp — linked into adx_engine_static only when guarded
void* operator new(std::size_t n) {
    if (adx::rt::inRtSection()) {
        adx::rt::ViolationLog::instance().record({
            .kind = ViolationKind::Allocation,
            .sizeBytes = static_cast<uint32_t>(n), ... });
    }
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc{};
    return p;
}
```

Note that it **still performs the allocation**. A guard that fails the
allocation would turn a test failure into a crash and would change the behavior
under test. The job is to observe, not to prevent.

All eight replaceable forms are provided (`new`, `new[]`, sized delete, aligned
variants, `nothrow` variants). Missing one is how a leak of coverage happens.

**(b) `_CrtSetAllocHook` on MSVC debug CRT.** Catches `malloc`/`realloc`/`free`
from C dependencies (RtAudio's internals, miniaudio, shine, libFLAC) that never
route through `operator new`. Debug-only; `operator new` replacement remains the
primary in RelWithDebInfo.

**What this does not catch, stated honestly:** allocations inside a dependency
statically linked against a *different* CRT, and OS-level allocations behind
Win32 calls. Mitigation is the clang-tidy ban list (Phase 0 §4.6) plus the rule
that no Win32 call ever appears below the binding layer.

### 3.4 Locks and blocking

`std::mutex` is banned in RT paths by clang-tidy, but third-party code and
honest mistakes slip through. Add a runtime layer:

```cpp
class RtCheckedMutex {            // used everywhere the engine needs a mutex
public:
    void lock() noexcept {
        if (inRtSection()) ViolationLog::instance().record({.kind = Lock, ...});
        m_impl.lock();
    }
    bool try_lock() noexcept;     // always legal, even on the RT thread
    void unlock() noexcept;
private:
    std::mutex m_impl;
};
```

Every engine mutex is an `RtCheckedMutex`. `std::mutex` appearing anywhere in
`engine/` outside this file is a clang-tidy error.

### 3.5 `SpscRing<T, Capacity>`

Bounded, single-producer single-consumer, wait-free on both sides, no allocation
after construction.

```cpp
template <class T, size_t Capacity>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::has_single_bit(Capacity));
public:
    [[nodiscard]] bool tryPush(const T& v) noexcept;  // false when full
    [[nodiscard]] bool tryPop(T& out) noexcept;       // false when empty
    [[nodiscard]] size_t sizeApprox() const noexcept;
private:
    alignas(kCacheLine) std::atomic<size_t> m_head{0};  // consumer owns
    alignas(kCacheLine) std::atomic<size_t> m_tail{0};  // producer owns
    alignas(kCacheLine) std::array<T, Capacity> m_slots{};
};
```

Memory ordering: producer does `relaxed` load of its own `m_tail`, `acquire`
load of `m_head`, store into slot, `release` store of `m_tail+1`. Consumer
mirrors it. Head and tail are on separate cache lines — false sharing between
the UI thread and the audio thread on a 50 µs budget is not theoretical.

*Why not `moodycamel::ReaderWriterQueue`* (which iteration one used): it grows by
allocating new blocks when full. Growth on the producer side is fine, but the
project needs a **hard bound** so that "the queue is full" is a designed,
testable state rather than a hidden malloc. One fewer dependency is a secondary
benefit. The `static_assert(std::is_trivially_copyable_v<T>)` discipline from
`_archive/src-cpp/include/AudioData.h` is kept verbatim — it was right.

### 3.6 `OverwriteRing<Frame, Capacity>` — the generalized `AudioTap`

Direct port of `_archive/src-cpp/include/AudioEngine.h:182-209`, whose memory
ordering is already correct: `Write` does a relaxed index computation, stores
the frame, then `fetch_add(1, release)`; `ReadLatest` does an `acquire` load of
the cursor and copies backwards from it. Writer never blocks, never dequeues,
and overwrites the oldest data. Readers may miss data; that is correct for
meters and scopes.

Generalized to N typed instances. Phase 1 instantiates:

| Instance | Frame type | Capacity | Consumer |
|---|---|---|---|
| master scope | `StereoFrame` | 8192 | oscilloscope, spectrum |
| master peak/RMS | `LevelFrame` | 256 | master meter |
| per-insert peak | `LevelFrame` | 256 each | mixer meters (Phase 6) |

The one correction to the archived version: it stores `size_t writePos` and
computes `available = min(count, writePos)`, which is correct but silently
assumes the cursor never wraps a 64-bit counter. Keep that assumption and
document it — at 48 kHz, `uint64_t` wraps in 12 million years.

### 3.7 `BlockArena` — the fix for defect §3.3.1

```cpp
class BlockArena {
public:
    explicit BlockArena(std::byte* storage, size_t bytes) noexcept;
    template <class T> [[nodiscard]] std::span<T> allocate(size_t n) noexcept;
    void reset() noexcept;       // called once at the top of each callback
    size_t highWaterMark() const noexcept;
private:
    std::byte* m_base; size_t m_capacity; size_t m_offset; size_t m_highWater;
};
```

Backing storage is allocated once, on the main thread, sized from
`kMaxBlockFrames` and the snapshot's declared maxima. `allocate()` bumps a
pointer with alignment; overflow is an `ADX_ASSERT` plus a recorded
`ViolationKind::Unbounded` and a null span, never a fallback to `malloc`.

`highWaterMark()` is exported so a test can assert the arena is *sized*
correctly rather than merely not overflowing today.

This is what `std::vector<ScheduledEvent>` and `std::vector<ActiveClipRef>`
become in Phase 3.

### 3.8 `Reaper` — the fix for §3.2's GC bin

Iteration one pushed retired snapshots into
`AudioEngine::m_sequenceGarbageBin` (`_archive/src-cpp/src/AudioEngine.cpp:145`),
a `std::vector<std::unique_ptr<...>>` **mutated from the audio thread** — the
instinct (never `delete` on the audio thread) was right; the mechanism was a
push_back that can reallocate.

```cpp
struct Retired {                 // trivially copyable
    void* ptr;
    void (*destroy)(void*) noexcept;   // type-erased deleter, no std::function
};

class Reaper {
public:
    [[nodiscard]] bool retire(Retired r) noexcept;  // audio thread; ring push
    size_t drain() noexcept;                        // main thread; destroys
    size_t pendingApprox() const noexcept;
private:
    SpscRing<Retired, 1024> m_queue;
};
```

`retire()` returning `false` (ring full) is a violation: it means the main
thread stopped draining. Record it; do not leak silently.

The main thread drains on a timer and unconditionally at stream stop.

### 3.9 Denormals

```cpp
struct ScopedFlushDenormals {     // FTZ + DAZ via _MM_SET_FLUSH_ZERO_MODE etc.
    ScopedFlushDenormals() noexcept; ~ScopedFlushDenormals() noexcept;
};
```

Installed once at the top of the audio callback, alongside `ScopedRtSection`.
Reverb and filter tails decaying into denormal range cost 100x on some
microarchitectures, and that shows up as a dropout under exactly the conditions
where you least want one. Iteration one had no denormal handling at all.

**Interaction with the bit-identical guarantee (Phase 0 §4.1):** FTZ/DAZ changes
results. It must therefore be applied *identically* in offline render and
realtime. `OfflineBackend` installs the same guard for this reason, and Phase 3
asserts on it.

### 3.10 Audio backend abstraction

```cpp
namespace adx::audio {

struct DeviceInfo {
    uint32_t id; FixedString<128> name; FixedString<32> apiName;
    uint32_t maxOutputChannels, maxInputChannels;
    std::span<const uint32_t> supportedSampleRates;
    bool isDefaultOutput, isDefaultInput;
};

struct StreamConfig {
    uint32_t outputDeviceId, inputDeviceId;
    uint32_t sampleRate = 48000;
    uint32_t blockFrames = 256;
    uint32_t outputChannels = 2, inputChannels = 0;
};

struct StreamInfo {               // the latency readout (§5.6)
    uint32_t sampleRate, blockFrames;
    double   outputLatencyMs, inputLatencyMs, roundTripLatencyMs;
    uint64_t xrunCount;
};

// Called on the audio thread. noexcept. Never allocates.
using RenderCallback = void (*)(void* user, float* out, const float* in,
                                uint32_t frames, const StreamTime& t) noexcept;

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    virtual std::span<const DeviceInfo> enumerate() = 0;
    virtual std::expected<void, Error> open(const StreamConfig&, RenderCallback, void* user) = 0;
    virtual std::expected<void, Error> start() = 0;
    virtual void stop() noexcept = 0;
    virtual StreamInfo info() const noexcept = 0;
};

} // namespace adx::audio
```

A raw function pointer plus `void*`, not `std::function` — `std::function` can
allocate, and its call is an indirect branch through a type-erased vtable on the
hottest path in the program.

Three implementations, and all three matter:

- **`RtAudioBackend`** — real devices (WASAPI/DirectSound now; ASIO in Phase 9,
  which is a device-API flag on the same class, not a new backend).
- **`NullBackend`** — no device. A `steady_clock`-paced thread calls the
  callback at the correct wall-clock rate and discards output. This is what CI
  uses, because GitHub's Windows runners have no audio device. Without it, the
  RT-safety gate would be untestable in CI and would therefore rot.
- **`OfflineBackend`** — no device, no clock; calls the callback as fast as it
  can. This is the offline render driver Phase 3 and Phase 8 build on, and
  having it here means offline render is *the same code path* from the
  beginning rather than a parallel one bolted on later (which is how offline and
  realtime drift apart).

### 3.11 The audio thread entry point

Exactly one place installs the guards, and every backend routes through it:

```cpp
void AudioThread::render(float* out, const float* in, uint32_t frames,
                         const StreamTime& t) noexcept {
    ScopedRtSection rt;
    ScopedFlushDenormals dn;
    ADX_ASSERT(frames <= kMaxBlockFrames);
    m_arena.reset();
    m_graph.process(out, in, frames, t);   // Phase 3 fills this in
}
```

In Phase 1, `m_graph.process` writes zeros. That is the whole deliverable: a
correctly-guarded thread producing silence.

### 3.12 Bindings added this phase

```python
adx_engine.enumerate_devices() -> list[DeviceInfo]   # dataclass-like, plain values
adx_engine.open_stream(config: StreamConfig) -> None
adx_engine.close_stream() -> None
adx_engine.stream_info() -> StreamInfo
adx_engine.rt_violation_count() -> int               # test + diagnostics surface
```

`enumerate_devices` returning a Python list is legal: it is O(devices) and
happens on user interaction, not per frame. Rule 2 (FINAL_PLAN §2.2) prohibits
O(notes) and O(frames), not O(1)-ish. Every one of these releases the GIL
(Rule 3).

---

## 4. Port map

| Archive source | Destination | Fidelity |
|---|---|---|
| `include/AudioEngine.h:182-209` (`AudioTap`) | `engine/rt/OverwriteRing.h` | Memory ordering verbatim; templated on frame type and capacity |
| `include/AudioData.h` `static_assert(std::is_trivially_copyable_v<AudioEvent>)` | `engine/rt/SpscRing.h` | Discipline kept as a `static_assert` on the ring's `T` |
| `src/AudioEngine.cpp:145` (`m_sequenceGarbageBin`) | `engine/rt/Reaper.h` | Idea kept, mechanism replaced (§3.8) |
| `CMakeLists.txt` rtaudio 6.0.1 | `cmake/AdxDependencies.cmake` | Same tag; add to the Phase 0 dependency table |
| — | `engine/rt/Denormal.h` | New. Iteration one had none. |

---

## 5. Tests

Catch2 tags: `[rt]`, `[audio]`, `[.slow]` for anything over a second.

| Test | Asserts |
|---|---|
| `spsc_push_pop_fifo` | ordering preserved over 1M items through a real second thread |
| `spsc_full_returns_false` | fills to `Capacity`, next `tryPush` is false, no growth, no alloc |
| `spsc_empty_returns_false` | `tryPop` on empty is false and does not modify `out` |
| `spsc_no_alloc_in_rt_section` | 100k pushes inside `ScopedRtSection`; violation count stays 0 |
| `overwrite_ring_latest_ordering` | after N writes, `ReadLatest(k)` returns the last k in write order |
| `overwrite_ring_partial_fill` | before first wrap, returns `min(k, written)` |
| `overwrite_ring_wraps` | writes 3x capacity; reader sees only the newest window, no torn frames |
| `overwrite_ring_concurrent` | `[.slow]` 10 s, 1 writer at 48 kHz + 1 reader at 60 Hz; every read is a contiguous non-decreasing run of a known ramp |
| `fixed_vector_overflow_asserts` | push past capacity records `Unbounded`, does not allocate, does not corrupt |
| `block_arena_bump_and_reset` | alignment honoured; `reset()` returns offset to 0; `highWaterMark` tracks the max |
| `block_arena_overflow_is_violation` | over-allocate → null span + recorded violation, never a `malloc` fallback |
| `reaper_drains_on_main_thread` | objects retired from a worker are destroyed exactly once, on the draining thread |
| `reaper_retire_does_not_free` | destructor counter is 0 until `drain()` is called |
| `reaper_full_queue_records_violation` | 1025 retires without draining → violation recorded, no leak-in-silence |
| **`alloc_guard_positive_control`** | **a deliberate `new int` inside `ScopedRtSection` IS recorded.** See §6. |
| `alloc_guard_negative_control` | the same `new int` outside an RT section is NOT recorded |
| `alloc_guard_catches_all_forms` | each of the 8 replaceable new/delete forms is observed |
| `alloc_guard_catches_std_containers` | `std::vector<int> v; v.push_back(1);` inside a section is recorded — this is the exact iteration-one defect, reproduced as a test |
| `lock_check_records_on_rt_thread` | `RtCheckedMutex::lock()` inside a section records `Lock`; `try_lock()` does not |
| `denormal_guard_sets_and_restores` | FTZ/DAZ bits set inside, restored after |
| `null_backend_60s_zero_violations` | `[.slow]` **the gate.** 60 s of NullBackend at 48 kHz / 256 frames: violations == 0, callback count within 0.1 % of expected, no callback over 3 ms |
| `offline_backend_produces_identical_silence` | OfflineBackend and NullBackend produce byte-identical output buffers |
| `rtaudio_enumerate_does_not_crash` | `[.device]` skipped when no device; asserts at least one device and a sane latency readout |
| `python/test_devices.py::test_enumerate` | returns a list; each entry has name, channel counts, sample rates |
| `python/test_devices.py::test_open_close_null` | opens the null backend, runs 1 s, closes, `rt_violation_count() == 0` |

### The positive control is the most important test in this phase

`alloc_guard_positive_control` is not a formality. Every other RT test in this
project, in every later phase, is meaningless if the hook is silently inert — a
broken hook makes 10,000 tests pass. So:

- It runs **first** in the suite (Catch2 ordering via a `[000-guard]` tag).
- If it fails, the whole suite aborts rather than continuing, because the
  remaining results would be worthless.
- It is also asserted in `Release` builds *as a skip with a printed notice*, so
  nobody mistakes an unguarded Release run for a clean one.

---

## 6. Definition of done

- [ ] `null_backend_60s_zero_violations` passes in CI, Debug and RelWithDebInfo.
- [ ] `alloc_guard_positive_control` is observed to **fail** when the hook is
      deliberately disabled, and to pass when enabled.
- [ ] `alloc_guard_catches_std_containers` reproduces iteration one's
      `AudioEngine.cpp:206` defect and catches it.
- [ ] A real device opens, runs 60 s of silence, and reports a plausible
      round-trip latency (verified manually once; the test is `[.device]`).
- [ ] `python -c "import adx_engine; print(adx_engine.enumerate_devices())"`
      lists the machine's real devices.
- [ ] `BlockArena::highWaterMark()` exported and asserted in at least one test.
- [ ] clang-tidy RT identifier ban list completed and passing over `engine/rt`.
- [ ] `.clang-tidy` observed to fire on a deliberately added `std::vector` in an
      RT path.
- [ ] FINAL_PLAN.md §10 Phase 1 row updated.

---

## 7. Out of scope (and who owns it)

| Thing | Owner |
|---|---|
| Any DSP; anything that makes sound | Phase 3/4 |
| The event *set* carried by `SpscRing` | Phase 3 (it depends on the Phase 2 project model) |
| ASIO | Phase 9 |
| Audio **input** capture and recording | Phase 8 (the backend already carries `in`; nothing consumes it yet) |
| Device-selection UI | Phase 6 |

---

## 8. Handoff to Phase 2

Phase 2 is main-thread-only work (project model, commands, parsing) and does not
consume the RT primitives directly. What it does inherit:

- `FixedString<N>` for names that must eventually be RT-visible.
- The `trivially_copyable` discipline, which constrains what the Phase 3
  snapshot may contain and therefore what the Phase 2 model must be able to
  flatten into.
- `Reaper` — Phase 2's command system publishes snapshots that Phase 3 retires
  through it.

**The constraint Phase 2 must respect:** anything reachable from the audio
thread must be flattenable into POD arrays. Phase 2 may use `std::string`,
`std::vector` and `shared_ptr` freely in the main-thread model — but it must
keep a clean line between the *editable* model and its *renderable* projection,
because Phase 3 has to build the latter without allocating during playback.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| Replacing global `operator new` breaks a dependency that replaces it too | Only `adx_engine_static` and the test binary link the guard; guard is a separate source file that can be excluded per target |
| TLS access inside `operator new` is too slow and skews timing | Measured, not assumed: a benchmark compares guarded vs unguarded callback time. Guard is off in Release regardless |
| GitHub runners have no audio device, so the gate never runs | `NullBackend` exists precisely for this and is the CI path |
| `NullBackend` clock jitter causes flaky 60 s tests | Assert on *violations* (deterministic) and on callback *count* within tolerance; assert per-callback duration only as a `[.slow]` non-blocking report |
| FTZ/DAZ makes offline and realtime differ | Both install `ScopedFlushDenormals`; Phase 3's bit-identity test would catch a regression immediately |
