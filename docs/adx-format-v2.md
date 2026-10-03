# The `.adx` format, version 2 — normative specification

This document defines `.adx` v2. It is **normative**: where an implementation and
this document disagree, this document is right and the implementation is a bug.

`docs/adxFormat.md` describes v1 and describes it incompletely. It is retained as
history only. v1 files still load — §12 defines exactly how — but v1 is not
specified here beyond that mapping.

**Status:** written as the first deliverable of Phase 2
([plans/phase_2.md](../plans/phase_2.md) §3), before the parser existed.

---

## 1. Design rules this format is accountable to

From [FINAL_PLAN.md](../FINAL_PLAN.md) §6, restated here because every rule below
is a consequence of one of them:

1. **Text is canonical.** The `.adx` file *is* the project.
2. **Round-trip is lossless and stable.** Parse → write → parse is byte-identical
   for a canonically formatted file.
3. **Diff-friendly.** One musical event per line. Adding a note changes one line.
4. **Forward-tolerant.** Unknown keys and sections survive a load/save cycle
   verbatim and are reported, never dropped.
5. **Diagnostics carry line and column.**
6. **v1 loads.**

Rules 2 and 4 together are why a reader builds two things from one file: a
`Document` (the lossless concrete syntax, §3) and a `Project` (the model, §7).
Anything the model does not understand stays in the document as *residue* and is
re-emitted where it was found.

---

## 2. Encoding, lines and whitespace

- A file is UTF-8. A leading BOM (`EF BB BF`) is permitted, preserved on write,
  and never emitted by the writer for a file that did not have one.
- Lines end with LF or CRLF. Both are preserved exactly as read. The writer emits
  LF for a file it is creating and otherwise preserves the file's dominant ending.
- A final line without a terminator is permitted and preserved.
- Horizontal whitespace is space (`U+0020`) and tab (`U+0009`). A tab counts as
  one column for diagnostics — column numbers are byte-derived character offsets,
  not display columns.
- Trailing whitespace is preserved by the document layer and removed by the
  canonical writer.

**Indentation is significant for block structure and for nothing else.** A line
indented more than the line above it belongs to that line's block (§4.3). The
canonical writer indents a child line by exactly two spaces per level.

---

## 3. Lexical structure

Every line is exactly one of six kinds.

```ebnf
line        = blank | comment | section | keyvalue | positional | unknown ;

blank       = { hspace } , newline ;
comment     = { hspace } , "#" , { any-but-newline } , newline ;
section     = { hspace } , "[" , ident , [ hspace , { any-but-"]" } ] , "]" ,
              { hspace } , [ comment-tail ] , newline ;
keyvalue    = { hspace } , ident , { hspace } , "=" , value , [ comment-tail ] , newline ;
positional  = { hspace } , word , { hspace , word } , [ comment-tail ] , newline ;
unknown     = { hspace } , { any-but-newline } , newline ;

ident       = ( letter | "_" ) , { letter | digit | "_" } ;
hspace      = " " | "\t" ;
```

`unknown` is the kind a line takes when it cannot be lexed as any of the others.
It is never an error by itself — it becomes residue and an `ADX1001`.

### 3.1 Comments, and why `F#5` is not one

```ebnf
comment-tail = hspace , "#" , { any-but-newline } ;
```

`#` begins a comment **only** when the character before it is whitespace or it is
the first character on the line. It is a literal `#` everywhere else. This rule is
ported verbatim from iteration one, which got it exactly right, and extended with
the two cases v1 did not consider:

- `F#5 0:0:0 0:1:0 96  # the comment` — the note name keeps its sharp, the comment
  is stripped. `#` here follows `F`, an alphanumeric.
- `AUDIO "track #3.wav" 16:0:0` — a `#` inside a quoted string is literal,
  regardless of what precedes it.
- `AUDIO take\#3.wav 16:0:0` — a `#` escaped by a backslash is literal, and the
  backslash is consumed.

Getting this wrong silently truncates user data, which is why it has its own test
section (`lexer_hash_comment_vs_sharp`, `lexer_hash_in_quotes`,
`lexer_escaped_hash`).

### 3.2 Words, quoting and escapes

```ebnf
word        = quoted | bare ;
quoted      = '"' , { qchar | escape } , '"' ;
bare        = bchar , { bchar } ;
bchar       = any character except hspace, '"', newline, and an unescaped "#" ;
qchar       = any character except '"', "\" and newline ;
escape      = "\" , ( '"' | "\" | "#" | "n" | "t" ) ;
```

Quoted strings exist because v1 could not represent a sample path containing a
space — the v1 `CLIP` parser's own comment says "FilePath must not contain
spaces". The canonical writer quotes a string when, and only when, its bare form
would not lex back to the same characters.

A word that *starts* with `"` is a quoted string and its quotes are removed. A
quoted run *inside* a bare word keeps its quotes and its spaces, so
`channel."Hardstyle Kick".drive` is one token whose quoted segment the parameter
path reader (§5.1) can still see.

An unterminated quoted string is `ADX0001`. An unrecognised escape is `ADX0002`
and the backslash is kept literally.

### 3.3 Numbers

Numbers are parsed with `std::from_chars`. There is no `std::stof`, no `try`, and
no `catch` anywhere in `engine/format/` — a CI grep enforces it. The reason is not
speed: `std::exception::what()` carries no position, so every v1 diagnostic
degraded to "Malformed X on line N" with no column to underline.

```ebnf
number      = [ "-" | "+" ] , ( digits , [ "." , digits ] | "." , digits ) ,
              [ ( "e" | "E" ) , [ "-" | "+" ] , digits ] ;
integer     = [ "-" | "+" ] , digits ;
```

A malformed number is `ADX0007`, and the diagnostic's span starts at the first
character `from_chars` could not consume.

### 3.4 Booleans

`yes` and `no`. Nothing else. `ADX0007` otherwise.

---

## 4. Document structure

### 4.1 Sections

```ebnf
document    = { line } ;
section-hdr = "[" , section-type , [ hspace , section-arg ] , "]" ;
section-type = ident ;
section-arg  = { any character except "]" and newline } ;
```

The section argument is taken **verbatim**, trimmed of surrounding whitespace. It
is the entity's name, so a name may contain spaces (`[CHANNEL Hardstyle Kick]`)
but never `]` or `.` — see §5.

A line before the first section header belongs to no section. Blank and comment
lines there are the file's preamble and are preserved. Anything else is `ADX0005`.

The section types v2 defines are, in canonical order:

| Section | Cardinality | Argument |
|---|---|---|
| `[PROJECT]` | 0 or 1 | — |
| `[TEMPO]` | 0 or 1 | — |
| `[METER]` | 0 or 1 | — |
| `[CHANNEL <name>]` | any | channel name |
| `[PATTERN <name>]` | any | pattern name |
| `[PLAYLIST]` | 0 or 1 | — |
| `[MIXER]` | 0 or 1 | — |
| `[MARKERS]` | 0 or 1 | — |

Any other section type is `ADX1002`: preserved verbatim as residue, reported as a
warning, never dropped.

### 4.2 Keys

A `keyvalue` line's key is an `ident`. Section-level keys are written in
`UPPER_SNAKE_CASE`; keys appearing inline inside a positional line are written in
`lowercase`. The parser matches keys case-sensitively in canonical position and
case-insensitively as a fallback, emitting nothing for a case difference — a
hand-authored `title=` is accepted and normalised on write.

A key the section does not define is `ADX1001`: residue, warning, preserved.

A key repeated within one section is `ADX1003`. The last occurrence wins; every
occurrence is preserved in the document, so the file still round-trips, but the
canonical writer emits the surviving one only.

### 4.3 Blocks

A line is a **child** of the nearest preceding line in the same section whose
indentation is strictly smaller. Blank and comment lines take part in no nesting
decision — they are attached to the block that is current where they appear and do
not open or close one.

A block-opening line whose block is empty is `ADX0010`, because a `NOTES Lead`
with nothing under it is always a mistake — usually an un-indented body.

---

## 5. References

One addressing scheme, shared by routing, automation targets and channel outputs,
so there is nothing to keep in sync.

```ebnf
reference   = "channel" , "." , name-seg
            | "insert" , "." , integer
            | "pattern" , "." , name-seg
            | "master" ;
name-seg    = quoted | bare-name ;
bare-name   = ( letter | digit | "_" | "-" ) , { letter | digit | "_" | "-" } ;
```

- **Inserts and playlist tracks are addressed by id**, not by position. Ids are
  monotonic and never reused, so `insert.5` means the same strip after any number
  of insertions and deletions.
- **Channels and patterns are addressed by name**, because a name is what a person
  reading a diff needs to see. Names are therefore unique within their kind
  (`ADX2003` when they are not) and may not contain `.`, `"`, `[`, `]` or a
  control character (`ADX2012`).
- A name containing anything outside `bare-name` is quoted:
  `channel."Hardstyle Kick"`.
- `master` is an **input alias** for the master insert, which is an ordinary insert
  in every other respect. It is accepted wherever a reference is, and the canonical
  writer always emits `insert.N` instead, so a project has exactly one spelling for
  every reference after one `adx fmt`.

A reference that does not resolve is an error, never a silent no-op — which is
what v1 did. See `ADX3002`, `ADX3003`, `ADX3004`.

### 5.1 Parameter paths

A parameter path is a reference followed by the parameter's own dotted name.

```ebnf
param-path  = "channel" , "." , name-seg , "." , channel-param
            | "insert" , "." , integer , "." , insert-param ;

channel-param = "volume" | "pan" | "pitch" | instrument-param ;
insert-param  = "gain" | "pan" | "width"
              | "slot" , "." , integer , "." , ident
              | "send" , "." , integer , "." , "level" ;
instrument-param = ident , { "." , ident } ;
```

`volume`, `pan` and `pitch` are reserved at channel level; any other dotted name
is a parameter of that channel's instrument (`channel.Lead.filter.cutoff`).
`slot.N` and `send.N` address the slot or send by **id**, consistent with §5.

A parameter path names a parameter that the file actually declares. Automating
`channel.Lead.filter.cutoff` requires the channel to carry a
`PARAM filter.cutoff=` line; automating a parameter that is not there is `ADX3001`,
not a silent no-op. The alternative — inventing the parameter at its default — would
add a line to the file that the author did not write, and Rule 2 says a load/save
cycle changes nothing.

Resolution happens **once, at load**, into an 8-byte POD `ParamRef`. Serialisation
goes back through the registry, so the text stays human-readable and stable while
the runtime stays integer, and renaming a channel rewrites every path that
referred to it automatically. A path that does not resolve is `ADX3001` with the
offending segment underlined.

---

## 6. Time

### 6.1 Positions

**Musical time is integer ticks at PPQ = 3840.** 3840 = 2⁸ × 3 × 5, which divides
exactly by every grid and tuplet a musician uses, including 128th-note
quintuplets. No PPQ divides by 7, so a 7-tuplet rounds; the rounding is
`(span × i + n/2) / n` in integer arithmetic, so it is *deterministic*, which is
what hot-reload and offline-export agreement actually require.

```ebnf
position    = bar-beat-tick | decimal-beats ;
bar-beat-tick = integer , ":" , integer , [ ":" , integer ] ;
decimal-beats = number ;
```

- `bar:beat:tick` is the canonical form and **all three fields are zero-based**.
  The first beat of the first bar is `0:0:0`. This makes a position and a duration
  the same arithmetic, which is why an eight-bar pattern is `LENGTH=8:0:0`.
- `bar:beat` is accepted and means `bar:beat:0`.
- A token **containing no colon** is decimal beats — quarter notes from `0`,
  converted as `round(beats × 3840)`. It exists for hand-authored files and for v1
  compatibility. `16` therefore means beat 16, not bar 16.
- `bar` may be negative (a count-in). `beat` and `tick` may not.

A malformed position is `ADX0008`.

### 6.2 Durations

A duration is written in the same notation and means **the tick distance from
`0:0:0` to that position**, measured through the project's meter map. In 4/4,
`0:1:0` is one beat and `8:0:0` is eight bars. A duration may not be negative
(`ADX2005`).

### 6.3 `[TEMPO]`

```ebnf
tempo-line  = position , hspace , number , [ hspace , "ramp" ] ;
```

`ramp` means the tempo travels linearly, in the tick domain, from this event to
the next one's value. A ramp with no following event is held constant. Ramps are
integrated in closed form, not numerically, because a numeric integration makes
tick→sample and sample→tick disagree and that disagreement is audible as drift on
a long project.

Tempo is clamped to [1, 999] bpm (`ADX2011`). Tick↔sample conversion is exactly
invertible while one tick is at least one sample long — 750 bpm at 48 kHz. Above
that the format is still legal and playback is still correct; only the exact
round-trip identity stops holding.

There is always a tempo event at `0:0:0`. A file that omits one gets 120 bpm.

### 6.4 `[METER]`

```ebnf
meter-line  = position , hspace , integer , "/" , integer ;
```

The denominator must be 1, 2, 4, 8, 16, 32 or 64 — the values that divide a whole
note exactly in ticks (`ADX2006`; the value is replaced with 4). There is always a
meter event at `0:0:0`, defaulting to 4/4.

A meter change that does not land on a bar line still ends the bar it interrupts,
so bar numbers stay strictly increasing and `fromBarBeat(toBarBeat(t)) == t` holds
across it.

---

## 7. Sections in detail

### 7.1 `[PROJECT]`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `ADX_VERSION` | integer | — | Format version. `2` for this document. Its presence is what identifies a v2 file (§12). |
| `TITLE` | string | `""` | |
| `AUTHOR` | string | `""` | |
| `CREATED` | string | `""` | ISO-8601 date, opaque to the engine. |
| `TUNING` | number | `440.0` | A4 in Hz. |
| `LOOP` | position `-` position | absent | `LOOP=8:0:0-16:0:0`. Presence means the loop is enabled — v1's rule, preserved deliberately. |

An `ADX_VERSION` greater than 2 is `ADX2009`: the file is still read with the v2
grammar, and every construct this version does not know becomes residue, which is
Rule 4 working as designed.

The writer emits v2 syntax, so it always writes `ADX_VERSION=2` or the higher
number the file arrived with — never 1. A project migrated from v1 remembers that
it was (`adx info` reports it), but the text it saves is v2 text and says so;
labelling it v1 would send the next load through the v1 shim.

### 7.2 `[CHANNEL <name>]`

| Key | Type | Default |
|---|---|---|
| `INSTRUMENT` | ident | `additive` |
| `OUTPUT` | reference | `master` |
| `POLYPHONY` | integer 1..256 | `16` |
| `STEAL` | `oldest-released` \| `oldest` \| `quietest` \| `none` | `oldest-released` |
| `VOLUME` | number 0..2 | `1.0` |
| `PAN` | number -1..1 | `0.0` |
| `PITCH` | number, cents | `0.0` |
| `MUTE` | boolean | `no` |
| `SOLO` | boolean | `no` |
| `COLOR` | `#rrggbb` | `#808080` |

Plus three positional lines:

```ebnf
param-line  = "PARAM" , hspace , param-name , "=" , number , [ hspace , "curve=" , curve ] ;
arp-line    = "ARP" , { hspace , arp-kv } ;
arp-kv      = "mode" , "=" , arp-mode
            | "rate" , "=" , fraction
            | "octaves" , "=" , integer
            | "gate" , "=" , number ;
arp-mode    = "off" | "up" | "down" | "updown" | "downup" | "random" | "order" ;
fraction    = integer , "/" , integer ;
zone-line   = "ZONE" , hspace , string , { hspace , zone-kv } ;
zone-kv     = "key" , "=" , key-range          (* default 0..127 *)
            | "root" , "=" , key               (* default 60; always written *)
            | "vel" , "=" , key-range          (* default 1..127 *)
            | "start" , "=" , integer          (* frames into the sample *)
            | "loop" , "=" , loop-mode         (* default off *)
            | "loopStart" , "=" , integer
            | "loopEnd" , "=" , integer        (* exclusive; 0 = end of sample *)
            | "xfade" , "=" , integer          (* loop crossfade, frames *)
            | "tune" , "=" , number            (* cents *)
            | "gain" , "=" , number            (* dB *)
            | "pan" , "=" , number
            | "rr" , "=" , integer , ":" , integer ;  (* round-robin group:index *)
key-range   = key , [ ".." , key ] ;
key         = integer | note-name ;
loop-mode   = "off" | "forward" | "pingpong" | "sustain" | "release" ;
```

`ZONE` maps a sample to a sampler channel's keys and velocities (phase_4.md §4.5),
one line per zone, in order. The path is resolved like an `AUDIO` item's - relative
to the project file - and the same file named by several zones, or by a zone and an
`AUDIO` item, is one entry in the sample pool. A field at its default is not
written, except `root`. Zones in a group `rr=g:i` with `g > 0` take turns: each note
that matches the group plays the zone whose index is next, from a counter that is
part of the render, so an offline render and a realtime one play the same zones.

`PARAM` replaces v1's positional tuples. `RESFILTER=0,1200,0.7,0.5,0.3` is
unreadable in a diff — which field changed? — and is the reason v1's format could
not grow without breaking. Named parameters are self-describing and
order-independent. Per-channel `POLYPHONY` replaces v1's single global 64-voice
pool, in which a pad could steal the kick.

Phase 2 stores instrument parameters as a named list and does not interpret them;
the instruments that give them meaning are Phase 4. An unknown parameter name is
`ADX1004` — a warning, and the value is kept.

### 7.3 `[PATTERN <name>]`

| Key | Type | Default |
|---|---|---|
| `LENGTH` | duration | `4:0:0` |
| `COLOR` | `#rrggbb` | `#808080` |

Three block openers:

```ebnf
notes-block = "NOTES" , hspace , name-seg , newline , { note-line } ;
note-line   = note-name , hspace , position , hspace , duration ,
              hspace , integer , { hspace , note-kv } ;
note-kv     = "pan" "=" number | "cutoff" "=" number | "res" "=" number
            | "fine" "=" integer | "rel" "=" integer
            | "slide" "=" slide | "bend" "=" bend | "lyric" "=" string ;
slide       = pitch-amount , "@" , duration , "+" , duration , [ "~" , curve ] ;
bend        = bend-point , { "|" , bend-point } ;
bend-point  = pitch-amount , "@" , duration , [ "~" , curve ] ;
pitch-amount = number , ( "st" | "c" ) ;   (* semitones or cents; stored as whole cents *)

mini-block  = "MINI" , hspace , name-seg , newline , { any-line } ;

auto-block  = "AUTOMATION" , hspace , param-path , newline , { breakpoint } ;
breakpoint  = position , hspace , number , [ hspace , curve ] ;
```

- `NOTES <channel>` names the channel the clip plays through — the Channel ≠
  Pattern split, which is what makes a riff reusable. At most one note clip per
  channel per pattern (`ADX2013`).
- Velocity is an **integer 0..127**, not a normalised float. 127 distinct values
  do not map onto round decimals, so a float spelling is lossy and makes `fmt`
  non-idempotent.
- A note name is `A`–`G`, then any number of `#` or `b`, then an octave, where
  `C4` is MIDI 60 (`ADX2008` if it is not a note name). A bare integer 0..127 is
  also accepted and is written back as a note name.
- A note starting outside `[0, LENGTH)` is `ADX2004`; it is kept, because a
  shortened pattern should not silently delete work.
- `slide`, `bend` and `lyric` are the note extensions (phase_4.md §4.0). Their
  times are **note-relative**. `slide=3st@0:1:0+0:0:960` glides 3 semitones up,
  starting a beat into the note and taking 960 ticks; the target is stored as an
  amount, not as a link to another note, so moving that note never retargets the
  slide. `bend=` is a freeform pitch curve: each point is the offset from the note's
  pitch at that time, and its `~curve` shapes the segment it starts. `lyric=` is the
  syllable a voice instrument sings. A pitch amount is written in semitones when it
  is a whole number of them (`-2st`), otherwise in cents (`-50c`). An absent key is
  not written, so a file without extensions round-trips byte for byte.
- `MINI` carries mini-notation source, one or more indented lines, each stored
  with its indentation and any trailing comment removed. Phase 2 stores the text;
  nothing compiles it (Phase 4 owns the compiler).
- A channel name that is not a `bare-name` is quoted in `NOTES` and `MINI`:
  `NOTES "Hardstyle Kick"`. Only a section header takes a name unquoted, because
  there the name runs to the closing `]` and cannot be ambiguous.

### 7.4 `[PLAYLIST]`

```ebnf
track-block = "TRACK" , hspace , integer , { hspace , track-kv } , newline ,
              { item-line } ;
track-kv    = "name" "=" string | "height" "=" integer | "mute" "=" boolean ;

item-line   = pattern-item | audio-item | auto-item ;
pattern-item = "PATTERN" , hspace , name-seg , hspace , position , { hspace , item-kv } ;
audio-item   = "AUDIO" , hspace , string , hspace , position , { hspace , item-kv } ;
auto-item    = "AUTOMATION" , hspace , param-path , hspace , position , { hspace , item-kv } ,
               newline , { breakpoint } ;

item-kv     = "length" "=" duration | "offset" "=" duration
            | "stretch" "=" number | "pitch" "=" number
            | "reverse" "=" boolean | "mute" "=" boolean ;

envelope-block = "ENVELOPE" , hspace , ( "gain" | "pan" | "pitch" | param-path ) ,
                 newline , { breakpoint } ;   (* indented under a PATTERN or AUDIO item *)
```

The integer after `TRACK` is the playlist track's id. `offset` is the source
offset used by slip editing (Phase 8 makes it do something; the model carries it
from here so that it does not have to be retrofitted). An omitted `length` means
the content's natural length.

An `ENVELOPE` block, indented under a `PATTERN` or `AUDIO` item, is automation that
belongs to that placement (phase_4.md §4.0): its breakpoint times are item-relative,
points past the item's length are not played, and a parameter it drives returns to
its own value when the item ends. `gain`, `pan` and `pitch` (cents) are properties of
the placement; anything else is a parameter path, resolved like an `AUTOMATION`
lane's (`ADX3001` if it names nothing). Gain, pan and pitch envelopes are stored and
round-trip, but are audible only on audio clips, which play from Phase 8.

A pattern name that is not a `bare-name` is quoted (`PATTERN "Verse 2" 0:0:0`). An
`AUTOMATION` item carries its breakpoints indented beneath it, in the same form as a
pattern's lanes: a playlist-level lane belongs to no pattern, so it has nowhere
else to live.

### 7.5 `[MIXER]`

```ebnf
insert-block = "INSERT" , hspace , integer , { hspace , insert-kv } , newline ,
               { slot-line | send-line } ;
insert-kv   = "name" "=" string | "gain" "=" number | "pan" "=" number
            | "mute" "=" boolean | "solo" "=" boolean
            | "invert" "=" boolean | "width" "=" number ;

slot-line   = "SLOT" , hspace , integer , hspace , ident , { hspace , slot-kv } ;
slot-kv     = "mix" "=" number | "bypass" "=" boolean | "sidechain" "=" reference
            | ident , "=" , number ;

send-line   = "SEND" , hspace , integer , hspace , reference , { hspace , send-kv } ;
send-kv     = "level" "=" number | "pre" "=" boolean ;

route-line  = "ROUTE" , hspace , reference , hspace , "->" , hspace , reference ;
```

`ROUTE` is a section-level line, not a child of an insert: it is a fact about the
graph, not about one strip. The routing graph is an arbitrary DAG — v1's
`SEND=Delay|Reverb` was two hardcoded global buses, which is a special case of
this and the reason real submixing was impossible. A cycle is `ADX3005`, reported
with the cycle's path in the message.

The integer after `SLOT` is the slot's id, and slots process in ascending id
order. The integer after `SEND` is the send's id. Both are project-wide rather than
per insert, because a parameter path names them directly
(`insert.2.slot.7.mix`, `insert.2.send.3.level`) and an 8-byte `ParamRef` has room
for one id, not two. They are written out so a path keeps meaning the same slot or
send after a deletion elsewhere. Effect type names are opaque to Phase 2; Phase 4
owns them. `mix=` is the slot's own wet/dry, never one of its named parameters.

`sidechain=insert.N` keys the slot from another insert's output - a ducker or
compressor listening to the kick (phase_4.md §4.9). It is an edge in the routing
graph like a `ROUTE`, so a key that would close a cycle is `ADX3005`. An effect that
takes no key input ignores it.

A file that declares no insert at all gets a master insert named `Master`, created
on load and written out on the next save: every channel has to feed something.

### 7.6 `[MARKERS]`

```ebnf
marker-line = position , hspace , string ;
```

---

## 8. Curves

One evaluator serves automation lanes, ADSR stages, the UI's curve drawing and the
renderer, so they cannot disagree. Every kind is a normalised shape on
[0,1] → [0,1]: it describes how a value travels between two breakpoints, not what
the breakpoints are. That a curve *serialises at all* is what closes the defect
where iteration one's Bézier envelope handles were lost on every save.

```ebnf
curve       = "linear"
            | "exponential" , [ "(" , number , ")" ]
            | "logarithmic" , [ "(" , number , ")" ]
            | "step" | "smooth" | "hold"
            | "bezier" , "(" , number , "," , number , "," , number , "," , number , ")" ;
```

| Kind | f(t) | Notes |
|---|---|---|
| `linear` | `t` | |
| `exponential(τ)` | `t^k` | Ease-in. τ ∈ [-1,1], `k = 2^(τ+1)`: -1 is linear, 0 is quadratic, +1 is a hard knee. τ defaults to 0. |
| `logarithmic(τ)` | `1 - (1-t)^k` | The mirror of the above. |
| `step` | `t < 1 ? 0 : 1` | Holds the *start* value and jumps at the end. This is v1's `step`, preserved exactly. |
| `smooth` | `t²(3 - 2t)` | Smoothstep. |
| `bezier(x₁,y₁,x₂,y₂)` | cubic Bézier through (0,0) and (1,1) | x₁ and x₂ are clamped to [0,1] so the curve stays a function of t. |
| `hold` | `t > 0 ? 1 : 0` | Jumps to the *end* value immediately. The sample-and-hold counterpart to `step`. |

Every kind satisfies `f(0) = 0` and `f(1) = 1`, and none of them ever returns NaN.
An unrecognised name is `ADX1005` and the segment falls back to `linear`.

---

## 9. Canonical form

`adx fmt` output is the canonical form, and the writer always emits it. A file
that is already canonical is byte-identical after a load/save cycle.

1. Sections in the order given in §4.1.
2. Within a section, keys in **declaration order** — the order of this document's
   tables — never alphabetical. Alphabetical order scatters related parameters.
3. Entities in id order, which is creation order, which is stable across saves.
4. Notes sorted by `(start, pitch)`. Breakpoints and markers by position.
5. Floats written with `std::to_chars` shortest round-trip representation, so
   `0.1` stays `0.1` and never becomes `0.10000000149011612`.
6. Exactly one blank line between sections, none within, and no trailing
   whitespace.
7. Child lines indented two spaces per level.
8. Residue re-emitted in a fixed place: an unknown key at the end of the section it
   was found in, an unknown section after all the known ones, in its original
   relative order. "Its original position" has no canonical meaning once the known
   sections around it have been reordered, and a rule that depended on one would not
   be idempotent. A file whose unknown content is already at those places — which is
   every file this writer produced — round-trips byte for byte.

`fmt(fmt(x)) == fmt(x)` is a property test, over the corpus and over fuzzer output.

---

## 10. Diagnostics

Every diagnostic carries a severity, a stable numeric code, and a span with line,
column **and length** — enough for an editor to underline the offending text. The
column is one-based and counts characters from the start of the line.

Codes are allocated in blocks: `0xxx` lexical, `1xxx` unknown-but-tolerated,
`2xxx` semantic, `3xxx` reference resolution, `4xxx` v1 migration. **Every code
this implementation can emit appears in the table below**; a code that does not is
a CI failure (`diagnostics_all_codes_documented`).

### 10.1 Lexical — `0xxx`

| Code | Severity | Meaning |
|---|---|---|
| `ADX0001` | Error | Unterminated quoted string. |
| `ADX0002` | Warning | Unrecognised escape sequence; the backslash is kept literally. |
| `ADX0003` | Error | Section header is missing its closing `]`. |
| `ADX0004` | Error | Section header has an empty type. |
| `ADX0005` | Warning | Content before the first section header. |
| `ADX0006` | Warning | Control character in text; preserved but not printable. |
| `ADX0007` | Error | Malformed number or boolean. |
| `ADX0008` | Error | Malformed position or duration. |
| `ADX0009` | Error | Malformed `key=value` pair. |
| `ADX0010` | Warning | Block opener with no indented body. |
| `ADX0011` | Error | Wrong number of fields on a positional line. |

### 10.2 Unknown but tolerated — `1xxx`

| Code | Severity | Meaning |
|---|---|---|
| `ADX1001` | Warning | Unknown key. Preserved verbatim as residue. |
| `ADX1002` | Warning | Unknown section. Preserved verbatim as residue. |
| `ADX1003` | Warning | Duplicate key in one section; the last occurrence wins. |
| `ADX1004` | Warning | Unknown parameter name; the value is kept. |
| `ADX1005` | Warning | Unknown curve name; `linear` is used. |
| `ADX1006` | Warning | Unknown inline key on a positional line; kept as residue. |

### 10.3 Semantic — `2xxx`

| Code | Severity | Meaning |
|---|---|---|
| `ADX2001` | Warning | Value out of range; clamped to the nearest legal value. |
| `ADX2002` | Error | A required key is missing. |
| `ADX2003` | Error | Duplicate entity name. |
| `ADX2004` | Warning | Note starts outside its pattern's length; kept. |
| `ADX2005` | Error | Negative duration. |
| `ADX2006` | Warning | Unsupported meter denominator; 4 is used. |
| `ADX2007` | Warning | Breakpoints were out of order; sorted on load. |
| `ADX2008` | Error | Not a note name. |
| `ADX2009` | Warning | `ADX_VERSION` is newer than this build understands. |
| `ADX2010` | Error | Duplicate entity id. |
| `ADX2011` | Warning | Tempo outside [1, 999]; clamped. |
| `ADX2012` | Error | Entity name contains a character names may not contain. |
| `ADX2013` | Error | More than one note clip for the same channel in one pattern. |
| `ADX2014` | Error | An entity's id is zero, which is the null id. |

### 10.4 Reference resolution — `3xxx`

| Code | Severity | Meaning |
|---|---|---|
| `ADX3001` | Error | Parameter path does not resolve. |
| `ADX3002` | Error | Reference to an unknown channel. |
| `ADX3003` | Error | Reference to an unknown insert. |
| `ADX3004` | Error | Reference to an unknown pattern. |
| `ADX3005` | Error | Routing cycle; the message names the cycle. |
| `ADX3006` | Warning | Referenced sample file does not exist on disk. |
| `ADX3007` | Error | Malformed reference. |

### 10.5 v1 migration — `4xxx`

Every migration decision that loses or invents information emits one of these, at
`Info`. Loading `suffocation.adx` should produce a readable migration report, not
silence.

| Code | Severity | Meaning |
|---|---|---|
| `ADX4001` | Warning | A v1 automation target did not resolve; the lane is preserved as residue. |
| `ADX4002` | Info | A v1 `[TRACK]` expanded into a channel, a pattern, a playlist track and an insert. |
| `ADX4003` | Info | A v1 `exp` curve was approximated: v1's exponential was defined on the ratio of the two values, v2's is a normalised shape, and the two are not the same function. |
| `ADX4004` | Info | A v1 `SEND=` created an aux insert, a send and a route. |
| `ADX4005` | Info | Velocities were rescaled from v1's 0..1 to 0..127. |
| `ADX4006` | Info | `SIDECHAIN=` became a `Ducker` slot on the master insert plus an explicit route. |
| `ADX4007` | Info | v1 master FX became slots on the master insert. |
| `ADX4008` | Info | A v1 positional tuple became named parameters. |
| `ADX4009` | Warning | A v1 entity name was not unique and has been suffixed. |
| `ADX4010` | Info | A v1 `CLIP` start time was in seconds and was converted through the tempo map. |
| `ADX4011` | Info | v1 `LOOP=` was present, so the loop is enabled. |
| `ADX4012` | Warning | A v1 line was not recognised by the v1 grammar either; kept as residue. |
| `ADX4013` | Info | A v1 `ENVELOPE=` or `FILTERENV=` with zero sustain. v1 ran a release from the sustain level, so that release was silent and the note was cut at note-off; the release became 0, which is what v1 played. |

---

## 11. Worked example

This is `adx fmt` output, so it is canonical by construction: every rule in §9 can be
read off it.

```
[PROJECT]
ADX_VERSION=2
TITLE="Suffocation"
TUNING=440

[TEMPO]
0:0:0 140
32:0:0 150 ramp

[METER]
0:0:0 4/4

[CHANNEL Lead]
INSTRUMENT=additive
OUTPUT=insert.2
POLYPHONY=8
VOLUME=0.8
PAN=0
PARAM env.attack=0.01 curve=bezier(0.2,0,0.8,1)
PARAM filter.cutoff=1200
ARP mode=up rate=1/16 octaves=2 gate=0.8

[CHANNEL Drums]
INSTRUMENT=sampler
OUTPUT=insert.1
POLYPHONY=16
VOLUME=1
PAN=0

[PATTERN Verse]
LENGTH=8:0:0
NOTES Lead
  F#5 0:0:0 0:1:0 102
  A5 0:1:0 0:1:0 92
MINI Drums
  bd*4, [~ sn]*2, hh(5,8)
AUTOMATION channel.Lead.filter.cutoff
  0:0:0 400 smooth
  4:0:0 3200 bezier(0.3,0,0.7,1)

[PLAYLIST]
TRACK 1 name="Drums"
  PATTERN Verse 0:0:0
  PATTERN Verse 8:0:0
TRACK 2 name="Vox"
  AUDIO "vocals take 3.wav" 16:0:0

[MIXER]
INSERT 1 name="Master"
INSERT 2 name="Lead" gain=0.9 pan=-0.1
  SLOT 1 Reverb mix=0.35 room=0.8 damp=0.5
  SLOT 2 EQ low=0 mid=2.5 high=-1
  SEND 1 insert.1 level=0.3
ROUTE insert.2 -> insert.1

[MARKERS]
0:0:0 "Intro"
32:0:0 "Drop"
```

---

## 12. v1 compatibility

A file is read as **v2** when it has a `[PROJECT]` section with
`ADX_VERSION` ≥ 2. Every other file is read through the v1 shim.

Migration is **in-memory only**. A v1 file is never rewritten in place;
`adx fmt --upgrade in.adx -o out.adx` is the explicit, opt-in path. Unknown v1
keys become residue exactly as in v2.

The mapping below was derived by reading `_archive/src-cpp/src/AdxParser.cpp`, not
by reading v1's documentation, which is known to be incomplete.

| v1 | v2 |
|---|---|
| `[GLOBAL] BPM=` | `[TEMPO]`, one event at `0:0:0` |
| `TUNING=` | `[PROJECT] TUNING=` |
| `MASTER_VOL=` | master insert `gain=` |
| `MASTER_DRIVE=x` | a `Distortion` slot on the master insert with `drive=1+x`: v1's master drive was `tanh(s * (1 + x))`, unlike a track's `DISTORTION`, which used the number as given |
| `DELAY=t,fb,mix` | a `Delay` slot on a **`Master FX`** insert that every track routes into, fully wet, with `dry=1` and `level=mix`: v1 added its echoes on top of the untouched mix |
| `REVERB=room,damp,mix` | a `Reverb` slot on the same `Master FX` insert, after the delay |
| `SIDECHAIN=on,amt,rel` | a `Ducker` slot on the master insert with `enabled`, `amount` and `releaseMs`, keyed (`sidechain=`) from the first track's insert, as v1 always keyed it |
| (implicit) | v1's fixed master compressor and output clamp become a `Compressor` (threshold −3 dB, 4:1, linear smoothing) and a `Limiter` at 0 dBFS, the last two slots on the master insert. The master's order is v1's: duck, drive, compressor, limiter (`ADX4007`) |
| `LOOP=start,end` | `[PROJECT] LOOP=`; its presence still means enabled |
| `MARKER=beat,name` | a `[MARKERS]` entry. v1 split on the **first** comma only, so a name may contain commas — preserved exactly |
| `[PATCH <name>]` and its keys | one `additive` `InstrumentSpec` in the patch library; each positional tuple becomes named `PARAM`s |
| `[TRACK <patch>]` | **one Channel + one Pattern + one PlaylistTrack + one Insert**, all named after the patch. This 1→4 expansion is the fix for the flat `Track` that blocked every DAW feature |
| `Note Start Len Vel` | a `NoteClip` in that Pattern; velocity `0..1` → `0..127` |

| `PATTERN=<mini>` | `Pattern.mini`, source text only |
| `CLIP path start [pitch stretch [R]]` | a `PlaylistItem` with an audio reference; `R` → `reverse=yes`; start seconds → ticks through the tempo map |
| `ARP mode rate oct gate` | the channel's `ARP` line; v1's integer mode maps 0→off, 1→up, 2→down, 3→updown, 4→random |
| `EFFECT <type> <args...>` | `SLOT` entries on that track's insert, positional args → named |
| `MIX=vol,pan` | insert `gain=` / `pan=` |
| `SEND=Delay,amt` / `SEND=Reverb,amt` | an aux insert named `Delay Bus` / `Reverb Bus`, created on first use and reused thereafter, plus a `SEND` and a `ROUTE` to master. The bus carries its effect fully wet at the master effect's level (insert `gain=`), because v1's sends fed only the effect's input, never the dry mix |
| `[AUTOMATION <track> <param>]` | an automation clip whose target is resolved through the parameter registry; unresolvable targets are `ADX4001` and the lane is kept as residue |

### 12.1 v1 parameter tuples

Each of these becomes named `PARAM` lines on the channel. The mapping is
exhaustive; anything not listed is residue.

| v1 key | v2 parameter names, in order |
|---|---|
| `ENVELOPE=a,d,s,r` (seconds) | `env.attack`, `env.decay`, `env.sustain`, `env.release` (seconds) |
| `HARMONICS=h1..h16` | `harmonic.1` … `harmonic.16` |
| `DRIVE=x` | `drive` |
| `FILTER=cut,lfoRate,lfoDepth` | `filter.cutoff`, `filter.lfoRate`, `filter.lfoDepth` |
| `SUB=level,wave,dropSemis,dropMs` | `sub.level`, `sub.wave`, `sub.dropSemitones`, `sub.dropMs` |
| `NOISE=level,type` | `noise.level`, `noise.type` |
| `RESFILTER=type,cut,res,envAmt,keyTrack` | `resfilter.type`, `resfilter.cutoff`, `resfilter.resonance`, `resfilter.envAmount`, `resfilter.keyTrack` |
| `FILTERENV=a,d,s,r` (seconds) | `filterEnv.attack`, `filterEnv.decay`, `filterEnv.sustain`, `filterEnv.release` |
| `FORMANT=vowelA,vowelB,morph,amount` | `formant.vowelA`, `formant.vowelB`, `formant.morph`, `formant.amount` |
| `VIBRATO=rate,depthCents,delayMs` | `vibrato.rate`, `vibrato.depthCents`, `vibrato.delayMs` |
| `GLIDE=ms` | `glide.ms` |
| `OSC=wave,unison,detune,pulseWidth` | `osc.wave`, `osc.unison`, `osc.detuneCents`, `osc.pulseWidth` |

### 12.2 v1 automation targets

| v1 target | v2 parameter path |
|---|---|
| `mix.volume` | `insert.<track's insert>.gain` - v1's lane replaced `MIX=` after the track's effects, and `MIX=` is the insert's gain |
| `mix.pan` | `insert.<track's insert>.pan` |
| `patch.<field>` | `channel.<track>.<mapped field>`, using §12.1's names |
| `effect.<Type>.<field>` | `insert.<id>.slot.<id>.<field>` on that track's insert |
| `master.sidechainAmount` | `insert.1.slot.<ducker>.amount` |
| `master.reverbMix` | `insert.<Master FX>.slot.<reverb>.mix` |
| `master.delayMix` | `insert.<Master FX>.slot.<delay>.level` |
| `master.masterDrive` | `insert.1.slot.<distortion>.drive` |
| anything else | unresolved: `ADX4001`, lane kept as residue |

---

## 13. Presets: `.adxpreset`

A preset is one instrument's or one effect's parameters, saved for reuse
(phase_4.md §4.12). It is read by the same lexer as a project, so it has the same
comments, quoting, numbers, curves and diagnostics.

```
# docs/C418.md 2.3: the "Aria Math" kalimba.
[PRESET]
NAME="Kalimba"
TYPE=additive
PACK=c418
TAGS=mallet, pluck, metallic

[PARAMS]
harmonic.1=1
env.decay=0.3 curve=exponential(0)
```

| `[PRESET]` key | Meaning |
|---|---|
| `NAME` | the preset's name, unique within its pack |
| `TYPE` | an instrument or effect type name; an unknown one is `ADX1001` |
| `PACK` | the pack it belongs to; a user preset keeps the pack it was saved from |
| `MIX` | effect presets only: the slot's wet/dry, 0..1 (`ADX2001` outside it) |
| `TAGS` | comma-separated, for browsing and search |

`[PARAMS]` holds `name=value [curve=...]` lines, one per parameter, checked against
the type's parameter table exactly as `PARAM` and `SLOT` parameters are: an unknown
name is `ADX1004` and kept, an out-of-range value is `ADX2001`. A parameter not
listed takes the type's default. `[ZONES]` holds a sampler preset's `ZONE` lines
(§7.2), with paths relative to the preset file. Any other section is `ADX1002`.

The shipped packs live in `engine/preset/packs/<pack>/`. User presets are saved to a
separate user directory, laid out the same way, and a user preset with the same pack
and name as a shipped one takes its place in the library.
