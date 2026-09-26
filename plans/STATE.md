# adX — STATE

**This file is the running ledger of phase completion. It is the first thing an
agent reads when picking up work, and the last thing it writes before finishing.**

## How this file works

This is a checklist of all twelve phases (0 through 11). The agent that
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
   checked — not "should pass", *observed to pass*.
2. Change the phase heading's `[ ]` to `[x]` and set **Status** to `Done` with
   the date.
3. Fill in **Open issues for Phase N+1**. If there are genuinely none, write
   `none` — do not leave the placeholder text.
4. Set the same phase's row in [FINAL_PLAN.md](../FINAL_PLAN.md) §10 to Done
   with the date. Both files must agree; this file is the working ledger, §10 is
   the permanent record.

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
| 2 — Project model, commands, `.adx` v2 | L | **Done locally** 2026-09-25 · CI pending |
| 3 — Audio graph & scheduling | L | Not started |
| 4 — Instruments & effects | XL | Not started |
| 5 — Frontend foundation | L | Not started |
| 6 — The DAW proper | XL | Not started |
| 7 — Text-first layer | M | Not started |
| 8 — Audio & export | M | Not started |
| 9 — Plugins & MIDI | L | Not started |
| 10 — Analysis, visualization, polish | L | Not started |
| 11 — Live performance | L | Not started |

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
| **Status** | Done locally (2026-09-25) — **CI pending**: not yet pushed |
| **Completed** | 2026-09-25 (locally) |

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

Not yet observed: the same in CI. The phase is recorded as done locally only, because
Phase 1's record already shows what happens when "verified locally" is written down
as "green in CI".

**Open issues for Phase 3:**

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

## [ ] Phase 3 — Audio graph & scheduling · L

| | |
|---|---|
| **Plan** | [phase_3.md](phase_3.md) |
| **Entry** | Phase 2 §6 complete |
| **Done when** | [phase_3.md](phase_3.md) §7 |
| **Status** | Not started |
| **Completed** | — |

> Carries the Phase 11 transport checkpoint. Read [phase_3.md](phase_3.md) §2
> before writing any code — a transport that forecloses live performance is not
> recoverable in Phase 11.

**Open issues for Phase 4:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 4 — Instruments & effects · XL

| | |
|---|---|
| **Plan** | [phase_4.md](phase_4.md) |
| **Entry** | Phase 3 §7 complete |
| **Done when** | [phase_4.md](phase_4.md) §7 |
| **Status** | Not started |
| **Completed** | — |

> Three tranches ([phase_4.md](phase_4.md) §3). Phase 5 may start once
> **Tranche A** is complete; B and C can run alongside it. If you hand off at
> Tranche A, say so in Status and list what B and C still owe.

**Open issues for Phase 5:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 5 — Frontend foundation · L

| | |
|---|---|
| **Plan** | [phase_5.md](phase_5.md) |
| **Entry** | Phase 4 Tranche A complete ([phase_4.md](phase_4.md) §3) |
| **Done when** | [phase_5.md](phase_5.md) §6 — **including the frame-rate acceptance step** |
| **Status** | Not started |
| **Completed** | — |

> Where FINAL_PLAN §2.2's bet is proven or disproven. If the three rules do not
> hold on the piano roll, that is a FINAL_PLAN-level finding, not an open issue
> — escalate rather than proceeding to Phase 6.

**Open issues for Phase 6:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 6 — The DAW proper · XL

| | |
|---|---|
| **Plan** | [phase_6.md](phase_6.md) |
| **Entry** | Phase 5 §6 complete, including the frame-rate acceptance step |
| **Done when** | [phase_6.md](phase_6.md) §6 |
| **Status** | Not started |
| **Completed** | — |

**Open issues for Phase 7:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 7 — Text-first layer · M

| | |
|---|---|
| **Plan** | [phase_7.md](phase_7.md) |
| **Entry** | Phase 6 §6 complete |
| **Done when** | [phase_7.md](phase_7.md) §7 |
| **Status** | Not started |
| **Completed** | — |

> The half no other DAW has (FINAL_PLAN §1). Deferred scope here costs more than
> deferred scope anywhere else — be specific in the open issues.

**Open issues for Phase 8:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 8 — Audio & export · M

| | |
|---|---|
| **Plan** | [phase_8.md](phase_8.md) |
| **Entry** | Phase 7 §7 complete |
| **Done when** | [phase_8.md](phase_8.md) §6 |
| **Status** | Not started |
| **Completed** | — |

**Open issues for Phase 9:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 9 — Plugins & MIDI · L

| | |
|---|---|
| **Plan** | [phase_9.md](phase_9.md) |
| **Entry** | Phase 8 §6 complete |
| **Done when** | [phase_9.md](phase_9.md) §6 |
| **Status** | Not started |
| **Completed** | — |

> FINAL_PLAN §10 names this the single largest risk in the project: the only
> phase whose failure modes are controlled by third-party code. Open issues here
> are likely to be about *containment*, not features.

**Open issues for Phase 10:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 10 — Analysis, visualization, polish · L

| | |
|---|---|
| **Plan** | [phase_10.md](phase_10.md) |
| **Entry** | Phase 9 §6 complete |
| **Done when** | [phase_10.md](phase_10.md) §7 — **including `bench_full_project`** |
| **Status** | Not started |
| **Completed** | — |

> `bench_full_project` is a hard prerequisite for Phase 11, not a soft one. Do
> not hand off with it unmet and an open issue in its place.
>
> This file sizes the phase **L**; FINAL_PLAN §7 sizes it **M**. See
> [phase_10.md](phase_10.md) for why.

**Open issues for Phase 11:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 11 — Live performance · L

**The final stage.**

| | |
|---|---|
| **Plan** | [phase_11.md](phase_11.md) |
| **Entry** | Phase 10 §7 complete, **including `bench_full_project`** |
| **Done when** | [phase_11.md](phase_11.md) §6 |
| **Status** | Not started |
| **Completed** | — |

> Week one validates the Phase 3 transport checkpoint before anything is built
> on it ([phase_11.md](phase_11.md) §3).

**Open issues remaining at completion:**

- _to be filled in by the agent that completes this phase_

There is no Phase 12. Anything still open when this phase closes is either fixed
here or promoted to a FINAL_PLAN-level decision about what adX does next.
