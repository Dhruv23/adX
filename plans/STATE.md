# adX — STATE

**This file is the running ledger of phase completion. It is the first thing an
agent reads when picking up work, and the last thing it writes before finishing.**

## How this file works

This is a checklist of all twenty-two phases (0 through 19, 10.5 and 11.5;
12–19, the sound library, were added 2026-10-25; 10.5, agent-run testing,
2026-11-01; and 11.5, M-Vave FM-1 hardware support, 2026-11-05). The agent that
implements a phase implements it, then marks that phase's checkbox done **and,
underneath it, lists any open issues for the agent who does the next phase to
fix.**

That handoff is the entire point of this file. A phase is rarely finished
perfectly: something gets deferred, something gets stubbed, something gets
discovered that belongs to the next phase's territory. Write it down. The next
agent starts by reading the previous phase's open issues, and is responsible for
either fixing them or explicitly re-carrying them forward with a reason.

### When you finish a phase

1. Verify every box in that phase's **Definition of done** section is actually
   checked — not "should pass", *observed to pass*. A box that needs physical
   hardware counts as checked once its emulated half has passed and is marked `[~]`
   (FINAL_PLAN §9, "Hardware testing is done at the end"); add its hardware half to the
   **Final review** list at the bottom of this file. Don't ask the user to run hardware
   checks before that review. **Every other box is agent-run** (FINAL_PLAN §9, set
   2026-11-01): if a box tests the software but needs someone at the keyboard, that is a
   plan bug. Build the software path (a CLI command, Phase 10.5's driver, a capture and a
   glitch measurement) and run it. Only `(judgement)` boxes, such as listening sessions and
   pack approvals, wait for the user's own words. `tools/lint.py plans` checks this
   once Phase 10.5 lands.
2. Change the phase heading's `[ ]` to `[x]` and set **Status** to `Done` with
   the date - or `Done (hardware deferred)` if any box is `[~]`.
3. Fill in **Open issues for Phase N+1**. If there are genuinely none, write
   `none` — do not leave the placeholder text.
4. Set the same phase's row in [FINAL_PLAN.md](../FINAL_PLAN.md) §10 to Done
   with the date. Both files must agree; this file is the working ledger, §10 is
   the permanent record.
5. **Phase completion.** Per-push CI (`.github/workflows/ci.yml`) is deliberately
   partial, so a phase is not done until the parts it skips have passed too:
   - **Locally, in Debug:** the `[.slow]` suite with the whole Phase 3 gate,
     which per-push CI runs only in the optimised builds:
     `ADX_FULL_EVIDENCE=1 ctest --preset windows-x64-debug -L slow`. Record the result.
   - **In CI, the full check** (`.github/workflows/full-check.yml`): clang-tidy over
     every file in Debug and Release, and the 10-minute loader fuzz. Push the phase's
     tag to run it (`git tag phase-N && git push origin phase-N`; the tag goes on the
     commit that marks the phase done), and record the run id.

   There is no nightly run. Per push, clang-tidy checks only the files a change can
   reach, and a change touching only `plans/` or Markdown runs no CI at all (except
   `docs/adx-format-v2.md`, which a test reads). Set 2026-10-02, at the user's
   request: per-push runs had reached ~30 minutes, half of it full clang-tidy.

6. **The launcher runs the latest build.** `adX.cmd` in the repository root is how the
   user opens adX (double-click; drop a `.adx` on it to open that project). It runs the
   code as last installed, so finish every phase - and any session that changes code -
   with a fresh build and the launcher check, and record the check's line here:

   ```
   py -3.12 -m pip install -e .[dev]
   py -3.12 tools/check_launcher.py
   ```

   The check opens the app through `adX.cmd` exactly as a double-click does and fails
   unless the whole window comes up. Set 2026-10-20, at the user's request.

### When you start a phase

1. Read this file top to bottom. Read the previous phase's open issues first.
2. Confirm the entry criteria on the row below are met. If they are not, stop
   and say so rather than building on an unfinished foundation.
3. Set **Status** to `In progress` with the date.
4. For each inherited open issue, either fix it during your phase or re-carry it
   into your own open-issues list with a reason. Silently dropping one is the
   failure mode this file exists to prevent.

### Writing a good open issue

Give each one a stable id (`P3-1`, `P3-2`, …), say what is wrong, say what it
blocks, and say what "fixed" looks like. One line of real detail beats a
paragraph of hedging.

```
- **P3-2** · PDC is computed but not applied to sends, so a send with a
  latent effect drifts from its dry path. Blocks §5.2 acceptance.
  Fixed when: test_pdc_send_alignment passes with non-zero send latency.
```

Mark severity when it is not obvious: `BLOCKER` (the next phase cannot start),
`CARRIED` (re-inherited from an earlier phase, still open), or leave it plain
for ordinary follow-up work.

### What this file is not

It owns **no scope**. [FINAL_PLAN.md](../FINAL_PLAN.md) owns *what* and *why*,
each `phase_N.md` owns *how* for its phase, and this file owns only *what state
each phase is in and what was left behind*. It never becomes a place to plan
work — moving scope between phases is still a change to FINAL_PLAN.md first
(FINAL_PLAN §9).

---

## Progress

| Phase | Size | Status |
|---|---|---|
| 0 — Foundation | S | **Done** 2026-09-13 |
| 1 — RT core | M | **Done** 2026-09-13 |
| 2 — Project model, commands, `.adx` v2 | L | **Done** 2026-09-25 (CI found one clang-tidy finding; fixed in Phase 3) |
| 3 — Audio graph & scheduling | L | **Done** 2026-10-01 (CI green, run 36835905774) |
| 4 — Instruments & effects | XL | **Done** 2026-10-04 (CI green, run 37269752424; full check run 37273504347) |
| 5 — Frontend foundation | L | **Done** 2026-10-06 (CI green, run 37509766570; full check run 37512932612) |
| 6 — The DAW proper | XL | **Done** 2026-10-21 (CI green, run 39422153732; full check run 39430247203; human gate P6-1 closed by the user) |
| 7 — Text-first layer | M | **Done** 2026-10-25 (CI green, run 40243278857; full check run 40248449713; human gate P7-1 closed by the user) |
| 8 — Audio & export | M | **Done** 2026-11-05 (hardware deferred: P8-1 `[~]`; per-push CI green, run 41991668112; full check green, run 42017826073 on tag `phase-10.5-r4`) |
| 9 — Plugins & MIDI | L | **Done** 2026-11-05 (hardware deferred: P9-1 and P9-4 `[~]`; per-push CI green, run 41991668112; full check green, run 42017826073 on tag `phase-10.5-r4`) |
| 10 — Analysis, visualization, polish | L | **Done** 2026-11-05 (CI green, run 41991668112; full check green, run 42017826073 on tag `phase-10.5-r4`) |
| 10.5 — Agent-run testing | L | **Done** 2026-11-05 (CI green, run 41991668112; full check green, run 42017826073 on tag `phase-10.5-r4`) |
| 11 — Live performance | L | Built and observed locally 2026-11-08 (every §6 box, the two hardware boxes by their emulated halves); CI green, run 42494990727 at `0c6c614`; the tag-triggered full check not run yet |
| 12 — Sound-design foundation | XL | Not started |
| 13 — Core Drums & Machines | M | Not started |
| 14 — Hyperpop | L | Not started |
| 15 — Hardcore | L | Not started |
| 16 — Classical | XL | Not started |
| 17 — Ambient | M | Not started |
| 18 — Breakcore / Jungle / DnB | M | Not started |
| 19 — Phonk + Vocal | M | Not started |
| Final review | — | Not started - after Phase 19; every deferred hardware half, then every Done phase re-checked |

---

## [x] Phase 0 — Foundation · S

| | |
|---|---|
| **Plan** | [phase_0.md](phase_0.md) |
| **Entry** | Repo in post-archive state: `FINAL_PLAN.md`, `plans/`, `docs/`, `_archive/`, `.gitignore`. Nothing else. |
| **Done when** | [phase_0.md](phase_0.md) §6 — every box, including each CI gate *observed to fire* |
| **Status** | Done (2026-09-13) |
| **Completed** | 2026-09-13 |

One build command, one install command, one test command, and six gates that have
each been broken and seen to fail — locally, and five of the six in CI as well
(`ruff format` only locally; the probe happened to be format-clean and `ruff check`
caught it first). Evidence table in [phase_0.md](phase_0.md) §6.

Proving the gates in CI exposed a flaw worth more than the proof: Actions stops a job
at the first failing step, so the first attempt reported one broken gate and hid four.
The lint steps now carry `if: !cancelled()`.

Three statements in [phase_0.md](phase_0.md) §3–§4 were wrong and are corrected in its
§10 — a compiler flag MSVC does not accept, a dependency cache layout that cannot
support two build trees, and a per-target language standard that breaks the Catch2
link. P0-1 through P0-5 were all closed during the phase; see §10 for what changed.

**Open issues for Phase 1:** none. All closed.

---

## [x] Phase 1 — RT core · M

| | |
|---|---|
| **Plan** | [phase_1.md](phase_1.md) |
| **Entry** | Phase 0 §6 complete |
| **Done when** | [phase_1.md](phase_1.md) §6 |
| **Status** | Done (2026-09-13) |
| **Completed** | 2026-09-13 |

> The allocator hook's positive-control test gates every phase after this one.
> If it has never been seen to fail, it is not a gate.

It has been seen to fail. The early-out in `noteIfRealtime` was forced true, the suite
rebuilt, and `alloc_guard_positive_control` failed with its own message before being
reverted. The hook catches all eight replaceable `operator new` forms, `std::vector`,
`std::string` and `make_shared`, and does not fire outside an RT section.

33 ctest tests on Debug, RelWithDebInfo and Release, including 60 s of NullBackend with
zero violations; 60 s through real hardware at 48 kHz/256 with zero xruns and zero
violations.

And in CI: run 34749336344 green on all three matrix jobs at commit 3fef53a. That box
sat open for an hour while a GitHub incident kept the run queued with no runner; it is
recorded here because "verified locally" and "green in CI" are different claims and the
phase was briefly marked done on the wrong one.

**Open issues for Phase 2:**

- **P1-1** · `_CrtSetAllocHook` is not installed, so `malloc`/`realloc`/`free` called
  directly by a C dependency never reaches the violation log — only C++ `operator new`
  does. [phase_1.md](phase_1.md) §3.3 specifies it as the second half of the hook; it
  was deliberately skipped because it exists only in the debug CRT, and the realtime
  gate has to hold in RelWithDebInfo too, so adding it would buy coverage in the
  configuration that needs it least while implying coverage in the one that needs it
  most. Nothing today is affected: RtAudio's allocations happen on the main thread at
  open time. It becomes real in Phase 4, when miniaudio, shine and libFLAC land on the
  callback path. Fixed when: either a mechanism catches C-library allocation in
  RelWithDebInfo, or [phase_1.md](phase_1.md) §3.3 records the gap as accepted and the
  clang-tidy `malloc` ban is named as the only thing standing behind it.

- **P1-2** · Realtime safety is enforced by directory, not by reachability.
  `tools/lint.py`'s `RT_PATHS` decides what the ban list applies to, and
  `engine/audio/` is deliberately not in it — backends legitimately allocate at open
  time. But `AudioThread::render` and everything it calls *is* realtime, and lives
  there. Today that is covered because the runtime guard watches the callback
  regardless of which directory the code sits in. It stops being enough as soon as a
  phase adds a helper under `engine/audio/` that is called only from the callback.
  Fixed when: either the realtime parts of `engine/audio/` move under a path the ban
  covers, or the ban is keyed on something better than directory.

- **P1-3** · The 60-second gate asserts on callback *count*, not on per-callback
  *duration*. [phase_1.md](phase_1.md) §5 asks for "no callback over 3 ms" and that is
  not measured — nothing times an individual callback. Count within 1 % catches a
  stream that stalls; it does not catch one that makes its deadline on average while
  missing it regularly, which is what a dropout actually is. Fixed when: the callback
  records its own duration into an `OverwriteRing<LevelFrame>`-style tap and the gate
  asserts on the worst case, not the mean.

- **P1-4** · `SpscRing::tryPush` is wait-free but its failure is silent to the caller
  that matters. A full ring returns `false` and the *caller* decides; `Reaper` records a
  violation, but nothing else does yet. Phase 2 publishes command snapshots through one
  of these. Fixed when: the Phase 2 event queue records `ViolationKind::Unbounded` on a
  refused push, rather than each new caller re-deciding.

- **P1-5** · `NullBackend` and `OfflineBackend` were asserted to produce identical
  silence, which is a much weaker statement than the bit-identity Phase 3 needs. Both
  currently write zeros, so the test cannot fail for the right reason. It is here as a
  placeholder that will start meaning something the moment the graph produces sound.
  Fixed when: Phase 3's bit-identical offline-vs-realtime test replaces it with a hash
  comparison over non-trivial output.

---

## [x] Phase 2 — Project model, commands, `.adx` v2 · L

| | |
|---|---|
| **Plan** | [phase_2.md](phase_2.md) |
| **Entry** | Phase 1 §6 complete |
| **Done when** | [phase_2.md](phase_2.md) §6 |
| **Status** | Done (2026-09-25). Pushed as `b03d5bd`; CI run 36206915522 was still running when this was marked, so CI is **unconfirmed** — see below |
| **Completed** | 2026-09-25 |

The flat `Track` is gone. A project is Channels, Patterns, a Playlist and a Mixer
with an arbitrary routing DAG, every mutation is a command, and `.adx` v2 is a
written, normative spec ([docs/adx-format-v2.md](../docs/adx-format-v2.md)) whose
every diagnostic code is checked against the implementation in both directions.

The gate held, and it earned its keep: `undo_to_empty_random` (100 seeds × 10,000
commands, comparing written text as well as the model) found two real bugs before it
passed — a create command that restored zeroed id counters when it had allocated
nothing, and removals restored in the wrong order. `writer_canonical_idempotent`
found a third (v2 text labelled `ADX_VERSION=1`), and the new CRLF test a fourth
(`adx fmt` doubling line endings on Windows stdout). All four are fixed and written up
in [phase_2.md](phase_2.md) §10, along with fourteen places the plan itself was
wrong or silent.

Observed locally: 112 ctest tests on Debug, RelWithDebInfo and Release; 31 pytest
tests; clang-tidy clean on 58 files; clang-format, ruff, mypy `--strict`, header
length and the new `format-safety` gate clean. `format-safety` was seen to fail on a
planted `std::stof` — after its first version was found not to fire at all.
`suffocation.adx` loads in ~0.6 ms; 100k notes in ~150 ms.

What CI actually said, checked by the Phase 3 agent: run 36206915522 was cancelled
by the next push, and run 36207438185 (commit `5b932f0`, the same code) finished with
`ctest` and `pytest` green on all three configurations, Debug green throughout, and
**`clang-tidy` failing on Release and RelWithDebInfo**. One finding:
`modernize-loop-convert` on the reverse loop in `CommandStack.cpp`, which only fires
under `NDEBUG` because Debug's checked iterators hide the pattern from the check. The
local run above had used the Debug compile database only. Reproduced locally against
the Release database and fixed in Phase 3 (`std::views::reverse`); Phase 3 runs
clang-tidy against all three databases before marking itself done.

**Open issues for Phase 3:**

- **P2-0** · **Fixed in Phase 3** · CI for `b03d5bd` was not observed to finish. When
  it was: `clang-tidy` red on Release and RelWithDebInfo, everything else green - see
  above. Confirmation that the fix is green in CI rides on Phase 3's push (P3-0).

- **P1-1** · `CARRIED` · `_CrtSetAllocHook` is not installed, so C-library
  `malloc`/`free` never reaches the violation log. Still nothing on the callback path
  calls C allocation — Phase 2 is main-thread only — so re-carried unchanged. It
  becomes real in Phase 4 with miniaudio, shine and libFLAC. Fixed when: as stated
  under Phase 1.

- **P1-2** · `CARRIED`, **now due** · Realtime safety is enforced by directory, and
  `AudioThread::render` lives in `engine/audio/`, outside `RT_PATHS`. Phase 2 added no
  realtime code, so it could not be fixed here without inventing some. Phase 3 is the
  phase that puts real work under the callback, which makes this Phase 3's to close.
  Fixed when: as stated under Phase 1.

- **P1-3** · `CARRIED`, **now due** · The 60-second gate asserts on callback count,
  not per-callback duration. Re-carried because a duration gate over a callback that
  does nothing measures nothing; Phase 3 gives it something to do. Fixed when: as
  stated under Phase 1.

- **P1-4** · `CARRIED` · A refused `SpscRing::tryPush` is silent to its caller.
  Phase 1 expected Phase 2 to publish command snapshots through a ring, but
  phase_2.md §7 assigns the render snapshot to Phase 3 — Phase 2 has
  `CommandStack::revision()` and `dirtySince()` for Phase 3 to poll, and no queue.
  Fixed when: Phase 3's snapshot publication records `ViolationKind::Unbounded` on a
  refused push.

- **P1-5** · `CARRIED` · `NullBackend`/`OfflineBackend` equivalence is still asserted
  on silence. Unchanged; Phase 3's bit-identical offline-vs-realtime test replaces it.

- **P2-1** · The nightly 10-minute randomised fuzz run (phase_2.md §5) does not
  exist: there is no scheduled workflow at all yet. The fixed-seed corpus runs on every
  build (10k document cases, 100k full-load cases on optimised builds), so coverage is
  deterministic but never explores new inputs. Fixed when: a scheduled CI job runs the
  loader over randomly mutated input for a fixed time budget and fails on a crash or
  hang.

- **P2-2** · Invariant diagnostics have no position. `validate()` reports with
  `kNoSpan` (line 0) because a project need not have come from a file — so a routing
  cycle or a dangling reference found at load is printed without a line to jump to.
  Parser diagnostics do carry spans. Blocks Phase 7's editor panel anchoring them.
  Fixed when: the parser records each entity's source span and `validate()` reports
  against it when one exists.

- **P2-3** · The v1 `SIDECHAIN=` key input is not modelled. It migrates to a `Ducker`
  slot with `enabled`/`amount`/`releaseMs`; v1 always keyed it from the first track,
  and nothing in v2 says so yet, because a sidechain input belongs to the effect.
  Fixed when: Phase 4's Ducker has a sidechain source and the shim sets it to the
  first track's insert.

- **P2-4** · `ParamDescriptor` ranges are defined but not enforced. The registry
  knows `filter.cutoff` is 20–20000 Hz; a file saying `PARAM filter.cutoff=-5` loads
  without a diagnostic. The table is also static and names only what the v1 shim
  produces. Fixed when: Phase 4's instruments register their descriptors and the
  loader reports `ADX2001` for a value outside one.

- **P2-5** · Mini-notation is stored, not compiled (by design: phase_2.md §7). The
  consequence to know about: a v1 file with `PATTERN=` lines migrates their text into
  `MINI`, but v1 compiled them into notes at load, so such a file has fewer notes in
  v2 than it played in v1 until Phase 4's compiler lands. No corpus file uses
  `PATTERN=`, so no fixture shows it. Fixed when: Phase 4 compiles `MINI` and the
  shim test asserts a migrated `PATTERN=` produces the notes v1 did.

---

## [x] Phase 3 — Audio graph & scheduling · L

| | |
|---|---|
| **Plan** | [phase_3.md](phase_3.md) |
| **Entry** | Phase 2 §6 complete |
| **Done when** | [phase_3.md](phase_3.md) §7 |
| **Status** | Done (2026-10-01). Every §7 box observed locally on Debug, RelWithDebInfo and Release; CI green on all three jobs: run 36835905774 at `053fdec` (one earlier Debug ctest failure, P3-8) |
| **Completed** | 2026-10-01 |

> Carries the Phase 11 transport checkpoint. Read [phase_3.md](phase_3.md) §2
> before writing any code — a transport that forecloses live performance is not
> recoverable in Phase 11.

The model makes sound. A project flattens into an immutable snapshot, crosses to the
audio thread through one lock-free queue, and renders through a compiled node graph
- inserts as slot chains, sends, an arbitrary routing DAG, PDC - on a scheduler that
walks per-track cursors and splits blocks at loop and tempo boundaries. Time is a set of
sources: the scheduler takes the set, each step names its source, each node is handed
it, and `two_time_sources_independent` drives two at different positions, tempos and
loops through one scheduler. `tools/lint.py positions` makes the rule mechanical and was
seen to fail on two planted globals.

The gate: a 200-channel / 100k-note project, captured on a real audio thread and
rendered offline, hashes identically at block sizes 64, 256 and 1024 - and all six
renders hash the *same*, so the output does not depend on block size at all. Zero
allocations under the hook in Debug and RelWithDebInfo. The gate earned its keep: at
1024 frames it overflowed the per-callback arena (every instrument's scratch held until
the next callback) and failed only at that size; the arena is now scoped per step.
Written up in [phase_3.md](phase_3.md) §11 with sixteen other places the plan was wrong
or silent.

Observed locally: 148 ctest tests on each of Debug, RelWithDebInfo and Release
(including the slow set); 36 pytest tests; clang-tidy clean against all three compile
databases; clang-format, ruff, mypy `--strict`, header length, `format-safety` and the
new `positions` gate clean. Golden corpus (five fixtures plus `docs/examples/`) hashes
identically in all three configurations. Single-note incremental rebuild of the 100k
project: 0.26 ms (budget 2 ms). Loaded 60 s realtime gate: worst callback 0.61 ms
against a 5.33 ms deadline. `suffocation.adx` renders 30 s in 0.23 s and plays through a
real device.

Inherited issues: P2-0, P1-2, P1-3, P1-4 and P1-5 fixed; P2-1 wired (see below);
P1-1, P2-2, P2-3, P2-4 and P2-5 re-carried with reasons.

**Open issues for Phase 4:**

- **P3-0** · **Closed 2026-10-01** · Run 36835905774 (commit `053fdec`) is green on
  all three matrix jobs, including clang-tidy on Release/RelWithDebInfo (P2-0's fix) and
  the `positions` gate. Run 36833200360 (`cfbdd90`, same code) was green on Release and
  RelWithDebInfo and **failed `ctest` on Debug** after 11 minutes - see P3-8.

- **P3-8** · **Fixed** · Run 36833200360's Debug `ctest` failure was
  `realtime_gate_60s_under_load`: 36 of 11,251 callbacks over the 5.33 ms deadline, worst
  44 ms, on a shared runner running an unoptimised build. The gate now allows 1 % over
  deadline only when `CI` is set and `NDEBUG` is not; Release, RelWithDebInfo and local
  runs stay strict (zero over). Confirmed: CI run 36950581569 (`c628ff0`) green on all three jobs.
  **Widened 2026-10-02:** the same gate then failed in the optimised builds too -
  RelWithDebInfo run 37066275031 (6 of 11253 over, worst 10 ms) and Release run
  37074455498 (4 of 11254, worst 23 ms), on test tones that cost a fraction of a
  millisecond per callback. That is the shared runner descheduling the thread, not the
  engine, so the 1 % slack now applies to every CI build. Local runs stay strict, and
  the phase-completion local run is where the gate is held at zero.

- **P2-1** · **Observed green by manual dispatch** · Nightly run 36950606211 (`c628ff0`,
  `workflow_dispatch`) passed: the 10-minute mutation fuzz and the Phase 3 gate in
  Debug. **2026-10-02: the nightly workflow is gone** (see "When you finish a phase",
  step 5). The fuzz moved to `full-check.yml`, run per completed phase; the Debug gate
  moved to the local phase-completion run. Fixed when: the first `full-check.yml` run
  is seen green.

- **P3-1** · The master can exceed 0 dBFS. Nothing limits it - the master limiter is
  FINAL_PLAN §5.2, Phase 4's - and dense projects of test tones clip:
  `suffocation.adx` peaks at 1.25. Fixed when: the master limiter exists and the corpus
  renders with peak ≤ 1.0.

- **P3-2** · Every instrument is a test tone and every slot is the identity.
  `NodeStore::channel()` / `slot()` ignore the type; its reuse check compares polyphony
  and steal mode only. Phase 4 makes the type choose the class - and must add the type
  to the reuse check, or changing a channel's instrument will keep the old node. Every
  golden hash moves when it does; regenerate with `ADX_UPDATE_GOLDEN=1` and say why.

- **P3-3** · Parameter values are applied per piece, not smoothed: a knob turn or an
  automation step is a gain jump at a block boundary (zipper noise). Smoothing is DSP
  and belongs in the Phase 4 nodes; `ProcessContext::params` is where they read the
  target. Fixed when: gain, pan and send level ramp across a block.

- **P3-4** · Sidechain is a real port - sorted, delay-compensated, cycle-checked
  (`cycle_via_sidechain`) - but `GraphBuilder` never creates a sidechain edge, because
  the model has nowhere to say one exists (P2-3). Fixed with P2-3: the Ducker declares a
  sidechain source and the builder wires it to `kPortSidechain`.

- **P3-5** · `reset()` on seek is not called. `effectsOf()` reports
  `resetPositionalDsp`, but no Phase 3 node's output depends on the timeline position,
  so nothing consumes it. The first node that does (a tempo-synced LFO, an audio clip)
  needs a way to declare it and the scheduler must call `reset()` for it. Fixed when:
  such a node exists and `test_seek.cpp` covers it.

- **P3-6** · A voice carries eight floats of instrument state inline
  (`kVoiceStateFloats`). A Phase 4 synth that needs more keeps a parallel array indexed
  by voice slot; nothing provides one yet. Fixed when: the first instrument that needs
  it adds it to `ChannelNode`, allocated in `prepare()` through `rt::OwnedArray`.

- **P3-7** · Meters are readable only from C++ (`NodeStore::findMeter`). Phase 5's UI
  needs a zero-copy binding for the 60 Hz read (FINAL_PLAN §2.2). Fixed when: one FFI
  call returns every strip's latest `LevelFrame`.

- **P1-1** · `CARRIED` · `_CrtSetAllocHook` is not installed. Phase 3 put no C library
  on the callback path, so still nothing is exposed; Phase 4's miniaudio, shine and
  libFLAC are the first. Fixed when: as stated under Phase 1.

- **P2-2** · `CARRIED` · Invariant diagnostics have no position. Phase 7's to need, and
  Phase 3 added no diagnostic that wants one. Fixed when: as stated under Phase 2.

- **P2-3** · `CARRIED` · The v1 `SIDECHAIN=` key input is not modelled. Phase 4's
  Ducker. Now also P3-4.

- **P2-4** · `CARRIED` · `ParamDescriptor` ranges are not enforced. Phase 4's
  instruments register their descriptors.

- **P2-5** · `CARRIED` · Mini-notation is stored, not compiled. Phase 4's compiler.

Fixed in Phase 3, for the record:

- **P1-2** · The realtime ban is keyed on a file's declared thread as well as its
  directory (`// adx-thread: realtime|main`). The callback's own code moved into
  `engine/audio/CallbackCore.*` and is under the ban.
- **P1-3** · Every callback is timed into a tap; the loaded 60 s gate
  (`realtime_gate_60s_under_load`) and the Phase 1 silence gate both assert on the
  worst callback, not the count.
- **P1-4** · `EngineCore::post` records `ViolationKind::Unbounded` on a refused push
  (`engine_queue_refusal_is_recorded`); `RenderEngine` keeps the message and resends it.
- **P1-5** · `render_bit_identical_200x100k` compares hashes of non-trivial output.

---

## [x] Phase 4 — Instruments & effects · XL

| | |
|---|---|
| **Plan** | [phase_4.md](phase_4.md) |
| **Entry** | Phase 3 §7 complete |
| **Done when** | [phase_4.md](phase_4.md) §7 |
| **Status** | Done (2026-10-04). Every §7 box observed; both human gates passed (below); CI green on all three jobs: run 37269752424 at `3abcaa5`. Full check green: run 37273504347 on the `phase-4` tag |
| **Completed** | 2026-10-04 |

> Three tranches ([phase_4.md](phase_4.md) §3). Phase 5 may start once
> **Tranche A** is complete; B and C can run alongside it. If you hand off at
> Tranche A, say so in Status and list what B and C still owe.

**Tranche A evidence** (2026-10-03, local, all three configurations): every C++ test
green on Debug, RelWithDebInfo and Release (225 cases), golden hashes identical in all
three; clang-tidy over every file on all three compile databases; clang-format, ruff,
mypy, headers, format-safety, positions and the new `dsp-math` gate clean; pytest 37
passed against a fresh `pip install -e .[dev]`; `adx info` names each channel's instrument.
`suffocation_spectral_match`: 31 of 31 third-octave bands within 1.5 dB of the fixed-v1
reference (worst +1.45 dB at 99 Hz); phase_4.md §11 says why the reference is v1 with
two scheduling bugs fixed. EBU TECH 3341 cases 1-5 within 0.1 LU. CPU per voice, Release:
Additive 0.084 % of a core per voice, VA 0.23 %, test tone 0.015 % (`adx_tests "[.perf]"`, 16 held voices; the sampler's default one-shot ends early, so its 0.012 % is not a sustained figure). Corrections to the plan: phase_4.md §11.

**Tranches B, C and D** (2026-10-04, commit `3abcaa5`): every §5.3 instrument and §5.4
effect exists, is tested and has a golden hash - 13 instrument types, 27 effect types.
Observed locally: 282 C++ cases green on each of Debug, Release and RelWithDebInfo (4
skipped: the Teto tests, without `ADX_TETO_DIR`), golden hashes identical in all three;
clang-tidy clean over all 196 files on all three compile databases; pytest, headers,
dsp-math, positions, format-safety, ruff and mypy clean. Phase completion, locally in
Debug: `ADX_FULL_EVIDENCE=1 ctest --preset windows-x64-debug -L slow` passed in 887.67 s
(the ctest timeout was 600 s and is now 1800: `render_bit_identical_200x100k` alone takes
590 s unoptimised). Voice: the four local Teto tests pass against the real bank. CI: run
37269752424 green on all three jobs. Corrections to the plan: phase_4.md §11, Tranches
B/C/D. Clang-tidy's run had been hiding findings in four files behind a crash in
`tools/lint.py`, which decoded clang-tidy's output in the console code page and failed on
the kana in the Voice sources; fixed (UTF-8). It also caught Slicer.h and
SamplePoolChannel.h putting `<vector>` within reach of realtime code; their main-thread
halves moved to SliceLayout.h and PoolZones.h.

**Human gates (§7), recorded 2026-10-04 from the user's listening:**

- **`suffocation.adx` A/B · passed on parity.** In the level-matched file that
  alternates v1 (fixed) and adX every 4 s, the user could not hear where the switches
  were, and heard no difference in the bass region either. But both sound "nothing like
  Suffocation - way too loud, super distorted". That is the *file*, not the port: v1's
  master drive is `tanh((1 + MASTER_DRIVE) x)` and the file sets `MASTER_DRIVE=3.0`, so
  the master is a 4x tanh; the render's crest factor is about 2 dB (peak -2.7 dBFS, RMS
  -4.7). Without the master drive it still peaks at the 0.8 limiter ceiling (RMS -11
  dBFS): the tracks sum hot. The port reproduces v1 faithfully, which is what this gate
  asks; the mix is opened as P4-10.
- **Voice (Teto) · passed.** `examples/teto_demo.adx` ("Sakura"): the user can discern
  the lyrics, and it is not too distorted.

**Open issues for Phase 5:**

- **P4-0** · **Closed 2026-10-05** · Full check run 37273504347 on the `phase-4` tag
  (`f89ce07`) green: clang-tidy over every file in Debug and Release, and the 10-minute
  loader fuzz. Per-push CI green on `3abcaa5` (run 37269752424) and `f89ce07` (run
  37273504332). This is the first full-check run seen green, which also closes **P2-1**.
- **P4-10** · `docs/examples/suffocation.adx` is the wrong recreation: the user's
  judgement (2026-10-04) is that the file's "stem" - its reconstruction of the Crystal
  Castles track - does not match the original, which is why neither engine sounds like
  Suffocation; it is also mixed far too hot (MASTER_DRIVE=3.0, hot track levels). A
  content fix, not an engine one: the port reproduces v1 faithfully on this file. A new
  recreation moves its golden hash and the A/B reference bands, and the v1 parity check
  stays measured against the old file. Fixed when: the user has heard a new recreation
  and accepted it.
- **P4-6** · Any project with a Voice channel plays 0.4 s late in realtime (the preroll
  PDC compensates). Fixed when: Phase 11 decides whether live play accepts it.
- **P4-7** · WORLD analyses are cached in memory only (no `.adxfrq`); the render cache
  never evicts.
- **P4-8** · Baked Voice parameters (gender, breathiness...) are read from the channel;
  their automation is not evaluated per WORLD frame.
- **P4-9** · PitchShifter is the phase vocoder only; RubberBand is not a dependency yet
  (Phase 8 brings it for offline stretch).
- **P4-1** · `Engine.levels()` is one call for every strip (P3-7 closed), but it copies
  into a fresh array each time. If the 60 Hz timer shows it in a profile, Phase 5
  binds a persistent buffer instead. Fixed when: measured, either way.
- **P4-2** · Presets carry no samples on their own: `preset::instrumentOf()` leaves a
  sampler preset's zones for the caller to register in the project's sample table.
  Phase 5/6's browser needs one command that applies a preset, samples included.
  Fixed when: dragging a sampler preset onto a channel plays it.
- **P4-3** · Project `TUNING` does not reach instruments; they assume A4 = 440 Hz.
  Every corpus file uses 440. Fixed when: `ProcessContext` carries it and a test at
  432 Hz passes.
- **P4-4** · v1's master ran compressor, master volume, clamp; the shim builds
  compressor, limiter, master gain. Audible only for a hot v1 master without drive.
  Fixed when: someone hears it matter, by moving the gain before the limiter.
- **P4-5** · The SubBass track is +1.7 dB at 99 Hz against v1 (within the gate, but
  unexplained: not control rate, not unison phases).
- **P3-3** · `CARRIED` · Channel and insert gain, pan and width are per frame when
  automated (closed for those); a **send level** is still applied per block.
  Fixed when: send level ramps across a block.
- **P3-5** · `CARRIED` · `resetPositionalDsp` is still consumed by nothing. The
  tempo-synced LFO (`dsp::Lfo`, sync mode) needs no reset - its phase is a function of
  position - and no node uses it yet. A Delay's or Reverb's tail does carry across a
  seek, as in every DAW that does not flush; decide in Phase 5 whether a seek flushes
  effect tails. Fixed when: decided, and `test_seek.cpp` covers it.
- **P1-1** · `CARRIED` · `_CrtSetAllocHook` is not installed. miniaudio now decodes,
  but on SamplePool worker threads, never on the callback, so still nothing is
  exposed. Fixed when: as stated under Phase 1.
- **P2-2** · `CARRIED` · Invariant diagnostics have no position. Phase 7's.
- **P2-5** · `CARRIED` · Mini-notation is stored, not compiled. phase_4.md §8 gives the
  compiler to Phase 7, which this file had as Phase 4's: Phase 7's.

Fixed in Phase 4, for the record: **P2-3** and **P3-4** (sidechain: `sidechain=` on a
slot, a real graph edge, cycle-checked), **P2-4** (descriptor ranges: `ADX1004`,
`ADX2001`), **P3-1** (master Limiter; the corpus peaks at or below 1.0), **P3-2** (node
reuse compares instrument and effect type; goldens regenerated), **P3-6** (per-voice
state arrays in `Instrument<State>`), **P3-7** (`Engine.levels()`), **P3-3** for
channel and insert gain/pan/width.

---

## [x] Phase 5 — Frontend foundation · L

| | |
|---|---|
| **Plan** | [phase_5.md](phase_5.md) |
| **Entry** | Phase 4 Tranche A complete ([phase_4.md](phase_4.md) §3) |
| **Done when** | [phase_5.md](phase_5.md) §6 — **including the frame-rate acceptance step** |
| **Status** | Done (2026-10-06). Every §6 box observed locally, the frame-rate acceptance step included; CI green on all three jobs: run 37509766570 at `6338bda`. Full check green: run 37512932612 on the `phase-5` tag |
| **Completed** | 2026-10-06 |

> Where FINAL_PLAN §2.2's bet is proven or disproven. If the three rules do not
> hold on the piano roll, that is a FINAL_PLAN-level finding, not an open issue
> — escalate rather than proceeding to Phase 6.

**The bet held.** Panning and zooming a 10,000-note pattern (with 1,000 ghosts) on
real hardware (Direct3D 11): frame time p99 **0.22 ms** against a 16.6 ms budget, and
the display vsync-locked at **60.0 fps** - one frame in 1,189 missed its vsync. A pan
inside the built range is 0 engine calls; a UI frame (playhead, transport, every meter)
is exactly 1. The three rules are mechanical: `tools/lint.py rules` is a CI step and
was seen to fire on one planted violation of each. No escalation.

What exists: a dockable shell (browser, piano roll, waveform, meters; layout persisted,
resettable; dark and light themes defined once in `tokens.py`) with a transport bar; the
piano roll as a C++ scene-graph item over `engine/geometry/` - culled, level-of-detail,
allocation-free rebuilds, selection and theme recoloured in place, the playhead its own
node; nine editing tools plus chords, every gesture one command (`EditNotes`, new) and
undoable byte for byte; six lanes and a lyric lane with the alias each note will sing;
quantize, seeded humanize, scale snapping, arpeggiate, seeded riffs; the waveform view
over the ported `ClipPeakCache` (worker thread, never blocks). The C++ items build
against a Qt SDK matching PySide6 that `tools/fetch_qt.py` provides - PySide6 ships
no headers, which the plan had not known. Twenty corrections to the plan:
[phase_5.md](phase_5.md) §10.

Observed locally (2026-10-06): C++ - 311 cases on each of Debug, RelWithDebInfo and
Release; perf-gate budgets on Release: full 10k build 0.136 ms (2.0), one bar
0.0038 ms (0.10), `hitTestRect` 0.0082 ms (0.20). Python - 97 tests (96 without the GPU
test), including the FFI round trip at 0.022 ms (0.50). clang-tidy clean over all 214 files on all three compile databases (after fixing 34
findings in the new code).
clang-format, ruff, ruff format, mypy `--strict`, headers, format-safety, positions,
dsp-math and the new `rules` gate clean. Phase completion, locally in Debug:
`ADX_FULL_EVIDENCE=1 ctest --preset windows-x64-debug -L slow` passed in 914.52 s. The app on the real audio device: `suffocation.adx` plays, the playhead
and every meter move, worst callback 0.89 ms.

Inherited issues: **P4-1** fixed (`Engine.frame()` fills a caller-owned array);
**P3-5** decided and covered (a seek does not flush effect tails;
`seek_keeps_effect_tails`). The rest re-carried below with reasons.

**Open issues for Phase 6:**

- **P5-0** · **Closed 2026-10-06** · Per-push CI green: run 37509766570 at `6338bda`,
  all three jobs (it fetches the Qt SDK, builds the plugin and runs the rules gate).
  Full check green: run 37512932612 on the `phase-5` tag (`4765ca3`) - clang-tidy over
  every file in Debug and Release, and the 10-minute loader fuzz. The first push
  (`406f1e9`, run 37502991839) was red on all three jobs: `ruff` on `test_waveform.py`
  (written after the local format pass), and on Release the
  Phase 1 `overwrite_ring_concurrent` slow test. That test's writer spun unthrottled
  instead of at 48 kHz as phase_1.md specifies, so a loaded runner could lap the ring
  mid-read - the seam the ring documents as allowed. Fixed: the writer is paced at
  48 kHz, and a read the writer provably overtook is counted (and must be rare) instead
  of failing the contiguity check.
- **P5-1** · No note audition. Drawing a note, clicking a key or dragging a pitch is
  silent: the engine has no preview-voice message from the main thread to the audio
  thread. Phase 6's channel rack and browser want the same thing. Fixed when: a drawn
  note sounds once, through the channel's instrument, without a snapshot rebuild.
- **P5-2** · Slides and pitch curves are drawn as straight segments; a curved slide
  (Exponential, Logarithmic, Smooth) sounds curved but looks straight. Fixed when: the
  `curves` layer subdivides curved segments with `core::Curve`.
- **P5-3** · The metronome button is disabled: no phase has built a click source in the
  engine. Fixed when: the transport bar's toggle sounds a click on the beat in realtime
  and never in an offline render.
- **P5-4** · The waveform panel shows files opened from the browser only. Audio clips
  in the playlist are Phase 6's, and the cache key's pitch, stretch, reversed and gain
  fields mean nothing until Phase 8 processes clips. Fixed when: a playlist clip draws
  through `WaveformView` and a changed key field redraws it.
- **P5-5** · The roll edits one channel in one pattern. Ghosts show the others, but
  there is no "edit all channels" mode, and selecting a ghost does nothing. Fixed when:
  Phase 6 decides whether the roll needs it (FL has it; it is not in §5.1's list).
- **P5-6** · The 60 fps measurement is one machine (this development box, Direct3D 11,
  vsync 60 Hz). Fixed when: someone runs `pytest -m gpu` on a second machine, ideally a
  laptop GPU, and records the numbers here.
- **P4-10** · `CARRIED` · `suffocation.adx` is the wrong recreation. Another agent is
  reconstructing the stem now (2026-10-06); nothing in Phase 5 depends on the file's
  content. Fixed when: as stated under Phase 4.
- **P4-2** · `CARRIED` · Presets carry no samples on their own. The browser here lists
  projects and audio; presets are Phase 6's browser content. Fixed when: as stated
  under Phase 4.
- **P4-3** · `CARRIED` · Project `TUNING` does not reach instruments. Engine work no
  Phase 5 surface needed. Fixed when: as stated under Phase 4.
- **P4-4** · `CARRIED` · v1 master gain order. Unchanged; nobody has heard it matter.
- **P4-5** · `CARRIED` · SubBass +1.7 dB at 99 Hz, unexplained. Unchanged.
- **P4-6** · `CARRIED` · Voice preroll latency in realtime. Phase 11's decision.
- **P4-7** · `CARRIED` · WORLD analyses cached in memory only. Unchanged.
- **P4-8** · `CARRIED` · Baked Voice parameters not automated per WORLD frame.
- **P4-9** · `CARRIED` · PitchShifter is the phase vocoder only. Phase 8.
- **P3-3** · `CARRIED` · A send level is applied per block. DSP; Phase 5 built no
  send UI to expose it. Fixed when: as stated under Phase 4.
- **P1-1** · `CARRIED` · `_CrtSetAllocHook` is not installed. Phase 5 put nothing on
  the callback path. Fixed when: as stated under Phase 1.
- **P2-2** · `CARRIED` · Invariant diagnostics have no position. Phase 7's.
- **P2-5** · `CARRIED` · Mini-notation is stored, not compiled. Phase 7's.

Fixed in Phase 5, for the record: **P4-1** (`Engine.frame()`, a persistent buffer, and
the meter frame counted at exactly one call) and **P3-5** (decided: no flush; tested).

---

## [x] Phase 6 — The DAW proper · XL

| | |
|---|---|
| **Plan** | [phase_6.md](phase_6.md) |
| **Entry** | Phase 5 §6 complete, including the frame-rate acceptance step |
| **Done when** | [phase_6.md](phase_6.md) §6 |
| **Status** | Done (2026-10-21). Every automated §6 box observed locally, the 200-insert frame-rate step included; per-push CI green (run 39422153732 at `094bc87`); full check green (run 39430247203 on `phase-6-built`); the human gate (P6-1) closed by the user. At the closing commit `0259310` (tag `phase-6`): per-push CI green (run 39563772077), full check green (run 39563772177) |
| **Completed** | 2026-10-21 |

**What exists.** The whole §2 manifest: channel rack (a C++ step grid, 64x64 with no
per-cell widget), playlist (draw, paint, select, slice, slip, stretch, mute, group; track
groups; markers; arrangements; audio clips drawn through the peak cache; clip
envelopes), mixer (200 strips, routing matrix that disables cycle cells before a
command exists, slots, sends), the generic parameter editor plus hand-built overviews
for nine instruments and nine effects, automation lanes and controllers (LFO, envelope,
peak, engine-side), browser with typed drag-and-drop, history, inspector, preferences
and autosave, one action registry with a remappable keymap, one global selection with
delete/copy/paste/duplicate over all six kinds, and the clipboard as `.adx` text.
Engine work under it: arrangement commands, controllers, audition voices and the
metronome, the geometry builders. Fifteen corrections to the plan:
[phase_6.md](phase_6.md) §10.

**The gate.** `test_workflow_end_to_end` writes, arranges, mixes and exports a 32-bar
track through the real panels - rack, piano roll, playlist, mixer, editors, automation,
File > Export / Save / Open - and the reopened project renders to identical bytes.

Observed locally (2026-10-20): C++ - 337 cases on each of Debug, RelWithDebInfo and
Release; clang-tidy clean over all 232 files on all three compile databases (59
findings fixed in the new code); clang-format, ruff, ruff format, mypy `--strict`,
headers, format-safety, positions, dsp-math and `rules` clean. Python - 156 tests green, both GPU tests included (154 without them, as CI runs).
The mixer on real hardware (Direct3D 11), 200 inserts metering while playing: 1,189 frames with the whole application up and the transport playing: frame time (the UI frame step plus the meter view's sync and render) p50 0.29 ms, p99 **0.63 ms**, max 2.1 ms; displayed frames vsync-locked at **60.0 fps**, interval p99 18.7 ms, max 20.6 ms - not one missed vsync; exactly **1 engine call per frame**; every strip's meter moved. (The first run failed this box - see §10, correction 14.)
The piano roll's Phase 5 measurement re-run on this build: p99 0.23 ms, 59.9 fps, interval p99 17.6 ms (Phase 5 recorded 0.22 ms and 60.0). Phase completion,
locally in Debug: `ADX_FULL_EVIDENCE=1 ctest --preset windows-x64-debug -L slow`
passed in 944.11 s

Launcher (2026-10-20, after the last build): `check_launcher: adX.cmd opened adX 0.1.0 -
11 panels, audio device open`.

Inherited issues: **P5-1** fixed (the piano roll auditions too; it had been done only in
the rack); **P5-2** fixed (curves drawn through `core::Curve`); **P5-3** fixed
(metronome: realtime only, never rendered); **P5-4** fixed (playlist audio clips through
the peak cache, redrawn on a key change); **P5-5** decided (below); **P4-2** fixed (a
dropped sampler preset brings its samples and plays); **P4-3** fixed
(`tuning_reaches_instruments_at_432`); **P3-3** fixed (`send_level_ramps_across_a_block`).
The rest re-carried below with reasons.

**Open issues for Phase 7:**

- **P6-0** · **Closed 2026-10-20** · Per-push CI green: run 39422153732 at `094bc87`, all three jobs. The
  three runs before it were red on pytest only, every job, for one cause: PySide6 6.12.0
  had shipped and the floating `>=6.7` let CI install it against the 6.11.2 Qt SDK the
  plugin builds with (phase_6.md §10, correction 15); now pinned. The full check runs on
  the `phase-6-built` tag (a `phase-*` tag, so it triggers the workflow) - `phase-6`
  itself is kept for the commit that closes P6-1. Full check **green**: run 39430247203
  on `phase-6-built` (`f1580f3`) - clang-tidy over every file in Debug and Release, and
  the 10-minute loader fuzz.
- **P6-1** · **Closed 2026-10-21** · The human gate (phase_6.md §6 box 2): nobody has made a piece
  of music with the build. Phase 7's entry is "Phase 6 §6 complete", so Phase 7 waits
  on it. An agent cannot close it. Fixed when: the user makes a track start to finish
  with `adx gui`, no text editor, and what they made and what got in the way is
  recorded here; anything that got in the way becomes an issue. **Session 1
  (2026-10-21):** editing and saving worked; the interface did not - the piano roll
  could not be resized, and the screen was crowded and unclear. Every finding is fixed
  and tested (phase_6.md §11). Still open: a full track, and the user's verdict on the
  revised interface. **Session 2 (2026-10-21):** the user wrote a phrase
  (`wanna_cry_human.adx`) and tried to repeat it with copy and paste; the file holds
  6 distinct notes, each stacked 12 times - every paste landed at tick 0 on top of the
  original. That, Stop rewinding, an undraggable unsnapped playhead, a faint rubber
  band and redo being Ctrl+Y only are fixed and tested (phase_6.md §11, second table).
  `output/transcription/wannacry_piano.adx` was written by an agent and does not count
  toward this gate. **Session 3 (2026-10-21):** a paste still landed a beat late when the
  copied notes began mid-bar (the clipboard text keeps notes' place in their bar; the
  paste kept that offset) - fixed: the first copied note lands on the cursor. Then, in
  the user's words: "the track is done. phase 6 is complete mark it as done."
  `wanna_cry_human.adx`, made in the app: one additive channel, one 4-bar pattern, 28
  notes, placed once; it loads with no diagnostics. What got in the way across the three
  sessions is phase_6.md §11; all of it is fixed and tested, so no new issue.
- **P6-2** · Two panels exceed §3 rule 1's ~400 lines: `piano_roll/panel.py` (840,
  Phase 5's, grown by audition) and `playlist/panel.py` (494). Ruff only limits
  function length. Fixed when: each is split into sub-widgets under ~400 lines.
- **P6-3** · The wavetable overview is a 2-D frame strip; §4.4's table asks for a 3-D
  table view. Usable as is (every parameter is in the grid beside it). Fixed when: a
  3-D (or stacked) view ships, or the table entry is struck as not worth it.
- **P5-5** · Decided, closed: no "edit all channels" mode. Ghosts show the other
  channels and a rack row's click opens that channel's clip in the roll, which covers
  the use; §5.1 does not list it. Reopen only if P6-1's session asks for it.
- **P5-6** · `CARRIED` · Both GPU measurements (piano roll, mixer) are one machine.
  Fixed when: someone runs `pytest -m gpu` on a second machine, ideally a laptop GPU.
- **P4-10** · `CARRIED` · `suffocation.adx` is the wrong recreation; the stem work in
  `scripts/` and `work/` (not this phase's, uncommitted) is that effort.
- **P4-4**, **P4-5**, **P4-7**, **P4-8** · `CARRIED` · Unchanged; nothing in Phase 6
  touched master gain order, SubBass, WORLD caching or per-frame Voice automation.
- **P4-6** · `CARRIED` · Voice preroll latency in realtime. Phase 11's decision.
- **P4-9** · `CARRIED` · PitchShifter is the phase vocoder only. Phase 8.
- **P1-1** · `CARRIED` · `_CrtSetAllocHook` is not installed. Phase 6 put audition and
  metronome code on the callback path: fixed-size arrays, no allocation by
  construction, but no test runs them under an allocation guard. Fixed when: as stated
  under Phase 1, and the audition/metronome render tests run under the guard.
- **P2-2** · `CARRIED` · Invariant diagnostics have no position. Phase 7's.
- **P2-5** · `CARRIED` · Mini-notation is stored, not compiled. Phase 7's.

---

## [x] Phase 7 — Text-first layer · M

| | |
|---|---|
| **Plan** | [phase_7.md](phase_7.md) |
| **Entry** | Phase 6 §6 complete |
| **Done when** | [phase_7.md](phase_7.md) §7 |
| **Status** | Done (2026-10-25). Built 2026-10-21; every automated [phase_7.md](phase_7.md) §7 box observed locally on Debug, RelWithDebInfo and Release; the human gate (P7-1) closed by the user. At `be487fe` (tag `phase-7`): per-push CI green (run 40243278857, and 40248449731 on the tag), full check green (run 40248449713) |
| **Completed** | 2026-10-25 |

> The half no other DAW has (FINAL_PLAN §1). Deferred scope here costs more than
> deferred scope anywhere else — be specific in the open issues.

**What exists.** A project can be edited in the GUI, in the in-app editor and in an
external editor at once. A re-parsed file becomes the narrowest commands
(`engine/format/adx/Patch.h`: ids remapped by name and number, notes by (start, pitch),
one `ReplaceProject` for any structural remainder), executed as one undo step; a
`ReadDirectoryChangesW` watcher (150 ms debounce, any event naming the file) feeds it;
`TextSync` keeps adX from hearing its own writes, refuses text with parse or validation
errors, and raises a conflict - keep mine, take theirs, show the diff - rather than
guess. Live mode writes 300 ms after the last edit. A reload never cuts a voice: surviving
entities keep their ids and so their nodes; a new effect type crossfades from the old
over 20 ms, a new instrument rings out the old one's released voices, a removed channel
rings out into the insert it fed (`graph/Handover.h`, `nodes/DepartingNode`). Mini-notation
is ported (Bjorklund fixed, ticks, spans, song cycles) and plays; the editor panel
highlights from engine tokens, underlines diagnostics (parse and validation, at their
lines) as you type within 5 ms on 10,000 lines, completes from the type catalog, lights
the sounding mini-notation step, and links its cursor with the piano roll. `adx` the
scripting module issues the GUI's commands, with a REPL console, `on_bar` callbacks on the
main thread, documented examples that are tests. Sixteen corrections to the plan:
[phase_7.md](phase_7.md) §11.

**The gates.** `test_bidirectional`: 200 interleaved GUI and external edits while
playing, Live sync, both sides converging after every one, ten deliberate collisions each
raised as a conflict, the file round-tripping - it found two data-loss bugs first (§11,
9-10). `reload_no_voice_cut`: 100 rewrites while a pad plays, output **bit-identical** to
no reloads, zero allocator-hook violations.

Observed locally (2026-10-21): C++ - 370 cases on each of Debug, RelWithDebInfo and
Release; clang-tidy clean over all 251 files on all three compile databases (99 findings
fixed); clang-format, headers, format-safety, positions, dsp-math and `rules` clean.
Python - 200 tests green without the GPU tests, both GPU tests green; ruff, ruff format,
mypy `--strict` clean. Patch fuzz 10,000 pairs per run plus 80,000 more over eight
seeds. Phase completion, locally in Debug: `ADX_FULL_EVIDENCE=1 ctest --preset
windows-x64-debug -L slow` passed in 972.41 s.

Launcher (2026-10-21, after the last build): `check_launcher: adX.cmd opened adX 0.1.0 -
13 panels, audio device open`.

After the CI fixes, observed locally (2026-10-25): ctest 100% on Debug (371, `-LE slow`),
RelWithDebInfo and Release (372 each); pytest 201 passed with `CI=1` in 3:55;
clang-tidy clean on the changed units against all three databases; clang-format,
headers, format-safety, positions, dsp-math, `rules`, ruff, ruff format and mypy
`--strict` clean. Launcher: `check_launcher: adX.cmd opened adX 0.1.0 - 13 panels, audio
device open`.

Inherited issues: **P2-2** fixed (validate reports at source lines via `SourceMap`;
`adx validate`, the editor and reloads use it); **P2-5** fixed (mini-notation compiles
and plays); **P6-1**, **P6-0** were closed in Phase 6. The rest re-carried below.

**Open issues for Phase 8:**

- **P7-0** · **Closed 2026-10-25** · The first push (7083e72, CI run 40174263448)
  failed. `incremental_budget` measured 6.1-6.6 ms against its 5 ms budget on the
  optimised runners. pytest hung on all three jobs until the six-hour limit, in a modal
  "Save changes?" box opened by `test_external_tempo_edit_shows_in_the_transport`. Both
  fixed in `be487fe` ([phase_7.md](phase_7.md) §11, 17-19): CI green on all three jobs
  (run 40243278857; 40248449731 on the tag), full check green on `phase-7` (run
  40248449713 - fuzz, clang-tidy over every file in Debug and Release).
- **P7-1** · **Closed 2026-10-25** · the human gate ([phase_7.md](phase_7.md) §7
  box 3). The user, in their words: "i personally checked the live edit barrier."
- **P7-2** · A node handing over renders only its own block, so a second effect-type
  change within the first's 20 ms, or a second instrument change within the first's
  release, cuts the first one's tail (`misc-no-recursion`; phase_7.md §11, 7). Fixed
  when: a chain of handovers is walked iteratively, or someone hears it matter.
- **P7-3** · Incremental diagnostics are cheap only for edits inside a `[PATTERN]` body;
  an edit to a channel, the mixer or a header is a full parse - 10.4 ms on a
  10,000-line file in Release, over the 5 ms budget. Pattern bodies are nearly all of a
  big file, so the common keystroke is inside budget (1.2 ms since §11, 17). Fixed when: the other
  section kinds get the same treatment, or a profile shows it is felt.
- **P7-4** · The cursor link with the piano roll covers note lines only; a MINI step or
  an automation breakpoint under the cursor selects nothing. Fixed when: the roll can
  show compiled MINI notes (Phase 11's launchable mini clips may want the same).
- **P7-5** · Conflict resolution is whole-file (keep mine / take theirs / diff); there is
  no three-way merge of non-overlapping edits. By design (§4.4: "do not guess"), but a
  per-section merge would turn most real conflicts into no conflict. Fixed when:
  decided.
- **P7-6** · `FileWatcher`'s non-Windows fallback (a 50 ms size/time poll) is compiled
  only off Windows and has never run. Fixed when: a non-Windows build exists.
- **P6-2** · `CARRIED`, grown · Panels over §3 rule 1's ~400 lines:
  `piano_roll/panel.py` (840), `playlist/panel.py` (494), and now `editor/panel.py`
  (410). Fixed when: each is split into sub-widgets under ~400 lines.
- **P6-3** · `CARRIED` · The wavetable overview is a 2-D frame strip, not §4.4's 3-D
  view.
- **P5-6** · `CARRIED` · Both GPU measurements are one machine.
- **P4-10** · `CARRIED` · `suffocation.adx` is the wrong recreation.
- **P4-4**, **P4-5**, **P4-7**, **P4-8** · `CARRIED` · Unchanged; Phase 7 touched none of
  master gain order, SubBass, WORLD caching or per-frame Voice automation.
- **P4-6** · `CARRIED` · Voice preroll latency in realtime. Phase 11's decision.
- **P4-9** · `CARRIED`, **now due** · PitchShifter is the phase vocoder only; RubberBand
  arrives with Phase 8's offline stretch.
- **P1-1** · `CARRIED` · `_CrtSetAllocHook` is not installed. Phase 7 added handover
  rendering on the callback path (fixed-size remembered parameters, arena scratch, no
  allocation by construction); `reload_no_voice_cut` runs 100 reloads under the guard
  with zero violations, but its edits are inaudible and trigger no handover, and the
  handover tests do not count violations. Fixed when: as stated under Phase 1, and the
  handover and departure tests run under the guard.

---

## [x] Phase 8 — Audio & export · M

| | |
|---|---|
| **Plan** | [phase_8.md](phase_8.md) |
| **Entry** | Phase 7 §7 complete |
| **Done when** | [phase_8.md](phase_8.md) §6 |
| **Status** | Done (hardware deferred) 2026-11-05. Every [phase_8.md](phase_8.md) §6 box observed; the latency gate is `[~]` (P8-1, hardware half in the final review). CI: per-push CI green, run 41991668112; full check green, run 42017826073 on tag `phase-10.5-r4`, its three `three-builds` jobs green |
| **Completed** | 2026-11-05 - per-push CI green, run 41991668112; full check green, run 42017826073 on tag `phase-10.5-r4` |

**What exists.** adX records, edits and delivers. Audio clips play (`AudioTrackNode`,
stateless in position) with slip, trim, fades, crossfades, clip envelopes, destructive
edits on a cached copy, RubberBand stretch, pitch and warp markers seeded from onsets; a
channel freezes bit-identically and thaws exactly. Export covers WAV 16/24/32f (RF64),
MP3, FLAC and OGG, ranges, tail-until-silence, split at markers, peak/LUFS normalise,
resample, TPDF/shaped dither, stems per channel/insert/pattern that sum to the master,
a MIDI side file, and runs as a job with progress and cancel. MIDI files import and
export (3840 PPQ, exact round trip). Collect-and-save rewrites paths in the text and
zips. Recording: capture ring, disk writer with header checkpoints, punch, count-in,
takes on lanes, comping, monitoring, loopback calibration. In the app: the export
dialog, a record toolbar, a clip editor panel, MIDI import/export, collect-and-save and
freeze in the menus, `adx render` headless. Twenty-two corrections: phase_8.md §11.

**Observed locally (2026-10-29).** ctest 100% on Debug (426, `-LE slow`), RelWithDebInfo
(427) and Release (427). Phase completion in Debug: `ADX_FULL_EVIDENCE=1 ctest --preset
windows-x64-debug -L slow` passed in 1374.74 s (before the clang-tidy clean-up, which
changed no behaviour). On the final tree (after that clean-up): ctest 100% on Debug (427, `-LE slow`),
RelWithDebInfo (428) and Release (428); pytest 237 passed with `CI=1` (GPU tests
deselected); clang-tidy clean over all 292 files on all three compile databases;
clang-format, headers, format-safety, positions, dsp-math, `rules`, ruff, ruff format
and mypy `--strict` clean. Launcher: `check_launcher: adX.cmd opened adX 0.1.0 - 14
panels, audio device open`.

Inherited issues: **P4-9** fixed (RubberBand offline stretch, pitch and warp:
`engine/clip/Stretch`; the realtime PitchShifter stays the phase vocoder, which is the
right tool for a live effect). **P1-1** advanced: `capture_no_alloc` and
`midi_input_no_alloc` run under the allocator guard with zero violations; the handover
tests still do not count. Re-carried below.

**Open issues for Phase 9:**

- **P8-0** · **Per-push CI green 2026-11-05** (run 41991668112, with Phases 9-10.5 on top); full check green (run 42017826073, tag `phase-10.5-r4`). Was: **BLOCKER for marking Phase 8 done** · Nothing is committed or pushed. Fixed
  when: per-push CI is green on all three jobs, and the full check - including the new
  `three-builds` job, which is `three_clean_builds_identical` - is green on a `phase-8`
  tag; record both run ids here and in phase_8.md §10.
- **P8-1** · **HALF CHECKED `[~]`; hardware half deferred to the final review; no longer
  blocks Phase 8** · The latency gate, `record_latency_alignment` at 3 buffer sizes. The
  emulated half passed 2026-10-30: the simulated cable, and VB-Audio Virtual Cable (offset 0
  at all three sizes after 5 s of settling, phase_8.md §10). The physical loopback is
  listed under "Final review" at the bottom of this file. Also open from that run, not
  hardware: calibration can be taken before a device's path settles, and the engine's
  xrun count isn't exposed to Python, so a shift after calibration goes unnoticed.
- **P8-2** · A cut inside a fade is not continuous (phase_8.md §11, 16): each half keeps
  its own side of the fade, shortened. Fixed when: a fade can be split into a gain
  envelope segment, or someone hears it matter.
- **P8-3** · No VBR MP3 (shine is CBR), no automation-as-CC in MIDI export (needs MIDI
  learn's mapping, Phase 9), no loop recording (Phase 11's performance recording is the
  natural owner), comp offsets exact only at constant tempo, a stop cuts audio clips
  instead of fading. phase_8.md §11, 5-9.
- **P8-4** · The record toolbar punches over the selected clips' span because the
  playlist has no loop region of its own; the recording input is a preference applied
  at the next start. Fixed when: the playlist has a loop/time selection, and the engine
  can reopen its stream with an input without a restart.
- **P8-5** · Found 2026-11-02 by Phase 10's STAKILLAZ golden fixture · MP3 export of a
  full-scale, hard-clipped, bass-heavy master is wrong: shine's subband filter sums 64
  fixed-point products in 32 bits and overflows, so the decoded file peaks at 2.3 with
  +20 dB below 50 Hz. LAME on the same samples is within 0.1 dB. `golden_all_formats`
  names the fixture as known-bad and checks the overflow is still there, so a fix fails
  it. Fixed when: shine is given headroom in its fixed-point path (and its global gain
  compensated), or the MP3 encoder is LAME; then the entry comes out of the test.
- **P7-2** … **P7-6** · `CARRIED` · Unchanged by Phase 8 (handover chains, incremental
  diagnostics outside patterns, MINI cursor link, three-way merge, non-Windows watcher).
- **P6-2** · `CARRIED`, grown · Over §3 rule 1's ~400 lines: `piano_roll/panel.py` (840),
  `playlist/panel.py`, `editor/panel.py`, and `mainwindow.py` (now ~690). Phase 8 put
  its window actions in `adx/delivery.py` rather than grow the window further.
- **P6-3**, **P5-6**, **P4-10**, **P4-4**, **P4-5**, **P4-7**, **P4-8**, **P4-6** ·
  `CARRIED` · Untouched by Phase 8.
- **P1-1** · `CARRIED` · `_CrtSetAllocHook` is not installed; the handover and departure
  tests do not run under the guard. As before.

---

## [x] Phase 9 — Plugins & MIDI · L

| | |
|---|---|
| **Plan** | [phase_9.md](phase_9.md) |
| **Entry** | Phase 8 §6 complete |
| **Done when** | [phase_9.md](phase_9.md) §6 |
| **Status** | Done (hardware deferred) 2026-11-05. Built 2026-10-30. Every [phase_9.md](phase_9.md) §6 box observed: the MIDI-latency and ASIO gates by their emulated halves (`[~]`, P9-1 and P9-4, hardware halves in the final review); P9-2 and P9-3 by the scenarios, locally and in CI. CI: per-push CI green, run 41991668112; full check green, run 42017826073 on tag `phase-10.5-r4` |
| **Completed** | 2026-11-05 - per-push CI green, run 41991668112; full check green, run 42017826073 on tag `phase-10.5-r4` |

> FINAL_PLAN §10 names this the single largest risk in the project: the only
> phase whose failure modes are controlled by third-party code. Open issues here
> are likely to be about *containment*, not features.

**What exists.** adX hosts CLAP and VST3 effects and instruments, every one of them in a
sandbox process (`adx_plugin_host.exe`): scanning is out of process with a cache and a
visible blacklist; audio crosses in shared memory against a deadline the audio thread
never waits past; a crash bypasses the insert and offers a restart, a hang is killed and
restarted with its state, a run of late blocks bypasses, NaN mutes. Plugin parameters
are laid out, automated, text-addressed and MIDI-learned like native ones; state saves as
one quoted base64 line beside the named parameters; PDC follows a plugin's latency,
including a change mid-play. MIDI learn binds any control to any parameter and saves in
a new `[MIDI]` section with the project's input routes. ASIO is compiled in (WASAPI stays
the default). In the app: a Plugins panel, a plugin window (embedded editor and the
generic one), a MIDI panel, MIDI learn in every parameter's menu, plugin state synced on
save and export. `adx_test_plugin` (CLAP and VST3) misbehaves in every way §4.8 lists and
each has a test. Fifteen corrections: phase_9.md §11.

**Observed locally (2026-10-30, the final tree).** ctest 100% on Debug (466, `-LE slow`),
RelWithDebInfo (467) and Release (467), the slow suite included in the optimised builds;
Phase completion in Debug: `ADX_FULL_EVIDENCE=1 ctest --preset windows-x64-debug -L slow`
passed in 1419.69 s; the 28 `[plugin]` tests three times back to back in Release; pytest 248
passed with `CI=1` (GPU tests deselected, nothing skipped); clang-tidy clean over all 331
files on all three compile databases; clang-format, headers, format-safety, positions,
dsp-math, `rules`, ruff, ruff format and mypy `--strict` clean. Launcher: `check_launcher:
adX.cmd opened adX 0.1.0 - 16 panels, audio device open`.

Inherited issues: **P8-3**'s "no automation-as-CC in MIDI export (needs MIDI learn's
mapping)" is unblocked - learn's bindings exist - but not built; re-carried in P8-3.
**P8-0**, **P8-1**, **P8-2**, **P8-4** untouched by Phase 9 and re-carried. **P1-1**
advanced: `plugin_alloc_does_not_affect_host` renders under the allocator guard with zero
violations while the plugin allocates in its process.

**Open issues for Phase 10:**

- **P9-0** · **Per-push CI green 2026-11-05** (run 41991668112); full check green (run 42017826073, tag `phase-10.5-r4`). Was: **BLOCKER for marking Phase 9 done** · Nothing is committed or pushed (and
  Phase 8 is in the same tree). Fixed when: per-push CI is green, and the full check is
  green on a `phase-9` tag - including `headless_render_with_plugins` - with both run ids
  recorded here and in phase_9.md §10. CI must build the fixture plugins and
  `adx_plugin_host` (they come with the test build) for the Python plugin tests not to
  skip there.
- **P9-1** · **HALF CHECKED `[~]` 2026-11-05; hardware half deferred to the final review;
  no longer blocks Phase 9** · Exclusive mode, `tools/measure_midi_latency.py --exclusive`:
  5.91 / 6.99 ms median at blocks 64 / 128 (12.49 at 256; phase_9.md §10). Was:
  `midi_roundtrip_latency`: a hardware keyboard to an audible note, measured
  under 10 ms. The emulated half is the loopMIDI run below; it becomes `[~]` when that
  measures under 10 ms (P9-9). The keyboard itself is listed under "Final review".
  The user has no MIDI keyboard (2026-10-30), so the software half has a tool:
  `tools/measure_midi_latency.py` drives `tools/midi_latency_probe.cpp`, which sends notes
  into a loopMIDI port and stamps them against WASAPI loopback of the output mix (both on
  QPC). It covers driver + input thread + callback + output buffer up to the mix, not a
  keyboard's USB link or the device buffer after the mix (printed as "endpoint"; 10 ms,
  shared mode's period, on this machine). Ran 2026-10-30 with loopMIDI: 67.75 / 69.45 /
  74.24 ms median to the mix at blocks 64 / 128 / 256 - **far over 10 ms**, nearly all of
  it audio output, not MIDI (P9-9). Numbers in phase_9.md §10. Evidence, not P9-1's close.
- **P9-9** · **Fixed 2026-11-05**: WASAPI exclusive mode (`WasapiExclusiveBackend`, in
  Preferences > Audio) measures 5.91 ms at block 64 with the same tool. Was: Output latency on WASAPI shared mode is about 60 ms
  on this machine: `Engine.audition` to the Windows mix measured 62.85 ms at block 64.
  MIDI in adds about 5 ms. Cause: the driver's only shared period is 480 frames with a
  1056-frame buffer; RtAudio 6.0.1 keeps that buffer and its own equal-size ring full,
  then the mix adds 10 ms. Shared mode cannot reach 10 ms on this device. Fix options: a
  WASAPI exclusive-mode backend (miniaudio is already a dependency and supports it), or
  ASIO (P9-4; FlexASIO in exclusive mode needs no hardware interface). Fixed when: the
  same tool measures under 10 ms on this machine.
- **P9-2** · **Closed 2026-11-05**: `scenario_real_plugins` 16 of 16 locally and green in
  the full check's scenarios job (run 42017826073); recorded in phase_9.md §6 and §10. Was:
  **BLOCKER; agent-run since 2026-11-01** (FINAL_PLAN §9) · Three real
  third-party plugins, VST3 and CLAP, instrument and effect: load, automate, save, reload
  (and the editor embeds or falls back). No longer waits for the user. Fixed when:
  `scenario_real_plugins` passes over the pinned open-source plugins of
  `tools/test_plugins.toml` (phase_10.5.md §4.6), with the list and output recorded in
  phase_9.md §10.
- **P9-3** · **Closed 2026-11-05**: `scenario_live_plugin_crash` green locally and in run
  42017826073. Was: **BLOCKER; agent-run since 2026-11-01** · A plugin crashing during playback,
  shown in the running app (load `adx_test_plugin`, set its `mode` to 1 while playing):
  playback of the rest continues, the notice appears, Restart brings it back. Fixed when:
  `scenario_live_plugin_crash` passes (phase_10.5.md §4.6).
- **P9-4** · **HALF CHECKED `[~]` 2026-11-05; hardware half deferred to the final review**
  · FlexASIO 1.10b (shared mode) over VB-Cable, `tools/measure_latency.py --asio`: takes 0
  samples off at 64/256/1024; adX's readout is the driver's own split. The run found that
  a duplex RtAudio stream reported twice its round trip; fixed (phase_9.md §11 16). Was:
  ASIO: the latency readout against a measured loopback. Emulated half, not yet
  run: FlexASIO (an open-source ASIO driver over WASAPI) with VB-Audio Virtual Cable as
  the loopback, `tools/measure_latency.py` opening the engine with `Engine(asio=True)`
  (the tool needs an `--asio` flag; the app has no audio-API preference yet). Installing
  FlexASIO needs the user's approval at the UAC prompt. A real interface's own driver is
  listed under "Final review".
- **P9-5** · **Fixed 2026-11-06** (local; not yet in CI): MIDI outputs (`MidiDevices::openOutput`, a sender thread that sends
  each message when its frame is heard), MIDI clock and MTC out on the audio clock
  (`MidiSync.h`: ClockSender, MtcSender), clock and MTC follow driving the arrangement's
  rate and position (`EngineCore::follow`), Preferences > MIDI outputs/sync/follow, and
  `send_midi` for Phase 11's LEDs. Tests: `test_midi_sync.cpp` (5 cases) and
  `test_midi_sync.py`, whose loopMIDI case has one engine follow another's clock. Was:
  MIDI output, sending clock to a device, clock follow wired to the
  arrangement's time source, and MTC (§3 step 3, §4.10). `MidiClock`'s sender and
  follower exist and are tested; nothing drives a device with them. Phase 11's LED
  feedback needs output.
- **P9-6** · **Fixed 2026-11-06** (local; not yet in CI): SMF bend in its RPN 0 range and MPE zones into pitch curves, slides
  and curves out as an MPE zone (`midi_bend_import_roundtrip`, `midi_export_slide_mpe`);
  live bend moves held notes; MIDI takes (`record/MidiTake`, the toolbar's MIDI button:
  there was no MIDI note recording at all) keep bend; UST, USTX and VSQX import
  (`adx/sung_import.py`, with lyrics, and UST/USTX pitch points); SMF lyric events both
  ways. PyYAML is a new dependency (USTX). Was: Pitch bend and MPE into
  `Note.pitchCurve` on input and import, and slides
  out as per-note bend on export (§4.1: `midi_bend_import_roundtrip`,
  `midi_export_slide_mpe`); UST/USTX/VSQX import.
- **P9-7** · **Fixed 2026-11-06** (local; not yet in CI): echoes carry a gesture flag (CLAP gesture begin/end, VST3
  performEdit); one equal to the project's value, or a value report in an instance's first
  half second without a gesture, is not an edit; the record toolbar's Auto button writes
  gestures as automation lanes (`project/AutomationWriter`); plugin instruments follow
  slides, pitch curves and the channel's pitch offset as per-note tuning (CLAP note
  expression, VST3 tuning). Tests: `test_plugin_automation.cpp`, `test_plugin_ui.py`.
  Was: A plugin editor's own parameter changes become values in the project, never
  automation lanes; there is no "write automation" mode. And plugin instruments ignore
  the channel's pitch offset and per-note glides (a plugin does its own pitch).
- **P9-8** · **Fixed 2026-11-06** (local; not yet in CI): the plugin window's bar (health, "Don't embed its editor" kept per
  plugin, Check latency, Restart), late blocks and state on each mixer slot, and a Restart
  button in the status bar on a crash notice. Was: The app's plugin UI is functional, not
  finished: no per-plugin "never embed"
  preference, no restart button in the notice (the menu's restart restarts every crashed
  child), no latency-check action (the engine has `plugin_check_latency`), no late-block
  counter in the mixer (the engine reports it: `Plugins.instances()`). FINAL_PLAN §5.6's
  "which plugin is making it stutter" belongs in Phase 10's polish.
- **P8-0**, **P8-1**, **P8-2**, **P8-3**, **P8-4** · `CARRIED` · As Phase 8 left them;
  P8-3's automation-as-CC is now buildable on learn's bindings.
- **P7-2** … **P7-6**, **P6-2**, **P6-3**, **P5-6**, **P4-10**, **P4-4**, **P4-5**,
  **P4-7**, **P4-8**, **P4-6** · `CARRIED` · Untouched by Phase 9. P6-2 grew:
  `mainwindow.py` gained two panels and the runtime.
- **P1-1** · `CARRIED` · `_CrtSetAllocHook` is not installed; the handover and departure
  tests do not run under the guard.

---

## [x] Phase 10 — Analysis, visualization, polish · L

| | |
|---|---|
| **Plan** | [phase_10.md](phase_10.md) |
| **Entry** | Phase 9 §6 complete |
| **Done when** | [phase_10.md](phase_10.md) §7 — **including `bench_full_project`** |
| **Status** | Done 2026-11-05. Every [phase_10.md](phase_10.md) §7 box observed: the bench's eight targets (P10-5, P10-6), the one-hour soak, the golden re-bless reviewed and listened to, a clean clone in 90 s, docs examples in CI. Per-push CI green (run 41991668112; 42017825970 at `384371e`); full check green, run 42017826073 on tag `phase-10.5-r4` |
| **Completed** | 2026-11-05 - CI run 41991668112; full check green, run 42017826073 on tag `phase-10.5-r4` |

> `bench_full_project` is a hard prerequisite for Phase 11, not a soft one. Do
> not hand off with it unmet and an open issue in its place.
>
> This file sizes the phase **L**; FINAL_PLAN §7 sizes it **M**. See
> [phase_10.md](phase_10.md) for why.

**Open so far (for whoever continues Phase 10):**

- **P10-1** · **Closed 2026-11-02, target revised** · Synthesized corpus 70 % -> 92.5 %
  exact (100 % with relatives) once minor keys also score against natural minor. The
  recorded corpus is GiantSteps Key (604 EDM previews, fetched and MD5-checked by
  `tools/fetch_key_corpus.py`, never committed): 39.2 % exact, 51.3 % with relatives,
  51.7 % MIREX weighted, which is where profile methods sit on it (the best CNNs reach
  about 75 % weighted). phase_10.md §6 and §11 revise the target with the numbers; the
  key is shown as a ranked suggestion with confidence, which tracks accuracy (24.5 % ->
  54.3 % exact from the lowest to the highest quartile).
- **P10-2** · **Closed 2026-11-02** · `AnalysisJob` and `bindings/analysis.cpp`; the
  Analysis panel runs key, chords, tempo, timbre, form and (ONNX builds) melody on notes or
  a file, each applied as one undo group (`tests/python/test_analysis_jobs.py`).
- **P10-3** · The spectrum analyzer reads the master only; §4.4's per-insert source needs
  a sample tap per insert (phase_10.md §11, 14). Fixed when: an insert's tap can feed the
  hub, or the source select is struck from §4.4.
- **P10-4** · Key accuracy on recordings is 51.7 % weighted. Improvements to try, measured
  with `tools/measure_key_corpus.py`: EDM-specific profiles, a tuning estimate, bass
  weighting tuned on recordings. Fixed when: measured better, or accepted as is.
- **P10-5** · **BLOCKER for Phase 11; the user's decision** · `bench_full_project` misses
  two of §4.5's targets: audio-thread CPU 58.9 % (< 50 %) and the worst block 96.8 % of
  the period (< 60 %), measured 2026-11-03 on an idle machine. Load, rebuild, xruns and
  allocations pass. The bit-exact optimizations are done (about 4 points); the rest is
  per-sample arithmetic, profiled per node type by `bench/profile_nodes.py` (FM, the pitch
  shifter, Overdrive, MultibandComp lead). The options, each with its cost, are in
  phase_10.md §11: output-changing DSP work with every changed hash re-blessed and
  reviewed (reaches the CPU target, not the worst block), a parallel scheduler that keeps
  the fixed summation order (a Phase 3-sized change), or a revised target. Fixed when:
  the bench passes, or the user revises §4.5's targets.
  **2026-11-04:** the user chose options 1 and 2 together. Both are written as code
  (phase_10.md §11, session of 2026-11-04).
  **2026-11-04, later: built, tested, measured.**
  - Debug, RelWithDebInfo and Release build. The fixes were C4324 pragmas in Crew.h and
    Scheduler.h, and 8 clang-tidy findings.
  - `test_parallel_render` passes in all three: serial and parallel hashes are the same.
    3 of 8 golden projects reach the crew, and the test now asserts at least 3.
  - Golden re-bless moved exactly 19 hashes: the 18 effect hashes of MultibandComp,
    Overdrive, PitchShifter, Saturation, SpectralFreeze and TransientShaper, plus
    `instrument/fm`. `hashes.txt`, `formats.txt`, every preset and every other unit are
    unchanged.
  - `bench_full_project` (Release, 3 min, idle machine): audio-thread CPU **8.3 %**,
    worst block **24.2 %** (p99 12.4 %), 0 xruns, load 0.26 s, rebuild 1.60 ms median
    (2.34 ms worst). That passes all six engine-side targets. It times the callback
    only: helpers' work is not counted, and total CPU across cores is not lower.
  - `profile_nodes.py`: a whole single-core render is 54.0 % of one core, down from
    60.8 %. That is the DSP changes alone, since the profiler forces serial.
  - Listening (judgement), 2026-11-04, on `build/p10-5_listen/ab/` and its `LISTEN.md`
    (PitchShifter is the one real change: the waveform differs by −22 dB, and a mono
    source now leaves a side channel at −45 dB RMS). The user: "the listens pass with no
    notes."
  - **Closed 2026-11-04** for the engine side. The bench's two UI and soak targets are
    tracked in P10-6.
- **P10-6** · Not yet run: `bench/bench_ui.py` (UI p99 with every panel open, on the GPU),
  `bench_one_hour_soak`, and the MSI's build and `installer_clean_install` on a fresh hosted
  runner (WiX 3.14 needs no install; the CI job is written with the full check). Fixed when:
  each has been run and recorded in phase_10.md §11.
  **2026-11-04:** `bench_ui.py --seconds 60` was run and failed.
  - Frame work passes: p99 0.45 ms, worst 0.69 ms.
  - The tick-to-tick interval does not: p99 27.3 ms against the 25 ms limit, with 3356
    frames in 60 s, about 56 Hz. Something outside the timed tick holds the main thread.
  - The run is also not the configuration §4.5 asks for: `analyzer_host.open` was only
    `["particles"]`. The six analyzers seem to share one tab group, and only the raised
    tab runs.
  - `bench_one_hour_soak` does not exist yet; it is only named in bench_full_project.py's
    docstring.
  **2026-11-04, later:**
  - `bench_ui.py` fixed twice. It now re-docks the six analyzers side by side, and it
    fails unless all six are open. `FrameClock` uses `Qt.PreciseTimer`: the coarse timer
    snapped to the 15.6 ms system tick, which is where the 56 Hz came from.
  - Re-run: frame work p99 0.76 ms (passes). The displayed interval still fails, at about
    44-45 fps with about 8 animated QQuickWidgets.
    - The GUI thread is about 95 % idle. Each QQuickWidget's sync and render are
      included in the 0.76 ms.
    - With only the mixer animating it holds 60 Hz. `timeBeginPeriod(1)` changes
      nothing, and floating the docks is worse.
    - The cost is Qt's widget composition and present path, not adX's work.
    - **The user's decision:** host the hot surfaces in a shared QQuickWindow (a UI
      change), or gate on work p99 (§4.5's literal target) and carry about 45 fps as a
      known limit. **The user chose the UI change (2026-11-04).**
  - **Fixed 2026-11-04.** The new `app/adx/panels/quick_host.py` replaces QQuickWidget
    in every hot surface (GeometryView, the piano roll, the waveform).
    - Each scene is a QQuickView in `QWidget.createWindowContainer`: a native child
      window that Qt's threaded render loop renders and presents off the GUI thread.
    - Input synthesized on the host is forwarded to the window. Drags the window
      receives go back to the host, where the playlist's filter handles them.
    - One window per surface rather than one shared window: the surfaces live in
      separate docks.
    - `bench_ui.py --seconds 30`: interval p99 **19.0 ms** (limit 25), work p99
      0.49 ms, all six analyzers open. **Passes.**
    - Presented frames per window, counted with `frameSwapped`: 62.5 fps on all eight
      animated surfaces (the clock's own rate), up from about 44.
  - Found on the way: `qoffscreen.dll` (the suite's platform) dereferences null
    destroying the main window after a dock holding a window container was floated
    (`test_layout_persists_and_resets`). The windows platform does the same steps
    cleanly. On offscreen, QuickHost holds a QQuickWidget instead: same API, same
    forwarding. The native path is covered by bench_ui, the launcher check and the
    scenarios, which run on the real display.
  - **`bench_one_hour_soak` written and passed** (`bench/bench_one_hour_soak.py`,
    60 min):
    - growth −0.01 MB/hour, last minus first 10-minute window −0.01 MB, 0 xruns, 0 late
      callbacks;
    - 1800 edits and undos with all six analyzers on;
    - private bytes about 991 MB from warm-up to the end.
  - The MSI job has not run: it needs a push and the full check (the user's approval).

**Open issues for Phase 11:**

Phase 10 and 10.5 together (2026-11-05). Neither phase's definition of done holds any of
these; each is re-carried with its reason.

- **P10-3** · `CARRIED` · The spectrum analyzer reads the master only; §4.4's per-insert
  source needs a sample tap per insert. Phase 11's live work adds per-insert taps for
  its own meters, which is where this fits. Fixed when: an insert's tap feeds the hub, or
  the source select is struck from phase_10.md §4.4.
- **P10-4** · `CARRIED` · Key detection on recordings is 51.7 % MIREX weighted
  (GiantSteps). P10-1 revised the target with the numbers; better profiles or a tuning
  estimate are improvements, not a gate. Fixed when: measured better with
  `tools/measure_key_corpus.py`, or accepted as is.
- **P10.5-2** · **Fixed 2026-11-06** (local; not yet in CI) with P9-7: once active, the CLAP flush is `process()` itself;
  idle() flushes only an inactive plugin. `test_real_plugins` now asserts a reopened
  project has nothing to undo. Was: `CARRIED` · CLAP `params.flush()` is called on the
  main thread while a
  plugin is active but not processing (Airwindows logs it). Moving it to `process()`
  needs echo filtering first; see the entry above. Fixed when: the flush runs on the
  audio thread with `test_real_plugins` passing and no spurious undo steps.
- **P10.5-3** · `CARRIED` · Two fresh launches can tab the docks differently (2 of 6 runs).
  The test compares every interactive widget, shown or not, so it no longer flakes; the
  cause is unexplained. Fixed when: the default layout is the same on every launch.
- **P10-7** · New · Real-window tests run on the second monitor and behind the user's
  windows (`ADX_SCREEN`, `ADX_BEHIND`, adx/screens.py). Behind mode makes Qt treat a
  window as active that Windows does not; the scenarios pass that way, but a test that
  depends on the OS focus (a real IME, a native file dialog) would need `ADX_BEHIND=0`.
  Fixed when: nothing; recorded so the next agent knows why a window is not in front.
- **P8-1, P9-1, P9-4** · `CARRIED` from their phases as hardware halves only (their
  emulated halves passed 2026-11-05; P9-9 is fixed). They wait for the final review.

---

## [x] Phase 10.5 — Agent-run testing · L

| | |
|---|---|
| **Plan** | [phase_10.5.md](phase_10.5.md) |
| **Entry** | Phase 9 built (it may run beside Phase 10's remaining work) |
| **Done when** | [phase_10.5.md](phase_10.5.md) §6 |
| **Status** | Done 2026-11-05. Every [phase_10.5.md](phase_10.5.md) §6 box observed; per-push CI green (run 41991668112; 42017825970 at `384371e`); full check green, run 42017826073 on tag `phase-10.5-r4`, scenarios job included |
| **Completed** | 2026-11-05 - CI run 41991668112; full check green, run 42017826073 on tag `phase-10.5-r4` |

> Added for FINAL_PLAN §9's rule "Every phase-ending check is agent-run". Software
> checks that used to need the user get a software path here. Listening and approval
> gates stay the user's.

**Inherits:** P9-2 and P9-3 (closed by this phase's scenarios); P8-1's leftover "the
engine's xrun count isn't exposed to Python" (`Engine.xruns()`, §3 step 1).

- P9-2: **closed locally** by `scenario_real_plugins`: Dexed, Surge XT (and its Effects
  bundle) and Airwindows Consolidated, VST3 and CLAP, on the stand-in and on WannaCry,
  through the GUI. 16 of 16 pass. To be recorded in phase_9.md §10 once CI agrees.
- P9-3: **closed locally** by `scenario_live_plugin_crash`, stand-in and WannaCry.
- P8-1's leftover: `Engine.xruns()` exists and is read by every scenario.

Evidence, 2026-11-03, this machine:
- ctest `-LE slow` 489/489 in Debug, RelWithDebInfo and Release;
- `ADX_FULL_EVIDENCE=1 ctest --preset windows-x64-debug -L slow` passed in 1797 s (P10.5-4);
- `CI=1 pytest tests/python`: all pass (3 skips are the ONNX and hardware cases);
- `pytest tests/scenarios`: 26 passed, 0 skipped;
- ruff, `mypy --strict`, and lint gates format/headers/format-safety/positions/dsp-math/rules/plans: clean;
- `check_launcher.py`: adX.cmd opened 24 panels, then drove the app, playing a project
  and capturing 48,384 frames with 0 xruns, and quit cleanly.

**Open issues (for whoever closes this phase):**

- **P10.5-0** · **Closed 2026-11-05**: committed and pushed (723dc0e onward); per-push CI green, run 41991668112, after two fixes it found (parallel ctest starved the timing tests on 4-core runners; reverted). Was: BLOCKER for Done · Nothing since `be487fe` is committed, so neither
  per-push CI nor the full check (with its new `scenarios` job) has seen this phase, or
  Phases 8-10 (P8-0, P9-0). Fixed when: committed and pushed with the user's go-ahead, and
  both runs are green with their ids recorded.
- **P10.5-2** · adX calls a CLAP plugin's `params.flush()` on the main thread while it is
  active but not yet processing; CLAP says the audio thread then (Airwindows logs
  "Host called the method clap_plugin_params.flush() on wrong thread"). Moving the flush
  into `process()` was tried and reverted. It exposed a second gap: values a plugin reports
  itself (`PluginCore`'s echoes) become project edits whether they come from its editor or
  from its post-load value report, so a reopened Airwindows project gained 22 "Set effect
  value" steps. Fixed when: echoes outside a CLAP gesture (or equal to the loaded state's
  values) are not recorded, and the flush moves to the audio thread with
  `test_real_plugins` still passing.
- **P10.5-3** · Two fresh launches can come up with docks tabbed differently. Seen as an
  extra QMainWindow tab bar and the piano roll's Slide button on or off screen, in
  `test_control_tree_names_stable`, in 2 of 6 full runs. The test now compares every
  interactive widget, shown or not. The layout difference itself is unexplained. Fixed
  when: the default layout is the same on every launch, or the cause is found and accepted.
- **P10.5-4** · **Closed 2026-11-05**: the `[.slow]` set is one ctest entry per case (cmake/AdxTesting.cmake), each with its own 1800 s timeout; the timing cases are RUN_SERIAL. Release `-L slow -j 8`: 14/14 in 300 s. Was: The Debug `[.slow]` suite takes 1797 s against ctest's 1800 s default.
  Under load it times out, and its 60 s realtime gate misses deadlines (167 callbacks over
  5.33 ms, seen while other jobs ran). Run it on an idle machine. Fixed when: it has a
  timeout with headroom, or the suite is split.
  **2026-11-04:** with the parallel scheduler (P10-5) the same suite passed in 953 s on
  an idle machine. That leaves headroom under 1800 s for now; it has not been checked
  under load.

**Open issues for Phase 11:** listed under Phase 10 above (P10.5-2, P10.5-3); P10.5-0 and P10.5-4 are closed.

---

## [ ] Phase 11 — Live performance · L

**The final stage.**

| | |
|---|---|
| **Plan** | [phase_11.md](phase_11.md) |
| **Entry** | Phase 10 §7 complete, **including `bench_full_project`**, and Phase 10.5 §6 complete |
| **Done when** | [phase_11.md](phase_11.md) §6 - the pad-controller boxes by their emulated halves; the physical controller is in the final review |
| **Status** | Built and observed locally 2026-11-08: every [phase_11.md](phase_11.md) §6 box, the 20-minute set and the walkthrough by their emulated halves (`[~]`). CI green, run 42494990727 at `0c6c614`; the tag-triggered full check not run yet |
| **Completed** | — |

> Week one validates the Phase 3 transport checkpoint before anything is built
> on it ([phase_11.md](phase_11.md) §3).

**What exists (2026-11-06, overnight; local, not in CI).** The week-one spike passed, and
the session's engine core is built and tested; pads, performance recording and the gates
are not. Details and evidence in [phase_11.md](phase_11.md) §10.

- **Spike (§3): the checkpoint held, with one gap.** `spike_two_sources_render` renders two
  sources with their own positions, rates and loops for 30 s through the existing Scheduler,
  every onset on its hand-computed frame. The gap: a graph node is bound to one source, so
  one channel could not play the arrangement *and* a launched clip. Closed by composition,
  not surgery: `graph::SessionLane` (a clip's events merged into its channel's per piece,
  on the clip's own source) and `TimeSource::requestSeekSamples` (a source started at -k
  puts tick 0 on frame k of a block). Nothing in `engine/transport/` changed shape.
- **`engine/session/`:** `LaunchQuantize`, `ClipState` (the five states, table-tested),
  `FollowAction` (seven, Random seeded), `SessionEngine` (launch/stop/scene/stop-all on
  the arrangement's grid or the session's own clock, exact sample offsets, one clip per
  track, stop as a release at the boundary with the source held for its tail, follow
  actions, rebind on snapshot swap, 128 runtimes with refusals counted), `TapTempo` and
  `nudgedRate`. `engine/session` joined lint's realtime paths.
- **The model and the text:** `project::SessionModel` (scenes, tracks, cells), `[SESSION]`
  parsed and written losslessly, `SetSession` (one undo step), `dirty::kSession`; the
  snapshot compiles each cell's notes (`SessionClip`).
- **App:** `Engine.launch_clip/stop_clip/fire_scene/stop_all_clips/clip_state/clip_progress`,
  `Project.session_info/set_session`, and the Session dock (a widget grid: click to launch,
  right-click to edit cells, scenes and stops).
- **Tests:** `test_spike.cpp`, `test_state_machine.cpp` (state machine, quantize grid,
  follow, tap tempo, nudge), `test_session_format.cpp`, `test_session_engine.cpp`
  (launch_quantize_exact at 4 tempos x 4 quantizes, instant, scene, stop_quantized,
  relaunch_before_boundary, clip_stop_preserves_other_clips_voices, follow, tails,
  follow-versus-stop), `test_session_panel.py`.

**What was added 2026-11-07/08 (closes P11-1..P11-8 below).** Pads (`PadController`,
`PadFeedback` on its own 30 Hz thread, `.adxpad` profiles with learn mode, velocity to clip
gain, `tools/virtual_pad.py`); `PerformanceRecorder` (commit is one command); mini-notation
cells; clip gain and scene tempo applied; clips follow the master rate, with nudge and tap
on the transport bar; tails end when the last voice is silent; the session grid on the
geometry pipeline (`SessionGeometry`: progress rings, queue marks, one build per frame);
`docs/performance.md`; the gates. Every phase_11.md §5 test exists. Evidence in
[phase_11.md](phase_11.md) §10.

**Closed:**

- **P11-1** · Pad controllers: built; `pad_profile_parse`, `pad_learn_builds_profile`,
  `pad_velocity_to_gain`, `pad_feedback_thread`, `virtual_pad_drives_a_set`.
- **P11-2** · Performance recording: `performance_record_roundtrip` is bit-identical
  (0 samples differ over 60 bars); with three nudges every onset is within a sample.
- **P11-3** · Launchable mini-notation cells: `mini_clip_cycles_independently`. Audio
  clips in cells are not part of §5.11; carried below.
- **P11-4** · Clip gain and scene tempo applied: `clip_gain_scales_velocity`,
  `scene_tempo_change`.
- **P11-5** · Clips follow rate changes: `clips_follow_the_master_rate`,
  `launch_across_tempo_change`; nudge and tap wired (`tap_tempo_through_the_engine`).
- **P11-6** · Release, not cut: `clip_stop_releases_not_cuts`,
  `clip_stop_preserves_effect_tail`, `time_source_released_after_tail`,
  `relaunch_legato_keeps_phase`, `concurrent_clip_limit`, `session_no_alloc` (10 min).
- **P11-7** · The session grid is geometry (`session_geometry_budget`). Dragging is
  carried below.
- **P11-8** · The gates pass: `live_set_no_dropouts`, `scenario_scripted_set_20min`
  (both materials), `scenario_performance_walkthrough`.

**Open issues remaining at completion (carried into Phase 12):**

- **P11-9** · Cells hold patterns and mini-notation only; no audio clips. Dragging between
  playlist and session (phase_11.md §4.9) is not built. Neither is in §5.11 or §6. Fixed
  when: an audio cell launches on the grid like a pattern cell, and a playlist item
  dragged into the grid becomes a cell (and back).
- **P11-10** · A recorded performance is not sample-identical to the capture when a VA
  or DrumSynth voice plays: their oscillator start phase is seeded from the placement id,
  and recording makes new placements. `scenario_scripted_set_20min` therefore compares
  10 ms RMS envelopes (stand-in correlation 0.955 against a 0.95 floor; WannaCry 0.9986).
  `performance_record_roundtrip` is bit-identical because its instruments don't seed
  phase. Fixed when: phase seeding follows the source pattern and note, not the
  placement, and the scenario compares samples.
- **P11-11** · Bit-identity holds only without rate changes. With nudges or taps, rate
  and tempo-event rounding differ, so onsets match within one sample, not bit for bit
  (`performance_record_roundtrip_with_nudges`). Accepted for now. Fixed when: the
  recorder writes tempo events that reproduce the rate's sample positions exactly.
- **P11-12** · The `.adx` parser rejects CRLF line endings in some sections (found
  splicing test projects). Fixed when: a CRLF copy of every golden project parses to the
  same model.
- **P11-13** · pytest's 240 s `faulthandler_timeout` prints a stack dump during the
  20-minute set (not a failure). Fixed when: the long scenarios set their own timeout
  marker.
- **P11-14** · **Closed 2026-11-08**: `scenario_live_plugin_crash[standin]` failed 3 of 5
  local runs. Airwindows, in the shared sandbox, missed its deadline (0-6 late blocks in
  5 s), and a late block is silence. The load came from a test instance that leaked from
  an aborted set run and played on about 3 cores. With it gone: 0 late blocks in 8 runs,
  with and without render workers, and the scenario passed 10 of 10 (5 runs, both
  materials).
- **P11-15** · Playing the small stand-in on the timer backend costs about 2.8 cores on
  this 32-thread machine, and about 1.1 with `ADX_RENDER_WORKERS=0`. The 15 crew helpers
  (P10-5) wake for every piece and spin 50 µs after each one, whether or not the piece
  has work for them. It doesn't cause dropouts (`live_set_no_dropouts` passed), but it
  is wasted CPU. Fixed when: a piece wakes at most as many helpers as its plan is wide,
  and a one-step piece runs inline.

Phase 11 closes the DAW. Anything still open when it closes is fixed here or
carried into Phase 12 with a reason, like any other handoff.

---

## [ ] Phase 11.5 — M-Vave FM-1: full hardware support and native DX7 voices · XL

| | |
|---|---|
| **Plan** | [phase_11.5.md](phase_11.5.md) |
| **Entry** | Phase 9 §6 complete (hard: MIDI I/O, clock, learn, plugin host, external audio input). Phase 11 is soft: only the keys-as-launcher and the LED feedback use it |
| **Done when** | [phase_11.5.md](phase_11.5.md) §6. The dx7 port is bit-identical to upstream msfa, every voice parameter is editable, and import, live edit, external parts, sequencer capture, FX match, keys/LEDs and firmware install pass by their emulated halves. The real device (import, the sound-alike capture plus listening verdict, parts, sequencer, LEDs, USB audio, one real flash cycle) is in the final review |
| **Status** | Not started |
| **Completed** | — |

> Added 2026-11-05; rewritten 2026-11-08 at the user's request ("fully support
> everything the FM-1 supports from its hardware"; patches must arrive as
> editable native synths). Does not block and is not blocked by Phases 12–19.

**Open issues remaining at completion:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 12 — Sound-design foundation · XL

| | |
|---|---|
| **Plan** | [phase_12.md](phase_12.md) |
| **Entry** | Phase 11 §6 complete; the post-Phase-11 golden set recorded as the baseline |
| **Done when** | [phase_12.md](phase_12.md) §6, including its human gate |
| **Status** | Not started |
| **Completed** | — |

> Tranches A (format/model), B (DSP), C (tooling). Phase 13 may start once
> phase_12.md §8's handoff items are done.

**Inherits:** P11-9 to P11-13 and P11-15 (listed under Phase 11 above); P10.5-2 and P10.5-3.

**Open issues for Phase 13:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 13 — Core Drums & Machines · M

| | |
|---|---|
| **Plan** | [phase_13.md](phase_13.md) |
| **Entry** | phase_12.md §8 handoff items |
| **Done when** | [phase_13.md](phase_13.md) §6 |
| **Status** | Not started |
| **Completed** | — |

**Open issues for Phase 14:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 14 — Hyperpop · L

| | |
|---|---|
| **Plan** | [phase_14.md](phase_14.md) |
| **Entry** | Phase 13 §6 complete; Phase 12 fully done |
| **Done when** | [phase_14.md](phase_14.md) §7 |
| **Status** | Not started |
| **Completed** | — |

**Open issues for Phase 15:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 15 — Hardcore · L

| | |
|---|---|
| **Plan** | [phase_15.md](phase_15.md) |
| **Entry** | Phase 14 §7 complete |
| **Done when** | [phase_15.md](phase_15.md) §7 |
| **Status** | Not started |
| **Completed** | — |

**Open issues for Phase 16:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 16 — Classical · XL

| | |
|---|---|
| **Plan** | [phase_16.md](phase_16.md) |
| **Entry** | Phase 15 §7 complete; libraries fetched and verified |
| **Done when** | [phase_16.md](phase_16.md) §7 |
| **Status** | Not started |
| **Completed** | — |

**Open issues for Phase 17:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 17 — Ambient · M

| | |
|---|---|
| **Plan** | [phase_17.md](phase_17.md) |
| **Entry** | Phase 16 §7 complete |
| **Done when** | [phase_17.md](phase_17.md) §7 |
| **Status** | Not started |
| **Completed** | — |

**Open issues for Phase 18:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 18 — Breakcore / Jungle / DnB · M

| | |
|---|---|
| **Plan** | [phase_18.md](phase_18.md) |
| **Entry** | Phase 17 §7 complete |
| **Done when** | [phase_18.md](phase_18.md) §7 |
| **Status** | Not started |
| **Completed** | — |

**Open issues for Phase 19:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 19 — Phonk + Vocal · M

| | |
|---|---|
| **Plan** | [phase_19.md](phase_19.md) |
| **Entry** | Phase 18 §7 complete |
| **Done when** | [phase_19.md](phase_19.md) §8 |
| **Status** | Not started |
| **Completed** | — |

**Open issues remaining at completion:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Final review — after Phase 19

| | |
|---|---|
| **Plan** | [FINAL_PLAN.md](../FINAL_PLAN.md) §7 "Final review", §9 "Hardware testing is done at the end" |
| **Entry** | Phase 19 done |
| **Done when** | every deferred hardware half below has passed on real hardware with its output in its phase log, and every Done phase still meets its own definition of done on the finished tree |
| **Status** | Not started |
| **Completed** | — |

Set 2026-10-30, at the user's request: hardware testing happens here, all of it, and
nowhere earlier. A hardware check's emulated half runs in its own phase; once that passes
the box is `[~]`, counts toward the phase being done, and its hardware half is listed
here. Until this review starts, nobody asks the user to run one.

**Deferred hardware halves** (run in this order; each line says what the emulated half
already showed and what the hardware run must add):

| Id | Phase box | Emulated half | Hardware half still to run |
|---|---|---|---|
| P8-1 | phase_8.md §6 `record_latency_alignment` | `[~]` passed 2026-10-30: VB-Audio Virtual Cable, offset 0 at 64/256/1024 after 5 s of settling (phase_8.md §10) | a physical loopback (interface out 1 → in 1, or a 3.5 mm headphone-to-mic cable), direct monitoring off: `py -3.12 tools/measure_latency.py` |
| P9-4 | phase_9.md §6 ASIO | `[~]` passed 2026-11-05: FlexASIO 1.10b (shared mode) over VB-Cable, `tools/measure_latency.py --asio`, take offset 0 at 64/256/1024 and the readout equal to the driver's (phase_9.md §10) | a real interface's own ASIO driver, against a physical loopback |
| P9-1 | phase_9.md §6 `midi_roundtrip_latency` | `[~]` passed 2026-11-05: loopMIDI in, WASAPI exclusive out, 5.91 ms median at block 64 and 6.99 at 128 (`tools/measure_midi_latency.py --exclusive`); shared mode is ~68-74 ms | a hardware MIDI keyboard to an audible note, under 10 ms |
| P11 | phase_11.md §6 the 20-minute set; the `docs/performance.md` walkthrough | passed 2026-11-08: `scenario_scripted_set_20min` (stand-in and WannaCry, real audio device, 0 xruns, LEDs match) and `scenario_performance_walkthrough`, through `tools/virtual_pad.py` on loopMIDI | the same set and walkthrough on a physical pad controller with LED feedback |

**Then the review of every Done phase:** each phase's §6/§7 checked again on the finished
tree, by an agent. The human gates the user closed before 2026-11-01 (P6-1, P7-1) are
re-checked through their Phase 10.5 stand-ins, not by asking the user again. Anything that no longer holds reopens as an open issue against that phase, with a
`FR-` id (FR-1, FR-2, ...).

**Findings:**

- _to be filled in by the agent that runs the review_
