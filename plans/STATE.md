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
| 0 — Foundation | S | In progress — local DoD complete 2026-09-12, CI unrun |
| 1 — RT core | M | Not started |
| 2 — Project model, commands, `.adx` v2 | L | Not started |
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

## [ ] Phase 0 — Foundation · S

| | |
|---|---|
| **Plan** | [phase_0.md](phase_0.md) |
| **Entry** | Repo in post-archive state: `FINAL_PLAN.md`, `plans/`, `docs/`, `_archive/`, `.gitignore`. Nothing else. |
| **Done when** | [phase_0.md](phase_0.md) §6 — every box, including each CI gate *observed to fire* |
| **Status** | In progress (2026-09-12). Every §6 box observed to pass locally; the two CI boxes are open. |
| **Completed** | — |

The skeleton is built and every gate has been broken and seen to fail — locally,
against the same pinned binaries and the same `tools/lint.py` entry points CI
invokes. The evidence table is in [phase_0.md](phase_0.md) §6.

What is **not** done is CI itself: `.github/workflows/ci.yml` has never executed,
because nothing has been pushed. §6 asks for "all ten CI steps green on Debug and
Release" and for each gate to be seen going red *in CI*, and neither can be
claimed from a local run. That is **P0-6**, and it is the one box between this
phase and Done.

Three statements in [phase_0.md](phase_0.md) §3–§4 turned out to be wrong and were
corrected rather than worked around — a nonexistent compiler flag, a dependency
cache layout that cannot support two build trees, and a per-target language
standard that breaks the Catch2 link. All three are written up in
[phase_0.md](phase_0.md) §10, because §4 binds every later phase.

**Open issues for Phase 1:**

- **P0-6** · `BLOCKER` for calling Phase 0 done (not for starting Phase 1) ·
  `.github/workflows/ci.yml` has never run. `ilammy/msvc-dev-cmd`, the `.deps`
  cache key, and whether `ninja` is on the `windows-latest` image are all
  unverified, as is every gate's behaviour *in CI* rather than on a developer
  machine. Fixed when: a push shows both matrix jobs green, and one deliberate
  breakage of each gate is seen to turn a CI run red.

- **P0-2** · The clang-tidy RT ban list is **header-level only**:
  `portability-restrict-system-includes` (the banned `<header>` set),
  `cppcoreguidelines-no-malloc`, `cppcoreguidelines-owning-memory`,
  `bugprone-exception-escape`, `misc-no-recursion`. The identifier-level half of
  [phase_0.md](phase_0.md) §4.6 — `printf`, `std::cout`, `new`, `delete`,
  `malloc`, `throw`, `std::function`, `std::string`, `std::mutex`,
  `std::lock_guard`, `std::shared_ptr` banned *by name* — is not implemented;
  §4.6 and §8 assign it to this phase, which is the first one with real RT
  headers to test it against. Note that `<cstdio>` is deliberately **not** in the
  header ban list, because `engine/core/Config.h` includes it for the default
  assertion handler and realtime code includes that header — an identifier-level
  `printf` ban is what actually closes that hole. Fixed when: `.clang-tidy-rt`
  bans each named identifier and a probe under `engine/rt/` is observed to trip
  each one.

- **P0-3** · The realtime path list exists twice: `RT_PATHS` in `tools/lint.py`
  (which does the work) and a prose list in `.clang-tidy-rt`'s header comment.
  The duplication exists only because clang-tidy's native scoping mechanism is a
  `.clang-tidy` file inside each realtime directory, and Phase 0 was forbidden
  from creating those directories. Phase 1 creates `engine/rt/`. Fixed when:
  either `engine/rt/.clang-tidy` replaces the driver's scoping and `RT_PATHS`
  loses that entry, or the prose list is deleted and the driver is named as the
  single source.

- **P0-5** · An editable install does not rebuild the extension.
  scikit-build-core builds into `build/skbuild-*`, which is a different tree from
  `build/windows-x64-*`, so after touching C++ you must re-run
  `pip install -e ".[dev]"` before `pytest` sees the change — otherwise the test
  suite silently exercises a stale `.pyd`. `editable.rebuild` was left off
  deliberately: it would need the MSVC environment present at *import* time and
  would make `pytest` fail in any ordinary shell. This will bite Phase 1 on its
  first day, since Phase 1 is the first phase with C++ that Python tests assert
  on. Fixed when: one documented command does build-then-reinstall, or the
  README says plainly that C++ changes need a reinstall.

- **P0-1** · `ADX_GIT_SHA` is captured at configure time, so a build made after a
  commit without reconfiguring reports the previous commit. Harmless for a version
  banner, not harmless once a golden render hash is attributed to a commit
  (FINAL_PLAN §9). Fixed when: the SHA is refreshed as part of the build, or
  `adx::gitSha()` stops claiming provenance it cannot guarantee.

- **P0-4** · `ruff` and `mypy` are floor-pinned (`>=0.6`, `>=1.11`) exactly as
  [phase_0.md](phase_0.md) §3.6 specifies, while clang-format and clang-tidy are
  pinned exactly. A ruff or mypy release that adds a rule therefore turns CI red
  on unchanged code, which is the failure mode the clang pins exist to prevent.
  Not changed here because §3.6 states the floors explicitly. Fixed when: either
  both are exact-pinned with a deliberate bump step, or §3.6 records the floor as
  an intentional choice.

---

## [ ] Phase 1 — RT core · M

| | |
|---|---|
| **Plan** | [phase_1.md](phase_1.md) |
| **Entry** | Phase 0 §6 complete |
| **Done when** | [phase_1.md](phase_1.md) §6 |
| **Status** | Not started |
| **Completed** | — |

> The allocator hook's positive-control test gates every phase after this one.
> If it has never been seen to fail, it is not a gate.

**Open issues for Phase 2:**

- _to be filled in by the agent that completes this phase_

---

## [ ] Phase 2 — Project model, commands, `.adx` v2 · L

| | |
|---|---|
| **Plan** | [phase_2.md](phase_2.md) |
| **Entry** | Phase 1 §6 complete |
| **Done when** | [phase_2.md](phase_2.md) §6 |
| **Status** | Not started |
| **Completed** | — |

**Open issues for Phase 3:**

- _to be filled in by the agent that completes this phase_

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
