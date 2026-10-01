# Phase 2 — Project model, commands, `.adx` v2 · L

| | |
|---|---|
| **Status** | Done (2026-09-25) · CI unconfirmed, see STATE.md P2-0 |
| **Governs** | `engine/core/` (ids, time, tempo), `engine/project/`, `engine/format/adx/`, `docs/adx-format-v2.md`, the headless CLI |
| **FINAL_PLAN refs** | §3.1 (AdxParser), §3.2 (automation evaluator), §3.3 items 7, 8, 11, 12, §4 in full, §6 in full, §7 Phase 2 |
| **Entry criteria** | [phase_1.md](phase_1.md) §6 complete |
| **Next** | [phase_3.md](phase_3.md) |
| **§5 coverage owned** | §5.1 model + commands + undo · §5.8 format, `adx validate`, `adx fmt`, `adx diff` · §6 in full |

---

## 1. Objective

Replace iteration one's flat `Track` — which was simultaneously an instrument, a
pattern, a playlist lane and a mixer strip (FINAL_PLAN §3.3.7, the structural
reason the app could not become a DAW) — with the FINAL_PLAN §4 decomposition;
put every mutation behind a command so undo exists from the first commit; and
make `.adx` v2 a format that round-trips losslessly.

This phase is main-thread only. Nothing here runs on the audio thread. It is
nonetheless the phase that decides whether Phase 3 is possible, because Phase 3
renders a *projection* of this model and can only do so without allocating if
this model is shaped to allow it.

**No audio in this phase.** The deliverable is a CLI and a test suite. Phase 3
makes it play.

---

## 2. Deliverables — exact file manifest

```
engine/core/
  Ids.h                 strong id types, generation-free monotonic counters
  Time.h                Ticks / Beats / Samples, PPQ, conversions
  TempoMap.h/.cpp       tempo + meter changes, exact tick<->sample mapping
  Rational.h            exact musical fractions for quantize grids
  Curve.h/.cpp          the shared evaluator (fixes §3.3.12 and §3.2)

engine/project/
  Project.h/.cpp        the aggregate + id allocation + invariant checks
  Channel.h             instrument instance + routing + arp + polyphony
  Pattern.h             NoteClip[] + AutomationClip[] + optional mini-notation source
  Note.h                one note, with per-note modulation lanes
  Playlist.h            PlaylistTrack[] -> Item[]
  Mixer.h               Insert[] -> Slot[], Route[], sends
  Resources.h           sample pool, patch library, path resolution
  ParamRegistry.h/.cpp  name path <-> ParamRef, descriptors, units
  Automation.h          AutomationClip, Breakpoint, lane evaluation
  Validate.h/.cpp       invariant checker used by tests and by load

engine/project/commands/
  Command.h             the interface + CommandResult
  CommandStack.h/.cpp   undo/redo, grouping, coalescing
  ChannelCommands.h/.cpp
  PatternCommands.h/.cpp
  NoteCommands.h/.cpp
  PlaylistCommands.h/.cpp
  MixerCommands.h/.cpp
  AutomationCommands.h/.cpp
  ProjectCommands.h/.cpp   tempo, meter, markers, metadata

engine/format/adx/
  Token.h               token kinds + source spans
  Lexer.h/.cpp          line/section/key/value lexing; comment rule (§4.3)
  Document.h/.cpp       the lossless concrete syntax tree (§4.2)
  Parser.h/.cpp         Document -> Project
  Writer.h/.cpp         Project + Document residue -> text
  V1Shim.h/.cpp         v1 Document -> v2 Project (§4.8)
  Diagnostics.h/.cpp    Severity, code, line, column, length, hint
  NoteName.h/.cpp       "F#5" <-> MIDI; ported lexing discipline

bindings/
  project.cpp           Project handle, command submission, undo/redo
  format.cpp            load/save/validate/format/diff, diagnostics as dataclasses

app/adx/
  cli.py                subcommands: validate, fmt, diff  (render lands in P8)

docs/
  adx-format-v2.md      NORMATIVE. Written first, implemented second.

tests/cpp/project/      test_ids, test_time, test_tempomap, test_curve,
                        test_commands, test_undo_random, test_validate
tests/cpp/format/       test_lexer, test_parser, test_writer, test_roundtrip,
                        test_v1shim, test_diagnostics
tests/python/           test_project_api.py, test_cli.py, test_examples.py
```

---

## 3. Order of work

The order matters and is not arbitrary:

1. `engine/core/` — ids, time, tempo map, curves. Everything depends on these.
2. `docs/adx-format-v2.md` — **the spec before the parser.** Writing the parser
   first is how iteration one ended up with a format nobody could document
   (`docs/adxFormat.md` describes maybe half of what the v1 parser actually
   accepts).
3. `engine/project/` — the model, with `Validate.h` written alongside it.
4. Commands — every mutation, no exceptions, including the ones the parser uses.
5. `Document` + `Lexer` — the lossless layer.
6. `Parser` + `Writer` — round-trip, tested before v1 is touched.
7. `V1Shim` — last, because it is defined in terms of the v2 model.
8. CLI + bindings.

---

## 4. Design

### 4.1 Time

**Musical time is integer ticks. `PPQ = 3840`.**

```cpp
namespace adx::core {
inline constexpr int64_t kPpq = 3840;          // ticks per quarter note

struct Ticks { int64_t v = 0; /* +,-,*,/,<=> */ };
struct Samples { int64_t v = 0; };
using Beats = double;                          // UI/display only, never storage
}
```

*Why integers:* float beats accumulate error under repeated edits, make
`a + b == b + a` unreliable, and make "is this note exactly on the grid?" a
tolerance question. Iteration one used `float startBeat` and paid for it in
quantize and loop-wrap arithmetic.

*Why 3840:* `3840 = 2^8 × 3 × 5`. It divides exactly by 2, 3, 4, 5, 6, 8, 10,
12, 15, 16, 20, 24, 30, 32, 40, 48, 60, 64, 80, 96, 120, 128, 160, 192, 240,
256, 320, 384, 480, 640, 768, 960, 1280, 1920 — every grid and tuplet a musician
actually uses, including 128th-note quintuplets.

**Stated limitation:** no PPQ divides by 7, so a 7-tuplet rounds. Mini-notation
(`bd*7`) therefore rounds to the nearest tick. This is acceptable and must be
*deterministic*: the rounding is `(ticks * i + n/2) / n` integer arithmetic, so
the same pattern always produces the same ticks, which is what the hot-reload
and offline-export agreement in FINAL_PLAN §3.1 actually requires. Exactness was
never the requirement; reproducibility was.

`.adx` writes musical positions as `bars:beats:ticks` (e.g. `9:3:1920`) — human
readable, diff-legible, and exactly representable. The parser also accepts plain
decimal beats for hand-authored files and for v1 compatibility, converting with
the same deterministic rounding.

### 4.2 `TempoMap`

```cpp
struct TempoEvent { Ticks at; double bpm; bool ramp; };   // ramp = linear to next
struct MeterEvent { Ticks at; uint16_t numerator, denominator; };

class TempoMap {
public:
    Samples toSamples(Ticks, uint32_t sampleRate) const noexcept;
    Ticks   toTicks(Samples, uint32_t sampleRate) const noexcept;
    double  bpmAt(Ticks) const noexcept;
    BarBeatTick toBarBeat(Ticks) const noexcept;
    Ticks   fromBarBeat(BarBeatTick) const noexcept;
private:
    std::vector<TempoEvent> m_tempo;   // sorted, always has one at tick 0
    std::vector<MeterEvent> m_meter;   // sorted, always has one at tick 0
    std::vector<int64_t>    m_cumSamples;  // prefix sums, rebuilt on edit
};
```

Constant segments integrate as `samples = ticks × 60 × sr / (bpm × PPQ)`. Linear
ramps integrate in closed form over the segment rather than numerically —
a numeric integration makes `toSamples` and `toTicks` disagree, and that
disagreement is audible as drift on long projects.

`m_cumSamples` makes `toSamples` O(log n) via binary search plus one segment
evaluation. It is rebuilt on the main thread whenever tempo changes, never
during render.

**Invariant, tested:** `toTicks(toSamples(t)) == t` for all `t` on segment
boundaries and for 100k random `t` across a 20-tempo-change map.

### 4.3 The lexing discipline (ported verbatim from iteration one)

`_archive/src-cpp/src/AdxParser.cpp` got one thing exactly right and it is
non-obvious: **`#` starts a comment, but `F#5` is a note name.** The v1
`stripComment` handles this. Port that rule precisely, and extend it with the
cases v1 did not consider:

- `#` begins a comment **only** when preceded by whitespace, start-of-line, or
  a value separator — never when it immediately follows an alphanumeric
  character.
- `#` inside a double-quoted string is literal. (v2 adds quoted strings for
  paths with spaces, which v1 explicitly could not represent — see the `CLIP`
  parser at `AdxParser.cpp:427`, whose comment reads "FilePath must not contain
  spaces".)
- A `\` before `#` escapes it.

This rule gets its own test file section because getting it wrong silently
truncates user data.

### 4.4 The lossless `Document` — the central idea of this phase

FINAL_PLAN §6 requires two things that naively conflict:

> Rule 2 — Round-trip is lossless and stable... byte-identical.
> Rule 4 — Forward-tolerant. Unknown keys and sections are preserved verbatim.

A parser that builds only a `Project` cannot satisfy Rule 4, because the
`Project` has nowhere to put a key it does not understand. A parser that builds
only a syntax tree cannot satisfy the rest of the phase. So build both:

```cpp
namespace adx::format {

struct Span { uint32_t line, column, length, byteOffset; };

struct Line {
    enum class Kind : uint8_t { Blank, Comment, SectionHeader, KeyValue, Positional, Unknown };
    Kind kind;
    Span span;
    std::string raw;          // EXACT original bytes, always retained
    std::string key;          // parsed view, empty for Blank/Comment
    std::string value;
    bool claimed = false;     // set true when the Parser consumed it
};

struct Section {
    std::string type;         // "CHANNEL", "PATTERN", ...
    std::string name;         // header argument(s), verbatim
    Span span;
    std::vector<Line> lines;
    bool claimed = false;
};

class Document {
public:
    static std::expected<Document, DiagnosticList> parse(std::string_view text);
    std::string write() const;                 // reproduces input byte-for-byte
    std::span<const Section> sections() const;
    // Residue: everything the Parser did not claim. Re-emitted on save.
    DocumentResidue residue() const;
};

} // namespace adx::format
```

**The contract:**

1. `Document::parse(text).write() == text`, byte for byte, for *any* input. This
   is testable with a fuzzer and is the foundation everything else stands on.
2. `Parser` walks the `Document`, builds the `Project`, and marks each line and
   section it understood as `claimed`.
3. Unclaimed lines are **residue**. The `Project` holds the residue (attached to
   the nearest owning entity where possible, at the document root otherwise).
4. `Writer` emits canonical text for everything in the `Project` and re-emits
   residue at its recorded position.
5. Unclaimed lines produce `Severity::Warning` diagnostics with code `ADX1001
   unknown key` / `ADX1002 unknown section` — never dropped, never silent.

This is also what makes Phase 7's bidirectional sync tractable: the `Document`
is the pivot between text edits and model edits, and line spans give the editor
panel somewhere to anchor diagnostics.

### 4.5 Numbers and diagnostics — fixing §3.3.11

Iteration one called `std::stof` inside `try/catch` per token
(`AdxParser.cpp` throughout). That is slow, and `std::exception::what()` carries
no position, so every diagnostic degrades to "Malformed X on line N".

```cpp
template <class T>
std::expected<T, ParseError> parseNumber(std::string_view, Span) noexcept;
// std::from_chars; no exception; returns the exact column of the first bad char
```

```cpp
struct Diagnostic {
    enum class Severity : uint8_t { Error, Warning, Info };
    Severity severity;
    uint16_t code;          // ADX0001..; stable, documented in docs/adx-format-v2.md
    Span     span;          // line, column, LENGTH — enough to underline
    std::string message;
    std::string hint;       // "did you mean RESFILTER?" — optional
};
```

Codes are allocated in blocks: `0xxx` lexical, `1xxx` unknown/tolerated,
`2xxx` semantic, `3xxx` reference resolution, `4xxx` v1 migration.
Every code is documented. An undocumented code is a CI failure (a script
cross-checks emitted codes against the spec).

### 4.6 The project model

```cpp
namespace adx::project {

class Project {
public:
    ProjectMeta        meta;        // title, author, created, adx version
    core::TempoMap     tempo;
    std::vector<Channel>      channels;
    std::vector<Pattern>      patterns;
    Playlist                  playlist;
    Mixer                     mixer;
    Resources                 resources;
    std::vector<Marker>       markers;
    format::DocumentResidue   residue;

    ChannelId newChannelId() noexcept;   // monotonic; ids are never reused
    // ... one per entity type
    Channel* find(ChannelId) noexcept;   // nullptr on miss; never throws
};

struct Channel {
    ChannelId id; std::string name; Color color;
    InstrumentSpec instrument;       // type tag + parameter values + sample refs
    InsertId       output;           // which mixer insert this feeds
    uint16_t       maxPolyphony = 16;   // fixes §3.3.6
    VoiceStealMode stealMode = VoiceStealMode::OldestReleased;
    ArpSettings    arp;
    bool muted = false, soloed = false;
    float volume = 1.0f, pan = 0.0f, pitchOffsetCents = 0.0f;
};

struct Note {
    NoteId   id;
    core::Ticks start, length;
    uint8_t  pitch;            // MIDI
    uint8_t  velocity;         // 0..127
    int16_t  fineTuneCents;
    float    pan, cutoff, resonance;   // the per-note lanes in §5.1
    uint16_t releaseVelocity;
};

struct NoteClip {  ChannelId channel; std::vector<Note> notes; };

// ADDENDUM (post-Phase-2; scheduled in Phase 3 §4.5, Phase 4 §4.2/§4.13, Phase 5 §4.7).
// Appended to Note; all default to "absent", so existing files and golden hashes
// are unchanged. Serialized only when set.
//   std::optional<NoteSlide> slide;        // glide from this note's pitch to a target
//   std::vector<PitchPoint>  pitchCurve;   // freeform cents-vs-time, note-relative ticks
//   std::string              lyric;        // Voice instrument; empty = none
//   struct NoteSlide  { int16_t targetCents; core::Ticks start, length; CurveShape shape; };
//   struct PitchPoint { core::Ticks t; int16_t cents; CurveShape shape; };
// `slide` is the editor's one-gesture case (A -> C); `pitchCurve` is the general
// case. They compose: slide is applied first, the curve is added on top. A slide
// whose target is a following note's pitch is stored as cents, not as a link, so
// moving or deleting the next note never silently retargets it.

struct Pattern {
    PatternId id; std::string name; Color color;
    core::Ticks length;                       // pattern-local, may be any length
    std::vector<NoteClip>       noteClips;    // at most one per channel
    std::vector<AutomationClip> autoClips;
    std::optional<MiniNotationSource> mini;   // raw text + compile cache key
};

struct PlaylistItem {
    ItemId id;
    core::Ticks start, length;
    core::Ticks sourceOffset;                 // slip editing (Phase 8)
    std::variant<PatternRef, AudioClipRef, AutomationRef> content;
    bool muted = false;
};

struct Insert {
    InsertId id; std::string name; Color color;
    float gain = 1.0f, pan = 0.0f;
    bool  muted = false, soloed = false, polarityInvert = false;
    float stereoSeparation = 1.0f;
    std::vector<Slot> slots;                  // effect + wetDry + bypass
    std::vector<Send> sends;                  // {InsertId target, float level, bool preFader}
};

struct Route { InsertId from, to; };          // the DAG; master is an Insert like any other

} // namespace adx::project
```

**Why `std::vector<Channel>` and not `std::vector<std::unique_ptr<Channel>>`:**
values are cheaper, copyable (which the undo system uses), and contiguous, which
matters when Phase 3 flattens them. Ids, never pointers, are the stable handle;
`find()` is a linear scan over a vector that is realistically under 200 entries,
and the one place that would care (Phase 3) uses a prebuilt index instead.

**The five separations, and the defect each one fixes** (FINAL_PLAN §4 states
the rationale; this is the mapping to iteration one):

| Separation | Iteration-one defect it removes |
|---|---|
| Channel ≠ Pattern | `Track.patchName` bound one instrument to one note list — a riff could not be reused |
| Pattern ≠ Playlist item | `Track.notes` was the arrangement; placing a pattern twice meant copying notes |
| Channel ≠ Insert | `Track.effects` + `Track.volume` made the mixer strip inseparable from the instrument; no drum bus |
| `Route[]` DAG | `SEND=Delay\|Reverb` was two hardcoded global buses (`AdxParser.cpp:400-415`) |
| Per-channel `maxPolyphony` | one global 64-voice pool; a pad stole the kick |

### 4.7 `ParamRegistry` — fixing §3.2's string-keyed automation

Iteration one's automation lanes carried `trackTarget` and `paramTarget` as
strings (`AdxParser.cpp:178-190`) and compared them per block.

```cpp
struct ParamRef { uint32_t owner; uint16_t index; uint16_t kind; };  // 8 bytes, POD

struct ParamDescriptor {
    std::string_view name;        // "cutoff"
    float min, max, defaultValue;
    Unit unit;                    // Hz, dB, ms, ratio, semitones, normalized
    ScaleKind scale;              // linear | logarithmic | stepped
    uint16_t index;
};

class ParamRegistry {
public:
    // "channel.Lead.filter.cutoff" / "insert.3.slot.1.mix" / "master.gain"
    std::expected<ParamRef, Diagnostic> resolve(std::string_view path, const Project&) const;
    std::string pathOf(ParamRef, const Project&) const;   // for the Writer
    const ParamDescriptor& describe(ParamRef) const noexcept;
};
```

Resolution happens **once, at load**. Serialization goes back through `pathOf`,
so the text stays human-readable and stable while the runtime stays integer.
A path that does not resolve is `ADX3001` with the offending segment underlined
— not a silent no-op, which is what v1 did.

### 4.8 `Curve` — fixing §3.3.12

Iteration one's patch editor had Bézier envelope handles that `.adx` could not
represent, so save+reload silently flattened every curve.

```cpp
enum class CurveKind : uint8_t { Linear, Exponential, Logarithmic, Step, Smooth, Bezier, Hold };

struct Curve {
    CurveKind kind = CurveKind::Linear;
    float tension = 0.0f;          // -1..1 for Exponential/Logarithmic
    float c1x = 0.33f, c1y = 0.0f, c2x = 0.67f, c2y = 1.0f;   // Bezier only
    float evaluate(float t) const noexcept;   // t in [0,1] -> [0,1]
};
```

One evaluator, in `engine/core/Curve.h`, used by: automation lanes, ADSR stages,
the UI's curve drawing, and the Phase 3 renderer. FINAL_PLAN §3.2 calls this
"one evaluator shared by engine and UI so they cannot disagree" and promotes it
to a general principle — this is where that principle is instantiated.

Every ADSR stage carries a `Curve`. It serializes. That closes §3.3.12.

`evaluate()` is `noexcept`, allocation-free, and branch-light — Phase 3 calls it
per automation point per block.

### 4.9 Commands

```cpp
class Command {
public:
    virtual ~Command() = default;
    virtual void apply(Project&) = 0;
    virtual void revert(Project&) = 0;
    virtual std::string_view name() const noexcept = 0;   // for the history panel
    virtual CommandId kind() const noexcept = 0;
    // Return true if `next` was absorbed into this command.
    virtual bool coalesceWith(const Command& next) noexcept { return false; }
    virtual DirtyMask dirty() const noexcept = 0;         // what Phase 3 must re-snapshot
};
```

`DirtyMask` is a bitfield (`Tempo | Channels | Patterns | Playlist | Mixer |
Routing | Resources`). It exists so Phase 3 can rebuild only the affected part of
the render snapshot instead of the whole project on every note drag. Designing
it in now costs one enum; retrofitting it costs a snapshot rewrite.

**`CommandStack`:**

```cpp
class CommandStack {
public:
    void execute(std::unique_ptr<Command>);   // apply + push + clear redo
    bool undo(); bool redo();
    void beginGroup(std::string_view label);  // an explicit transaction
    void endGroup();
    std::span<const HistoryEntry> history() const;  // the Phase 6 panel
    uint64_t revision() const noexcept;       // bumped on every mutation
private:
    std::vector<std::unique_ptr<Command>> m_done, m_undone;
    std::chrono::steady_clock::time_point m_lastExecute;
};
```

**Coalescing rule, written down so it is not guessed at:** `execute()` offers the
new command to `m_done.back()->coalesceWith()` only when all of these hold —
same `CommandId`, same target ids, no `beginGroup` boundary between them, and
under 500 ms since the previous execute. A note drag becomes one history entry;
a drag, a pause, and another drag become two.

`revision()` is the change token the UI and Phase 3 both poll. It is cheaper and
more reliable than a signal graph.

**Non-negotiable:** the parser mutates the project through commands too. That is
what makes FINAL_PLAN §4's guarantee real — "the `.adx` writer, the GUI, and the
Python scripting API all mutate the project through the *same* command set" —
and it means loading a file is undoable, which falls out for free.

### 4.10 `.adx` v2 — structural shape

Full normative grammar with EBNF is `docs/adx-format-v2.md`, written as the first
deliverable of this phase. The shape:

```
[PROJECT]
TITLE="Suffocation"
ADX_VERSION=2
TUNING=440.0

[TEMPO]
0:0:0    140.0
32:0:0   150.0 ramp
[METER]
0:0:0    4/4

[CHANNEL Lead]
INSTRUMENT=additive
OUTPUT=insert.2
POLYPHONY=8
VOLUME=0.8
PAN=0.0
PARAM env.attack=0.01 curve=bezier(0.2,0.0,0.8,1.0)
PARAM filter.cutoff=1200
ARP mode=up rate=1/16 octaves=2 gate=0.8

[PATTERN Verse]
LENGTH=8:0:0
NOTES Lead
  F#5   0:0:0    0:1:0   0.80
  A5    0:1:0    0:1:0   0.72
MINI Drums
  bd*4, [~ sn]*2, hh(5,8)
AUTOMATION channel.Lead.filter.cutoff
  0:0:0    400   smooth
  4:0:0   3200   bezier(0.3,0.0,0.7,1.0)

[PLAYLIST]
TRACK 1 name="Drums"
  PATTERN Verse   0:0:0
  PATTERN Verse   8:0:0
TRACK 2 name="Vox"
  AUDIO "vocals take 3.wav" 16:0:0 offset=0:0:0 stretch=1.0 pitch=0 reverse=no

[MIXER]
INSERT 1 name="Master" GAIN=1.0
INSERT 2 name="Lead"   GAIN=0.9 PAN=-0.1
  SLOT 1 Reverb   mix=0.35 room=0.8 damp=0.5
  SLOT 2 EQ       low=0 mid=2.5 high=-1
  SEND insert.5 level=0.3 pre=no
ROUTE insert.2 -> insert.1

[MARKERS]
0:0:0    "Intro"
32:0:0   "Drop"
```

Decisions embedded above, each deliberate:

- **One musical event per line, positions as `bar:beat:tick`.** Adding a note
  changes exactly one line (FINAL_PLAN §6 rule 3).
- **`PARAM key=value` instead of v1's positional `FILTER=a,b,c`.** v1's
  positional tuples are unreadable in a diff (`RESFILTER=0,1200,0.7,0.5,0.3` —
  which field changed?) and are the reason the format could not grow without
  breaking. Named params are self-describing and order-independent.
- **`curve=` on any value.** Closes §3.3.12.
- **Quoted strings for anything that may contain spaces.** v1 could not express
  a sample path with a space in it.
- **`insert.N` / `channel.Name` reference syntax**, matching `ParamRegistry`
  paths exactly, so automation targets and routing use one addressing scheme.
- **`MINI` is a first-class pattern body**, not a `PATTERN=` key hidden inside a
  track (v1, `AdxParser.cpp:421`).

### 4.11 Writer canonicalization

`adx fmt` output is the canonical form, and `Writer` always emits it. Rules:

- Sections in fixed order: `PROJECT, TEMPO, METER, CHANNEL*, PATTERN*, PLAYLIST,
  MIXER, MARKERS`.
- Within a section, keys in declaration order (the order of `ParamDescriptor`),
  never alphabetical — alphabetical scatters related parameters.
- Entities in id order, which is creation order, which is stable across saves.
- Notes sorted by `(start, pitch)`.
- Floats written with `std::to_chars` shortest-round-trip representation, so
  `0.1` stays `0.1` and never becomes `0.10000000149011612`.
- Exactly one blank line between sections; none within.
- Residue re-emitted at its recorded position.

**Idempotence is a property test:** `fmt(fmt(x)) == fmt(x)` for the corpus and
for fuzzer output.

### 4.12 The v1 compatibility shim

FINAL_PLAN §6 rule 6 requires every v1 file to load, including
`docs/examples/suffocation.adx` (551 lines). Read from the archived parser, the
complete v1 surface and its v2 mapping:

| v1 construct (`_archive/src-cpp/src/AdxParser.cpp`) | v2 target |
|---|---|
| `[GLOBAL] BPM=` | `[TEMPO]` single event at `0:0:0` |
| `TUNING=`, `MASTER_VOL=` | `[PROJECT] TUNING=`, master insert `GAIN=` |
| `MASTER_DRIVE=`, `DELAY=t,fb,mix`, `REVERB=room,damp,mix` | slots on the master insert, in that order |
| `SIDECHAIN=on,amt,rel` | a `Ducker` slot on master + an explicit sidechain `Route` |
| `LOOP=start,end` | `[PROJECT] LOOP=` (its presence still means enabled — v1's rule, preserved) |
| `MARKER=beat,name` | `[MARKERS]` entry; name may contain commas (v1 split on the **first** comma only — preserve exactly) |
| `[PATCH <name>]` + `HARMONICS/ENVELOPE/DRIVE/FILTER/SUB/NOISE/RESFILTER/FILTERENV/FORMANT/VIBRATO/GLIDE/OSC` | one `InstrumentSpec` of type `additive` in the patch library; each key maps to named `PARAM`s (the positional→named table lives in `V1Shim.cpp` and is exhaustive) |
| `[TRACK <patchName>]` | **one Channel + one Pattern + one PlaylistTrack + one Insert**, all named after the patch. This 1→4 expansion *is* the fix for §3.3.7. |
| `Note Start Len Vel` lines | `NoteClip` in that Pattern; velocity `0..1` → `0..127` (v1 clamped `vel*127`) |
| `PATTERN=<mini>` | `Pattern.mini`; still compiled once at load, still after `LOOP=` is known |
| `CLIP path start [pitch stretch [R]]` | `PlaylistItem` with `AudioClipRef`; `R` → `reverse=yes` |
| `ARP mode rate oct gate` | `Channel.arp` |
| `EFFECT Reverb\|Distortion\|Bitcrush\|Chorus\|EQ ...` | `SLOT` entries on that track's Insert, positional args → named |
| `MIX=vol,pan` | Insert `GAIN=`/`PAN=` |
| `SEND=Delay,amt` / `SEND=Reverb,amt` | auto-create two aux inserts named `Delay Bus` / `Reverb Bus` on first use; add a `SEND` and a `ROUTE` to master |
| `[AUTOMATION <track> <param>]` + `beat value curve` | `AutomationClip` with the string target resolved through `ParamRegistry`; unresolvable → `ADX4001` warning, lane preserved as residue |

Shim rules:

- Migration is **in-memory only**. v1 files are never rewritten in place.
  `adx fmt --upgrade in.adx -o out.adx` is the explicit, opt-in path.
- Every migration decision that loses or invents information emits an `ADX4xxx`
  Info diagnostic. Loading `suffocation.adx` should print a readable migration
  report, not silence.
- Unknown v1 keys become residue, exactly as in v2.

### 4.13 CLI

`app/adx/cli.py`, over the bindings. No second binary.

```
adx validate FILE...         parse, report diagnostics, exit 1 if any Error
adx fmt [--check] [-i] FILE  canonical formatting; --check exits 1 if it would change
adx fmt --upgrade IN -o OUT  v1 -> v2 migration with a report
adx diff A B                 semantic diff (entities added/removed/changed), not textual
adx info FILE                counts, duration, tempo range, channel/pattern list
```

`adx diff` is semantic because a textual diff of a text format is already
`git diff`; the value adX adds is "you moved 3 notes and changed the reverb
mix", which requires the model.

`adx render` is Phase 8. The subcommand is declared here and exits with a clear
"not available until Phase 8" message rather than being absent.

### 4.14 Bindings

```python
p = adx_engine.Project.load("song.adx")        # -> (Project, list[Diagnostic])
p.save("song.adx")
p.channels                                     # O(channels) — legal under Rule 2
p.pattern(pid).note_count()                    # NOT a list of note objects
p.execute(adx_engine.commands.MoveNotes(pid, ids, delta_ticks))
p.undo(); p.redo(); p.revision
p.history()                                    # list[HistoryEntry] for the P6 panel
```

Notes are **never** exposed as a list of Python objects — that is an explicit
Rule 2 violation and would make a 10,000-note pattern cost 10,000 allocations
per redraw. Phase 5 reads them as a zero-copy numpy view. Phase 2 exposes only
counts and command submission, which is sufficient for the CLI and for tests,
and which prevents a convenience API from being built now and depended on later.

Every `load`, `save`, `execute`, `undo` releases the GIL (Rule 3).

---

## 5. Tests

| Test | Asserts |
|---|---|
| `time_tick_roundtrip` | `toTicks(toSamples(t)) == t` on segment boundaries and 100k random ticks across a 20-change map |
| `time_tuplet_determinism` | 5-, 7- and 11-tuplets produce identical ticks across 1000 recompiles |
| `tempomap_ramp_closed_form` | a linear ramp's integrated sample position matches an analytic value to < 1 sample over 10 minutes |
| `tempomap_barbeat` | `fromBarBeat(toBarBeat(t)) == t` across meter changes including 7/8 and 5/4 |
| `curve_endpoints` | every `CurveKind` satisfies `evaluate(0)==0`, `evaluate(1)==1` |
| `curve_bezier_monotone` | default handles produce a monotone curve; `evaluate` never returns NaN |
| `lexer_hash_comment_vs_sharp` | `F#5 0:0:0 0:1:0 0.8  # comment` keeps the note, drops the comment |
| `lexer_hash_in_quotes` | `AUDIO "track #3.wav" ...` keeps the `#` |
| `lexer_escaped_hash` | `\#` is literal |
| `document_byte_identical_roundtrip` | `parse(x).write() == x` for the corpus **and** for 10k fuzzer-generated inputs including malformed ones |
| `document_preserves_unknown` | a file with `FUTURE_KEY=1` and `[FUTURE_SECTION]` survives load+save unchanged and emits `ADX1001`/`ADX1002` |
| `writer_canonical_idempotent` | `fmt(fmt(x)) == fmt(x)` for the corpus and for fuzzer output |
| `writer_float_shortest` | `0.1` round-trips as `0.1` |
| `diagnostics_column_accuracy` | 20 hand-built malformed lines report the exact expected `(line, column, length)` |
| `diagnostics_all_codes_documented` | every code the code can emit appears in `docs/adx-format-v2.md` |
| `parser_no_exceptions_on_fuzz` | 100k mutated inputs produce diagnostics, never an uncaught throw, never a crash, never a hang |
| `v1_shim_loads_all_examples` | all four `docs/examples/*.adx` load with zero Errors |
| `v1_shim_suffocation_fidelity` | `suffocation.adx` → v2 model has the expected channel/pattern/note/effect/automation counts (hand-verified once, then frozen as a fixture) |
| `v1_shim_track_expands_to_four` | one `[TRACK X]` produces exactly one Channel, Pattern, PlaylistTrack and Insert, correctly cross-referenced |
| `v1_shim_marker_first_comma` | `MARKER=16,Drop, part 2` yields beat 16, name `Drop, part 2` |
| `v1_shim_send_creates_bus` | `SEND=Reverb,0.3` creates one aux insert, one send, one route — and a second track's send reuses the same bus |
| **`undo_to_empty_random`** | **the §9 gate.** Seeded RNG generates 10,000 valid random commands against a project built from `suffocation.adx`; apply all, undo all; the written output is byte-identical to the initial write. Repeated for 100 seeds. |
| `undo_redo_symmetry` | after N undos and N redos, output is byte-identical to pre-undo |
| `command_coalescing_window` | two drags 100 ms apart coalesce; 600 ms apart do not; a `beginGroup` boundary prevents coalescing |
| `command_dirty_mask` | each command reports exactly the subsystems it touched (table-driven) |
| `parser_uses_commands` | loading a file and then undoing every command yields an empty project |
| `validate_catches_invariants` | dangling `InsertId`, routing cycle, note outside pattern length, duplicate id — each detected with the right code |
| `route_cycle_detection` | a 3-insert cycle is rejected with the cycle path in the message |
| `python/test_cli.py` | `validate`, `fmt --check`, `diff`, `info` exit codes and output on the corpus |
| `python/test_examples.py` | round-trip every `docs/examples/*.adx` through the Python API |

**Fuzzing** runs in CI as a fixed 10k-case corpus with a fixed seed (fast,
deterministic) and nightly as a 10-minute libFuzzer-style random run.

---

## 6. Definition of done

- [x] `docs/adx-format-v2.md` exists, is normative, contains complete EBNF, and
      documents every diagnostic code. *Checked in both directions by
      `diagnostics_all_codes_documented` (every emitted code appears in the spec) and
      `every documented code exists in the table` (the spec names no code nothing can
      emit). Its worked example is `adx fmt` output, so it cannot drift from the
      writer.*
- [x] All four `docs/examples/*.adx` load through the v1 shim with zero Errors.
      *`v1_shim_loads_all_examples`, which also runs `validate()` on the result.
      `suffocation.adx` fidelity is frozen as a hand-derived fixture: 7 channels, 7
      patterns, 349 notes, 21 slots, 10 inserts, 6 markers, 6 lanes, 25 breakpoints.*
- [x] `document_byte_identical_roundtrip` passes on the corpus and 10k fuzz cases,
      including CRLF, BOM, NUL, lone CR, unterminated quotes and no final newline.
- [x] `writer_canonical_idempotent` passes, on the corpus and on generated input.
- [x] `undo_to_empty_random` passes for 100 seeds x 10,000 commands. *The 100-seed
      run is a `[.slow]` case, which ctest runs by default; two seeds run in the fast
      loop. It compares the written text as well as the model. It found two real
      bugs before it passed - see section 10.*
- [x] `parser_uses_commands` passes - proving the one-command-set guarantee.
- [x] `adx validate`, `adx fmt`, `adx diff`, `adx info` all work on the corpus.
      *`tests/python/test_cli.py`; `adx render` is declared and exits 2 naming
      Phase 8.*
- [x] Zero `std::stof`/`try`/`catch` in `engine/format/` (grep check in CI).
      *`python tools/lint.py format-safety`, CI step 11. Observed to fail: a
      `std::stof` appended to `NoteName.cpp` turned it red. The first version of the
      gate did not fire - its regex had been corrupted into literal backspace
      characters - which is exactly why a gate is not done until it has been seen to
      fail.*
- [x] `ParamRegistry` resolves every automatable parameter that exists so far;
      no string comparison survives past load. *All six automation lanes in
      `suffocation.adx` resolve, including the two v1 `MASTER` lanes, to 8-byte
      `ParamRef`s. The render-path grep is Phase 3's, as planned.*
- [x] FINAL_PLAN.md section 10 Phase 2 row updated.

Also observed: 112 ctest tests green on Debug, RelWithDebInfo and Release; 31 pytest
tests; clang-tidy clean on all 58 first-party files; ruff, mypy `--strict`,
clang-format and the header-length gate clean. Loading `suffocation.adx` takes
~0.6 ms and a 100k-note file ~150 ms against the 500 ms budget in section 9, guarded
by `test_load_budget`.

---

## 7. Out of scope (and who owns it)

| Thing | Owner |
|---|---|
| Anything that produces audio | Phase 3 |
| The render snapshot / flattened projection | Phase 3 |
| Mini-notation **compilation** (the model stores the source text; nothing compiles it yet) | Phase 4 stores the compiler; Phase 7 wires it live |
| `adx render` | Phase 8 |
| Any UI | Phase 5+ |
| Sample file loading (the model holds paths and validates existence only) | Phase 4 |

---

## 8. Handoff to Phase 3

Phase 3 inherits, and may rely on:

- `Project` as the single mutable main-thread truth, with `CommandStack::revision()`
  as the change token.
- `DirtyMask` on every command, so incremental snapshot rebuild is possible from
  day one.
- `TempoMap` with exact bidirectional tick↔sample conversion — the basis of
  sample-accurate scheduling.
- `ParamRef` as an 8-byte POD, already resolved: **no string comparison in the
  render path is possible**, because the render path never sees a string.
- `Curve::evaluate()` — `noexcept`, allocation-free, shared with the UI.
- Stable, never-reused entity ids — which is what makes `(ChannelId, NoteId)`
  voice identity (fixing §3.3.4) well-defined.
- `Validate.h` — Phase 3 asserts the project is valid before snapshotting, so
  the render path never has to defend against a malformed model.

**The constraint Phase 3 must respect:** the model above is main-thread-only and
uses `std::vector`/`std::string` freely. Phase 3 must not reach into it from the
audio thread. It builds a flattened POD snapshot and hands over a pointer.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| Byte-identical round-trip is harder than it looks (line endings, trailing whitespace, BOM) | `.gitattributes` normalizes to LF (Phase 0); `Document` retains `raw` bytes for every line; the fuzzer includes CRLF, BOM and trailing-whitespace cases |
| The v1 shim is a long tail of undocumented behavior | The mapping table in §4.12 was derived by reading the v1 parser, not the v1 docs — `docs/adxFormat.md` is known incomplete. `suffocation.adx` at 551 lines exercises nearly all of it and is the acceptance fixture |
| Command-per-mutation makes bulk operations (load a 100k-note file) slow | `beginGroup` + a bulk `AddNotes` command that carries a whole clip; measured, with a budget of < 500 ms to load `suffocation.adx` |
| `ParamRegistry` paths break when an entity is renamed | Paths resolve to ids at load and serialize from ids at save, so a rename rewrites the text automatically. Renaming is a command; the writer is the only thing that emits paths |
| The format grows keys faster than the spec is updated | `diagnostics_all_codes_documented` plus a CI check that every `ParamDescriptor` name appears in the spec |

---

## 10. Corrections made while executing this plan

**`std::expected` is C++23**, as Phase 1 already found. Nothing here returns one.
`Document::parse` always succeeds and takes a `DiagnosticList&`; the number and
position parsers return `bool` and record their own diagnostic with a column;
`ParamRegistry::resolve` returns a `ParamResolution` carrying a reason and the offset
and length of the offending path segment, so the caller — which knows the line and
column the path started at — builds the diagnostic that underlines exactly the bad
segment.

**`TempoMap` keeps cumulative seconds, not samples.** §4.2 named `m_cumSamples`. That
table would depend on the sample rate, so a 96 kHz export and a 48 kHz stream would
need two and could disagree about where a segment boundary lands. Seconds are
rate-independent; rounding to frames happens once, in `toSamples`. The round-trip
identity holds while a tick is at least one sample long — 750 bpm at 48 kHz — which
the spec now states.

**Velocity is an integer 0..127 in v2.** The §4.10 example wrote `0.80`. 127 values do
not map onto round decimals, so a float spelling is lossy and makes `fmt`
non-idempotent. Only the v1 shim reads 0..1, and it reports `ADX4005`.

**Inline keys are lowercase.** §4.10 wrote `INSERT 1 name="Master" GAIN=1.0`, mixing
the two conventions. Section-level keys are `UPPER_CASE`; keys inside a positional
line are `lowercase`, everywhere.

**Indentation is significant for block structure.** §4.10's example indents note
lines under `NOTES` but never said whether that mattered. It does: a line belongs to
the nearest preceding line with less indentation. Nothing else lets a `MINI` body, a
note list and a playlist automation lane end unambiguously. An opener with no
indented body is `ADX0010`.

**`SEND` carries an id, and slot and send ids are project-wide.** A parameter path
names a slot or send directly (`insert.2.slot.7.mix`) and an 8-byte `ParamRef` has
room for one id. Writing the id is what keeps a path pointing at the same send after
something else is deleted. The parser *adopts* the ids a file gives (inserts, slots,
sends, playlist tracks) rather than renumbering them — `Project::new*Id(wanted)`.

**`master` is an input alias only.** `master.gain` is gone from the path grammar; the
writer always emits `insert.N`, so a project has one spelling per reference.

**An automation path must name a parameter the file declares.** Auto-creating the
missing `PARAM` at its default would add a line the author never wrote, and Rule 2
says a load/save cycle changes nothing. `ADX3001` instead.

**`Pattern::mini` is a vector, one per channel**, not `std::optional`: §4.10's own
example has a pattern with both `NOTES Lead` and `MINI Drums`.

**The writer always writes `ADX_VERSION=2` (or higher).** A migrated project keeps
its source version for `adx info`, but writing `1` over v2 syntax sent the next load
through the v1 shim. `writer_canonical_idempotent` caught it.

**Residue position.** "Re-emitted at its recorded position" has no canonical meaning
once the known sections around it have moved. Unknown keys go at the end of their
section, unknown sections after all known ones, in original order — idempotent, and
byte-identical for any file this writer produced.

**Curves.** `Step` holds the start value and jumps at the end (v1's `step`, exactly);
`Hold` jumps immediately. Every kind satisfies f(0)=0, f(1)=1. v1's `exp` was defined
on the ratio of the two values and cannot be expressed as a normalised shape; it maps
to `exponential` and reports `ADX4003`.

**`SIDECHAIN=` becomes a `Ducker` slot, with no route.** §4.12 asked for "an explicit
sidechain `Route`", but every track insert already routes to the master, and `Route`
has no notion of a sidechain input — a second identical edge would mean nothing. The
key input belongs to the effect Phase 4 builds. Recorded as P2-3.

**Bindings.** Diagnostics cross as dicts and become dataclasses in
`app/adx/format.py`. Commands are built by *name* (`commands.MoveNotes("Verse",
"Lead", 120)` moves that clip's notes) because note ids are not exposed — exposing
them would mean a list of per-note objects, which Rule 2 forbids until Phase 5's
numpy view exists.

**`coalesceWith` is not `noexcept`.** Absorbing a rename or a breakpoint list copies
strings and vectors; a `noexcept` declaration would turn an allocation failure into
`std::terminate`. clang-tidy's `bugprone-exception-escape` found it.

**`parser_no_exceptions_on_fuzz` runs the whole load**, not just the document layer,
on mutated input of both grammars: 400 cases in the fast loop, 100k in the `[.slow]`
case on optimised builds and 10k in Debug, where a full load is ~25× slower and the
extra coverage is nil.

**Manifest additions**, each because a header needed an implementation or a concern
needed one home: `engine/project/Color.h`, `Channel.cpp`, `Automation.cpp`,
`Diff.h/.cpp` (the semantic diff), `engine/format/adx/Value.h/.cpp` (number, position,
curve and colour parsing and formatting, kept together so the two directions agree),
`bindings/ProjectHandle.h`, `app/adx/format.py`, `tests/cpp/Corpus.h`.

**Two bugs the undo gate found**, recorded because they are the kind that reappear:

- A create command whose `apply()` bailed out early (its parent was gone) still
  restored its saved `IdMarks` on revert — which were all zero — resetting every id
  counter in the project. Guarded on whether an id was actually allocated.
- Removals detached entities in descending index order (correct) and restored them in
  the same order (wrong): removing indices 1 and 2 of four and restoring highest-first
  swaps them. Restoration is now ascending, in five commands.

And one the CRLF test found: `adx fmt` to stdout on Windows wrote CRLF text through a
text-mode stream and doubled every line ending. It writes bytes now.
