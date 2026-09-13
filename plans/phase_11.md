# Phase 11 — Live performance · L — **the final stage**

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `engine/session/`, the session view, launch scheduling, pad mapping, performance recording |
| **FINAL_PLAN refs** | §1 (live performance is in scope), §5.11 in full, §7 Phase 3 checkpoint, §7 Phase 11, §10 |
| **Entry criteria** | [phase_10.md](phase_10.md) §7 complete — **including `bench_full_project`**, which is a hard prerequisite, not a soft one |
| **Next** | none. This is the last phase. |
| **§5 coverage owned** | §5.11 **all** |

---

## 1. Objective

Turn a finished DAW into an instrument you can play.

FINAL_PLAN §7 explains why this is last, and the reasoning is worth keeping in
front of you while building it:

> Last on purpose: it is the only feature that depends on *all* of the others
> being finished and stable. Session view needs the playlist (6) to have
> something to launch and to record back into; pad triggering needs MIDI in and
> MIDI learn (9); launchable mini-notation clips need the text layer (7); and
> performing on an engine that still drops out under load is pointless, so it
> needs Phase 10's optimization pass behind it.

And why it is cheap despite being last:

> Because Phase 3's checkpoint already made time a set of sources, this phase is
> mostly composition rather than surgery.

That claim is tested in the first week (§3). If it turns out to be false, that
is the single most important thing to discover on day one of this phase.

---

## 2. Deliverables — exact file manifest

```
engine/session/
  SessionModel.h          Scene[] x Track[] -> ClipSlot; part of Project
  ClipSlot.h              a launchable: PatternRef | AudioClipRef | MiniSource
  ClipState.h             the per-clip state machine (§4.2)
  LaunchQuantize.h        Instant | 1/4 | 1/2 | Bar | 2Bar | 4Bar | 8Bar
  FollowAction.h          None | Stop | Next | Previous | First | Random | RepeatN
  LaunchScheduler.h/.cpp  quantized trigger resolution (§4.3)
  SessionEngine.h/.cpp    owns N TimeSources; drives clip playback (§4.4)
  ClipRelease.h/.cpp      voice + effect-tail handling on stop (§4.5)
  SceneLauncher.h/.cpp    fire a column; scene-level quantize and follow
  TempoNudge.h/.cpp       nudge, tap tempo, and their interaction with the map

engine/record/
  PerformanceRecorder.h/.cpp   captures fired clips back to the playlist (§4.6)

engine/midi/
  PadController.h/.cpp    device profiles, velocity-sensitive triggering
  PadFeedback.h/.cpp      LED colour/state out, where the device supports it
  profiles/               *.adxpad — declarative device maps (§4.7)

engine/geometry/
  SessionGeometry.h/.cpp  the clip grid: state colours, progress rings, queue marks

app/adx/panels/session/
  panel.py                the dock; grid + scene column + master controls
  view.qml                QQuickItem host
  SessionItem.h/.cpp      QQuickItem + QSGGeometryNode
  slot_editor.py          per-clip: quantize, loop, follow action, legato
  pad_setup.py            device selection, mapping, feedback test

app/adx/panels/
  transport_bar.py        (extended) tap tempo, nudge, global launch quantize

docs/
  performance.md          setting up a controller and performing a set

tests/cpp/session/        test_state_machine, test_launch_quantize, test_scenes,
                          test_follow_actions, test_release, test_recorder
tests/python/             test_session_panel.py, test_pad_mapping.py
```

---

## 3. Week one — validate the checkpoint before building on it

Before any session UI exists, write this spike:

1. Acquire a second `TimeSource` from `TransportSet` (Phase 3 §4.1).
2. Start it at a different position, at a different rate, with its own loop.
3. Schedule one pattern against each source, simultaneously, through the
   existing `Scheduler`.
4. Render 30 seconds offline and assert both patterns appear at their own
   hand-computed positions.

Phase 3's `two_time_sources_independent` test already asserts most of this, so
the spike should be an afternoon. **If it is not** — if anything outside
`engine/transport/` turns out to assume a single position after all — stop and
fix that first, before writing a line of session code. The whole economics of
this phase rests on the checkpoint having held, and finding out in week six
instead of week one is the expensive version.

---

## 4. Design

### 4.1 The session model

```cpp
struct ClipSlot {
    ClipSlotId id;
    std::optional<std::variant<PatternRef, AudioClipRef, MiniSource>> content;
    core::Ticks     length;            // loop length; may differ from content length
    bool            loop = true;
    LaunchQuantize  quantize = LaunchQuantize::Bar;   // inherits global if unset
    FollowAction    follow = FollowAction::None;
    uint8_t         followRepeats = 1;
    bool            legato = false;    // keep phase when replacing a playing clip
    float           gain = 1.0f;
    Color           color;
};

struct SessionTrack {                   // one column; mirrors a playlist track
    PlaylistTrackId linked;             // where performance recording writes
    std::vector<ClipSlot> slots;        // one per scene row
    ChannelId channel;                  // or InsertId for audio tracks
    bool stopAllOnSceneFire = true;
};

struct Scene { SceneId id; std::string name; std::optional<double> tempo; Color color; };

struct SessionModel {
    std::vector<Scene> scenes;
    std::vector<SessionTrack> tracks;
    LaunchQuantize globalQuantize = LaunchQuantize::Bar;
};
```

`SessionModel` is part of `Project`, serialized into `.adx` as a `[SESSION]`
section, using Phase 2's format machinery unchanged. A performance setup is
therefore text, diffable and scriptable, which is entirely consistent with the
premise of the project — and means you can write a set in a text editor.

**One track column corresponds to one playlist track.** That correspondence is
what makes performance recording (§4.6) natural rather than a translation layer,
and it is worth preserving even though it constrains the model slightly.

### 4.2 The clip state machine

```
        launch                  quantize boundary
Stopped ------> Queued ------------------------------> Playing
   ^               |  relaunch/stop before boundary        |
   |               v                                        | stop
   +---------- (cancelled)                                  v
   |                                                    QueuedStop
   |                       quantize boundary                 |
   +---------------------------------------------------------+
                                    |
                     clip end + follow action -> Stopped | Queued(other)
```

Five states: `Stopped`, `Queued`, `Playing`, `QueuedStop`, `Retriggered`.

`Retriggered` is separate from `Queued` because a clip that is *already playing*
and re-launched must either restart at the boundary (default) or continue in
phase (`legato`), and conflating that with a cold start produces the classic
"retrigger kills my legato bassline" bug.

Every transition happens **on the audio thread**, at an exact sample offset
computed by `LaunchScheduler`. State is published to the UI through a Phase 1
`OverwriteRing` — the UI never polls the state machine, and the state machine
never waits for the UI.

### 4.3 `LaunchScheduler`

```cpp
class LaunchScheduler {
public:
    // MAIN thread (UI click) or AUDIO thread (pad event). Lock-free either way.
    void requestLaunch(ClipSlotId, LaunchQuantize) noexcept;
    void requestStop(ClipSlotId, LaunchQuantize) noexcept;
    void requestScene(SceneId, LaunchQuantize) noexcept;

    // AUDIO thread, per block. Resolves pending requests whose boundary falls
    // inside [blockStart, blockEnd) and emits transitions at exact offsets.
    void resolve(const transport::TimeSource& master, uint32_t frames,
                 std::span<ClipTransition> out) noexcept;
};
```

The **master** time source (the arrangement's, `TimeSourceId{0}`) provides the
musical grid that quantization resolves against, even when the arrangement is
not playing. Each launched clip then runs on *its own* time source. That
separation is the whole design: one shared grid, N independent positions.

Boundary computation is integer tick arithmetic against the `TempoMap`, so it is
exact and stays exact across tempo changes. `Instant` resolves at the next
sample, which is the only unquantized option and is what you want for
one-shots.

Requests are held in a fixed-capacity, preallocated table keyed by
`ClipSlotId` — one pending request per slot, later requests replacing earlier
ones. That is both correct (mashing a pad should queue the last intent, not a
backlog) and allocation-free.

### 4.4 `SessionEngine` and time sources

```cpp
class SessionEngine {
public:
    void process(const SnapshotSession&, uint32_t frames,
                 transport::TransportSet&, Scheduler&) noexcept;
private:
    std::array<ClipRuntime, kMaxConcurrentClips> m_runtime;  // preallocated
};

struct ClipRuntime {
    ClipSlotId slot;
    transport::TimeSourceId time;     // acquired on launch, released on stop
    ClipState state;
    uint64_t  launchGeneration;
};
```

On launch: `TransportSet::acquire()` (Phase 3 §4.1 — preallocated, never
allocates), position set to the clip's start, loop set to the clip's length,
state to `Playing`. On stop: release the source back to the pool after the
release tail completes.

`kMaxConcurrentClips = 128`. Exhaustion is a recorded condition with a UI
indication, never a silent failure to launch — a pad that does nothing with no
explanation is the worst possible live failure mode.

Because `ProcessContext` already carries `const TimeSource&` (Phase 3 §4.3),
**no instrument, effect or scheduler code changes at all** to support this. That
is the checkpoint paying off, and it is the specific thing §3's spike verifies.

Mini-notation clips work because `PatternCompiler` (Phase 7) takes `cycleIndex`
as a parameter — each clip's cycle counter derives from its own time source, so
`<a b>` and `every 4` advance per-clip rather than globally. This is why
FINAL_PLAN §5.11 says live mini-notation "joins §5.8 rather than duplicating
it": no new compiler work is needed.

### 4.5 Release, not cut — the one genuinely new engine concept

FINAL_PLAN §7 identifies this as the only novel engine work in the phase:

> The one genuinely new engine concept is voice and effect-tail handling across a
> clip stop — a launched clip that stops must release rather than cut, which the
> archived engine's note-off handling already gets right and can be reused.

When a clip stops:

1. Every voice owned by that clip receives a note-off and enters its release
   stage. Voices are identified by `(ChannelId, NoteId)` from Phase 3 §4.7, so a
   stopping clip releases *its* voices and not another clip's on the same
   channel — which is precisely the defect (§3.3.4) Phase 3 fixed, now load-
   bearing rather than merely correct.
2. The clip's time source keeps advancing until the longest release completes,
   then is released back to the pool. A time source freed while voices still
   sound is a stuck or cut note.
3. Effect tails on the clip's channel are **not** cut. If the channel is shared
   with another playing clip, nothing happens to the effects at all.
4. If the channel becomes idle, its effects continue processing silence until
   their tails decay below −90 dBFS, then idle out. Cutting a reverb tail on
   clip stop is the single most recognizable sound of a badly built clip
   launcher.

`ClipRelease` owns this bookkeeping. It is roughly 150 lines and it is the part
of this phase most worth writing carefully.

### 4.6 Performance recording

```cpp
class PerformanceRecorder {
public:
    void start(core::Ticks at) noexcept;      // audio thread: begins logging
    void stop() noexcept;
    // MAIN thread: converts the log into ONE undoable command that writes
    // ordinary PlaylistItems into the linked playlist tracks.
    std::unique_ptr<Command> commit(Project&) const;
};
```

The audio thread logs `(timeSamples, ClipSlotId, event)` into a preallocated
ring. On commit, the log becomes ordinary `PlaylistItem`s on each track's linked
playlist track, with exact start ticks and lengths, plus tempo-nudge changes
written into the `TempoMap`.

**The output is an ordinary arrangement.** No special "recorded performance"
entity exists. That is what makes FINAL_PLAN's gate achievable — *"the recorded
performance reopens as an editable arrangement that renders identically to what
was heard"* — and it reuses Phase 8's take machinery for any audio recorded
alongside.

"Renders identically" is testable precisely because every stochastic element is
seeded (mini-notation probability, humanize, round-robin, generators) — Phase 3
§4.10 condition 6, now paying off in a place it was not written for.

### 4.7 Pad controllers

Device profiles are declarative, in the same format family as everything else:

```
[PAD_PROFILE]
NAME="Launchpad Mini MK3"
MATCH_DEVICE="LPMiniMK3 MIDI"
GRID=8x8
NOTE_BASE=81 ROW_STRIDE=-10
VELOCITY_SENSITIVE=yes
[FEEDBACK]
MODE=sysex_rgb
STOPPED=#101010  QUEUED=#FFB000,blink  PLAYING=#00FF40,pulse
QUEUED_STOP=#FF4000,blink  EMPTY=#000000  RECORDING=#FF0000,pulse
[CONTROLS]
SCENE_LAUNCH=row:8  STOP_ALL=cc:19  TAP_TEMPO=cc:20
```

Shipping profiles for the common grid controllers, plus a **learn mode** that
builds a profile by pressing pads in order — because there are hundreds of
controllers and shipping a profile for each is not a plan.

Velocity-sensitive triggering maps pad velocity to clip gain when enabled.

**LED feedback** is generated from the clip state ring on a 30 Hz timer on a
dedicated thread — not the audio thread (Rule 1: MIDI sysex output is a blocking
syscall) and not the UI thread (feedback must not stutter when a dialog is open).
30 Hz is enough for a blinking queued indicator to read correctly and is well
under the sysex throughput of every device that supports it.

### 4.8 Tempo nudge and tap tempo

Nudge temporarily scales `TimeSource::m_rate` (Phase 3 §4.1 — the field that has
existed and been unused since Phase 3, for exactly this) on the master source,
with slaved clip sources following. Tap tempo averages the last 4 taps with
outlier rejection.

Both write into the `TempoMap` when the performance recorder is running, so the
recorded arrangement reproduces the tempo the audience actually heard.

### 4.9 The session panel

The same geometry pipeline as every other hot surface (Phase 5 §4.3):
`SessionGeometry` → `GeometryBuffer` → `SessionItem` → `QSGGeometryNode`.

Per slot: name, colour, state, a progress ring showing position within the
clip's loop, a queue indicator, and a level indicator (reusing Phase 10's meter
rings). A 8×64 grid is 512 slots updating at 60 Hz — one ring read, one geometry
build, one upload per frame, exactly like the mixer.

Alongside the arrangement, not instead of it: the session view is a dock, and
the playlist stays open. Clips can be dragged between the two, and dragging a
playlist clip into a session slot is one command.

---

## 5. Tests

| Test | Asserts |
|---|---|
| `spike_two_sources_render` | §3's week-one spike, kept as a permanent test |
| `state_machine_transitions` | all 5 states × all triggers, table-driven, exhaustive |
| `launch_quantize_exact` | for each quantize setting and each of 4 tempos, the transition lands on the exact hand-computed sample |
| `launch_across_tempo_change` | a launch queued before a tempo change fires at the musically correct moment |
| `relaunch_before_boundary` | replaces the pending request rather than queueing a second |
| `relaunch_legato_keeps_phase` | with legato on, position is continuous; with it off, it restarts |
| `stop_quantized` | `QueuedStop` stops on the boundary, not immediately |
| `instant_launch_next_sample` | `Instant` fires within 1 sample |
| `scene_fires_column` | all clips in a scene start on the same sample; empty slots stop their track when configured |
| `scene_tempo_change` | a scene with a tempo applies it at the fire boundary |
| `follow_actions` | each of the 7 actions, including `RepeatN` counting correctly |
| `follow_random_seeded` | random follow is reproducible from the project seed |
| **`clip_stop_releases_not_cuts`** | **the core correctness test.** On stop, voices decay through their release; no sample-to-sample discontinuity > 0.01 |
| `clip_stop_preserves_other_clips_voices` | two clips on one channel; stopping one leaves the other's voices sounding |
| `clip_stop_preserves_effect_tail` | a reverb tail continues for its full decay after the clip stops |
| `time_source_released_after_tail` | the source returns to the pool only once the last voice is silent |
| `concurrent_clip_limit` | launching a 129th clip is reported, not silently dropped |
| `session_no_alloc` | 10 minutes of continuous launching/stopping under the allocator hook: 0 violations |
| `mini_clip_cycles_independently` | two mini clips with `<a b>` advance their own cycle counters |
| `pad_profile_parse` | every shipped profile loads; an unknown key warns and is preserved |
| `pad_learn_builds_profile` | a scripted press sequence produces a correct profile |
| `pad_velocity_to_gain` | velocity maps to clip gain per the configured curve |
| `pad_feedback_thread` | LED output runs on its own thread; 0 audio-thread MIDI writes; 30 Hz maintained under UI load |
| `tempo_nudge_rate` | nudge scales rate and returns exactly; no accumulated drift after 100 nudges |
| `tap_tempo_outliers` | one bad tap in four does not move the tempo more than 2 % |
| **`performance_record_roundtrip`** | **the phase gate, part 1.** A scripted 2-minute performance (40 launches, 6 scenes, follow actions, 3 tempo nudges) is recorded, committed, reopened, and renders **bit-identically** to the captured live output |
| `performance_record_is_ordinary` | the committed result contains only ordinary `PlaylistItem`s and `TempoMap` events — no session-specific entities |
| `performance_record_one_command` | commit is a single undo entry |
| `session_geometry_budget` | 8×64 grid: one FFI call and one build per frame, within budget |
| `session_serializes` | `[SESSION]` round-trips losslessly through Phase 2's writer |
| **`live_set_no_dropouts`** | **the phase gate, part 2.** 30 minutes of continuous performance on the Phase 10 benchmark project: 0 dropouts, 0 allocator violations, p99 UI frame < 16.6 ms |

---

## 6. Definition of done

FINAL_PLAN §7's gate, unpacked:

> **Done when** a set can be performed end to end from a hardware pad controller
> — clips launched and stopped on quantized boundaries, scenes fired, follow-
> actions honoured — with no dropouts under the Phase 1 allocator hook, and the
> recorded performance reopens as an editable arrangement that renders
> identically to what was heard.

- [ ] `performance_record_roundtrip` passes **bit-identically**.
- [ ] `live_set_no_dropouts` passes for 30 minutes.
- [ ] `clip_stop_releases_not_cuts` passes.
- [ ] `session_no_alloc` passes.
- [ ] **A human has performed a real set** on real hardware, for at least 20
      minutes, and reported no glitches — recorded in the phase log. Every other
      criterion is a proxy for this one, and this one is the claim being made.
- [ ] Every §5.11 item exists: session view, per-clip positions, quantized launch
      with queued-next indicator, scenes, follow actions, pad mapping with
      velocity and LED feedback, performance recording, tempo nudge, tap tempo,
      launchable mini-notation clips.
- [ ] `docs/performance.md` written and its setup walkthrough verified on a real
      device.
- [ ] FINAL_PLAN.md §10 Phase 11 row set to Done.

---

## 7. When this phase is finished

Every item in FINAL_PLAN §5.1 through §5.11 is complete. FINAL_PLAN §10's status
table reads Done on all twelve rows, and the repository is what §1 set out to
build:

> a music production environment with the feature surface of FL Studio, built
> from scratch, whose project format is a human-readable, hot-reloadable,
> git-diffable text file.

There is no Phase 12. Further work is maintenance, platform ports (the engine
has stayed portable C++20 with no Win32 in it, per §1's non-goals, so this is
work rather than a rewrite), and whatever the plan then says — at which point
FINAL_PLAN.md is amended and a new phase file joins this directory.

---

## 8. Out of scope

| Thing | Status |
|---|---|
| Video / lighting sync (DMX, Ableton Link) | not planned; Link would be a reasonable post-11 addition and fits the time-source model cleanly |
| Multi-user / networked performance | **never** — FINAL_PLAN §1 non-goal |
| Hardware-specific deep integration beyond pad profiles | not planned; the profile format is the extension point |

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| **The Phase 3 checkpoint did not actually hold** | §3's week-one spike finds out on day one rather than week six. If it failed, the remediation is a transport refactor that is still far smaller than it would have been without the checkpoint, because `ProcessContext` already carries the time source by reference |
| Release-not-cut bookkeeping has edge cases that produce stuck notes live | Three dedicated tests (`clip_stop_releases_not_cuts`, `..._preserves_other_clips_voices`, `time_source_released_after_tail`) plus the 30-minute soak, which is where stuck-voice leaks actually show up |
| Pad feedback latency makes the controller feel unresponsive | Dedicated 30 Hz thread, never the audio or UI thread; measured end to end from state change to LED |
| Performance recording is not bit-identical because something is not seeded | Every stochastic element was seeded from Phase 3 onward as a deliberate precondition (§4.10 condition 6); if one is found unseeded, that is a bug in the earlier phase and is fixed there |
| 128 concurrent clips is too few for an ambitious set | It is a constant, preallocated; raising it is a one-line change with a memory cost that is measured. Exhaustion is surfaced, never silent |
| Phase 10's optimization targets slipped and this phase inherits a slow engine | `bench_full_project` is a hard entry criterion (§ header). Starting this phase on an engine that drops out would make its gate unreachable by definition |
