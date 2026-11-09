# Phase 12 — Sound-design foundation · XL

| | |
|---|---|
| **Status** | Not started |
| **Governs** | rack presets and macros, per-voice modulation, the `physical` instrument, new DrumSynth models, eleven new effects, sampler keyswitch/release zones, SFZ import, SF2/DLS import and the `gm` General MIDI player, the sample-library pipeline, pack tooling and audition |
| **FINAL_PLAN refs** | §4 (a new `[MACRO]` entity), §5.3, §5.4, §5.9 (preset packs), §5.12 (the sound library — added with this phase), §7 Phase 12, §9 |
| **Entry criteria** | [phase_11.md](phase_11.md) §6 complete. Every golden hash recorded at the end of Phase 11 is the baseline this phase must not move. |
| **Next** | [phase_13.md](phase_13.md) |
| **§5 coverage owned** | §5.12 infrastructure (the packs themselves are Phases 13–19) |

---

## 1. Objective

Make adX able to *be* the instruments the packs in Phases 13–19 call for, then
make building those packs a repeatable, tested process instead of hand-editing
`.adxpreset` files.

When this plan was written (2026-10-25) the research for the seven packs
was checked against the post-Phase-11 engine, parameter by parameter
(`adx_engine.type_catalog()`). The packs could not be built faithfully, for
seven reasons. An eighth (G8) was added on 2026-11-02 at the user's request.
This phase exists to remove each one:

| # | Gap | Which packs it blocks | Fixed by |
|---|---|---|---|
| G1 | A preset is one instrument **or** one effect. Effects only live on mixer inserts. A hardstyle kick (3 layers + an 8-slot chain), a supersaw lead (3 layers + sends) or a vocal chain cannot be one preset. | all | §4.1 racks |
| G2 | No macros: a preset cannot expose "Tail Length" or "Openness" as one knob over several parameters. | all | §4.2 |
| G3 | No per-voice modulation beyond VA's 2 LFOs (Hz only, 5 targets) and Wavetable's position LFO. No pitch envelope on VA, Wavetable or FM. No velocity→brightness. No mono/legato. | hoover, lasers, plucks, 808 glides, FM brass (Risset), screeches | §4.3 |
| G4 | No physical modelling: no plucked/bowed string, no modal resonator, no blown tube. Guitar, bass guitar, harp, solo bowed strings, steel pan, celesta, Rhodes/Wurli tines, bowed glass are all approximations without it. | hyperpop (glaive/underscores guitars), classical, ambient (steel pan, Wurlitzer), band | §4.4 |
| G5 | Effects missing for these genres: a real clipper/waveshaper, a standalone resonant filter, comb/resonator, OTT-style upward compression, ensemble chorus, tape, long FDN reverb with shimmer, rotary, hard-tune pitch correction, tempo-synced volume shaper. | hardcore (clipper, OTT), hyperpop (hard-tune, OTT), ambient (tape, shimmer), classical/band (rotary) | §4.6 |
| G6 | DrumSynth has kick/snare/hat/clap/tom. The 808's cowbell, cymbal, rim and clave and a shaker are missing, though the six-oscillator metal bank (`kMetal`, 205.3–800 Hz) already exists in `DrumSynth.cpp`. | core drums, phonk (the cowbell *is* drift phonk) | §4.5 |
| G7 | No way to use real sample libraries at scale: the sampler has round-robin but no keyswitches or release-trigger zones, nothing imports SFZ, and nothing fetches, pins or licence-checks a multi-GB library. | classical, ambient, band, breakcore | §4.7, §4.8 |
| G8 | No SoundFont (SF2) or DLS banks, and no General MIDI player (FL's Fruity LSD). A `.mid` file can't be auditioned on a GM bank, the Windows GS Wavetable bank (`gm.dls`) can't be used, and the thousands of free SF2 banks out there have to be converted by hand first, which loses their modulators and articulation. The sampler's envelope is per instrument and it has no filter, but SF2 and DLS give every zone its own envelope, filter and LFOs. | hyperpop (the GM set, §4.5 of phase 14), chiptune/lo-fi sketches, MIDI import auditioning | §4.7.4, §4.7.5 |

Everything here is additive. A parameter added to an existing type defaults to
inert, so **every pre-existing golden hash must be unchanged** (the Phase 4
discipline: "Golden hashes identical in all three configurations").

The phase runs in three tranches, as Phase 4 did:

- **A — model and format:** racks, macros, the modulation layer, mono/legato,
  sampler zone extensions, SFZ import, SF2/DLS import and the `gm` player,
  `lib://` paths. (§4.1–4.3, §4.7)
- **B — DSP:** `physical`, the DrumSynth models, the effects. (§4.4–4.6)
- **C — tooling:** library fetch, multisample rendering, wavetable and IR
  generators, Kick Lab, audition, the pack manifest, browser upgrades. (§4.8–4.11)

A may start immediately. B depends on A's modulation layer (every new instrument
uses it). C can start in parallel with B and must finish before Phase 13.

---

## 2. Deliverables — exact file manifest

```
engine/project/
  Rack.h/.cpp               the expanded rack: layers -> channels, chains -> inserts
  Macro.h/.cpp              Macro { name, value, targets[] {path, min, max, curve} }
  MacroCompile.h/.cpp       macros compile into per-block automation like controllers
  commands/RackCommands.*   InsertRack (one undo step), SaveRack, MacroSet/AddTarget

engine/preset/
  Rack.h/.cpp               .adxrack parse/write (same lexer, same diagnostics)
  PackManifest.h/.cpp       pack.adxpack: id, version, requires, libraries, licences
  LibraryPaths.h/.cpp       lib://<library>/<path> resolution + missing-library fallback

engine/dsp/
  ModMatrix.h/.cpp          8 slots: source x destination x amount, per voice
  ModEnvelope.h             two extra per-voice ADSRs with Curve stages
  PitchEnvelope.h           dedicated exponential pitch sweep (amount, time, curve)
  TempoLfo.h                Lfo + tempo sync + delay + fade-in (wraps dsp::Lfo)
  Waveguide.h/.cpp          fractional delay line, loss filter, dispersion allpass
  ModalBank.h/.cpp          up to 32 decaying resonators, SIMD-friendly layout
  Exciters.h/.cpp           pick, hammer, mallet, bow (friction table), breath/jet, reed
  Fdn.h/.cpp                8-line feedback delay network for Hall

engine/instruments/physical/
  PhysicalParams.h          descriptor table (§4.4)
  PhysicalInstrument.h/.cpp models: pluck, bow, modal, tube
  ModeTables.h              shipped mode-ratio tables (§4.4.3), each with its source

engine/instruments/drumsynth/
  DrumSynthParams.h         (extended) models Cowbell, Cymbal, Rim, Clave, Shaker, Conga
  MetalBank.h/.cpp          the six-oscillator bank shared by hat/cymbal/cowbell

engine/instruments/{va,wavetable,fm,additive}/
  *Params.h                 (extended) mod.*, modEnv*.*, pitchEnv.*, lfo sync, voice.mode
engine/instruments/wavetable/
  WavetableParams.h         (extended) osc2, sub, noise, warp, filter type/env

engine/instruments/sampler/
  ZoneSet.h                 (extended) keyswitch groups, release-trigger zones, xfade layers
  ZoneArticulation.h        per-zone overrides: DAHDSR amp + mod envelopes, filter, 2 LFOs (§4.7.4)

engine/instruments/gm/
  GmParams.h                descriptor table: bank, program, bankMsb/Lsb, part mode, drums (§4.7.5)
  GmInstrument.h/.cpp       16-part General MIDI player over sampler zones
  GmBankCache.h/.cpp        one parsed bank shared by every gm channel that names it

engine/effects/
  Clipper.h/.cpp            soft/hard/sine-fold/foldback/asym, 1-16x oversampled
  Shaper.h/.cpp             transfer curve from core::Curve knots, oversampled
  Filter.h/.cpp             ladder/SVF/comb/formant, LFO (synced), envelope follower
  Resonator.h/.cpp          tuned comb bank (1-4 voices), damping, feedback
  Ensemble.h/.cpp           2-8 voice BBD-style chorus, Juno I/II/I+II modes
  Tape.h/.cpp               wow, flutter, saturation, hiss, HF age, speed
  Hall.h/.cpp               FDN reverb, 0.1-60 s, predelay, mod, shimmer, freeze
  Rotary.h/.cpp             Leslie horn + drum, slow/fast with ramp, drive
  PitchCorrect.h/.cpp       YIN detect + PSOLA, key/scale, retune speed, formant keep
  VolumeShaper.h/.cpp       tempo-synced gain curve (LFOTool/Kickstart), 16-step gate
  Multiband.h/.cpp          (extended) per-band upward compression -> OTT

engine/format/sfz/
  SfzParser.h/.cpp          the opcode subset in §4.7.2 -> SampleZone[]

engine/format/soundfont/
  Riff.h/.cpp               bounds-checked RIFF chunk walker shared by both formats
  Sf2Parser.h/.cpp          sfbk: phdr/pbag/pgen/pmod, inst/ibag/igen/imod, shdr, smpl+sm24
  DlsParser.h/.cpp          DLS level 1 and 2: lins/ins/lrgn/rgn/wsmp/wlnk/art1/art2, wvpl
  BankModel.h               Bank { presets[] { bank, program, name, zones[] } } both parsers emit
  Generators.h/.cpp         timecents/centibels/absolute cents -> seconds/dB/Hz; default modulators

app/adx/panels/
  browser/                  (extended) packs, categories, audition-on-click, favourites
  rack/panel.py             rack view: layers, chain, 8 macro knobs
  library/dialog.py         "Get sound libraries": size, licence, progress, verify

tools/packs/
  fetch_libraries.py        pinned download + SHA-256 verify into the library store
  import_sfz.py             SFZ -> .adxpreset/.adxrack with lib:// paths
  import_soundfont.py       one SF2/DLS preset -> .adxpreset (for editing it as a plain sampler)
  make_bank_fixtures.py     writes the tiny deterministic SF2 and DLS test banks (§5)
  render_multisample.py     an adX preset -> multisample WAVs + a sampler preset
  kick_lab.py               resample loop: render a kick rack per note -> sampler preset
  gen_wavetables.py         deterministic numpy wavetables (§4.10)
  gen_irs.py                deterministic impulse responses (§4.10)
  audition.py               standard phrase per category -> WAV, LUFS, spectrum PNG
  licence_report.py         every asset's licence -> installer/THIRD-PARTY.md section

docs/
  adx-format-v2.md          (extended) [MACRO], .adxrack, pack.adxpack, lib://
  sound-design.md           how to build a pack: the recipes' vocabulary and workflow

tests/cpp/rack/ tests/cpp/modmatrix/ tests/cpp/physical/ tests/cpp/effects/
tests/cpp/sfz/ tests/cpp/soundfont/ tests/cpp/gm/
tests/python/test_pack_tools.py tests/python/test_rack_panel.py
tests/data/banks/           tiny_gm.sf2, tiny_gm.dls (generated, committed, < 200 KB each)
```

---

## 3. Order of work

1. **Baseline.** Record the post-Phase-11 golden set. Every later step re-runs it.
2. **Tranche A.**
   1. `[MACRO]` in the model and format, MacroCompile, commands; a macro is
      automatable like any parameter (`macro.<name>`).
   2. `.adxrack` read/write and `InsertRack`, which expands a rack into channels
      and inserts in **one** undo step.
   3. The modulation layer (§4.3) on VA first, then Wavetable, FM, Additive.
   4. `voice.mode` mono/legato in the voice pool.
   5. Sampler zone extensions, then the SFZ parser, then `lib://` resolution.
   6. Per-zone articulation, then the RIFF walker and SF2 parser, then DLS, then
      the `gm` instrument on top (§4.7.4–4.7.5). Articulation is built first
      because SF2 import is wrong without it, and it rides on the §4.3 envelopes.
3. **Tranche B.** `physical` (pluck → modal → bow → tube, in that order of payoff),
   the DrumSynth models, then the effects in the order hardcore needs them:
   Clipper, Filter, OTT, VolumeShaper, Ensemble, Shaper, Resonator, Tape, Hall,
   PitchCorrect, Rotary.
4. **Tranche C.** The tools in §4.8–4.11, the browser and rack panel, and the library dialog.
5. **The demonstration rack set** (§6): one rack per gap, proving each closed.

---

## 4. Design

### 4.1 Racks (`.adxrack`) — closes G1

A rack is a small project fragment. It holds layers (channels), a chain (insert
slots), optional sends, controllers and macros. On insertion it expands into
ordinary model entities, so the rest of adX needs no new concept to play it.

```
# Every rack starts with what it is and what it was built from.
[RACK]
NAME="Euphoric Kick"
PACK=hardcore
CATEGORY=Drums/Kick
TAGS=kick, hardstyle, euphoric
TEMPO_HINT=150
ROOT_HINT=F1
VERSION=1

[LAYER punch]
TYPE=drumsynth
KEYS=0..127  VELOCITY=0..127  GAIN=-4  PAN=0
[PARAMS]
model=0
pitchDrop=30
pitchTime=0.018
decay=0.12
[CHAIN]
SLOT ParametricEq mix=1 band1.type=4 band1.freq=60

[LAYER tail]
PRESET=hardcore/Euphoric Tail         # a layer may reference a preset by pack/name
GAIN=0

[CHAIN]                               # the rack's own insert, after the layers sum
SLOT ParametricEq band1.type=4 band1.freq=30 band2.type=1 band2.freq=700 band2.gain=9
SLOT Clipper mode=1 drive=12 oversample=3
SLOT ParametricEq band1.type=6 band1.freq=2400 band1.q=6
SLOT Overdrive drive=14 tone=6000 mode=0
SLOT Multiband preset=OTT depth=0.35
SLOT Limiter ceiling=-0.3

[SEND reverb]                         # optional: a send to a return the rack also creates
LEVEL=-18
SLOT Hall size=0.6 decay=1.8 mix=1
SLOT Ducker amount=0.8 releaseMs=120 key=rack

[MACROS]
MACRO 1 "Tail Length" value=0.5
  TARGET layer.tail.env.decay        range=0.15..0.60 curve=exponential(0.3)
  TARGET layer.tail.pitchEnv.time    range=0.03..0.09
MACRO 2 "Crunch" value=0.4
  TARGET chain.2.drive               range=6..24
  TARGET chain.4.drive               range=8..22
```

**Expansion** (`InsertRack`, one command, one undo entry):

- each `[LAYER]` → a `Channel` named `<rack>.<layer>` (unique-suffixed), routed to
  the layer's own insert when it has a `[CHAIN]`, else straight to the rack insert.
  `KEYS=`/`VELOCITY=` become the channel's note filter (`noteFilter.keyLo/keyHi/velLo/velHi`,
  default full range). It is the one field this phase adds to `Channel`, and it
  is what lets a kit be one rack with a layer per key, and lets a split or a
  velocity layer exist at all. A pattern note outside the filter is skipped by
  the scheduler at compile time, never per block;
- the rack `[CHAIN]` → a new `Insert` named `<rack>` with those slots, routed to master;
- each `[SEND]` → a return insert plus a send from the rack insert. `key=rack` keys
  a ducker from the rack insert, the hardstyle "sidechained returns" pattern;
- `[MACROS]` → project `[MACRO]` entries with targets rewritten to resolved paths
  (`channel.Euphoric_Kick.tail.env.decay`, `insert.Euphoric_Kick.slot.2.drive`).

Rack-relative paths (`layer.*`, `chain.N.*`, `send.*`) exist only inside the
`.adxrack` file. After expansion everything is an ordinary path, so the `.adx`
text stays readable and diffable with no rack indirection (§6 design rule 3).

**SaveRack** does the inverse from a selection of channels and inserts, so a
pack author can build a sound in the GUI and save it as a rack.

A `.adxpreset` remains valid and is still the right file for a single instrument
or effect. Racks may reference presets (`PRESET=`) so a sound is defined once and
reused by many racks.

### 4.2 Macros — closes G2

```cpp
struct MacroTarget { std::string path; ParamRef target; float min, max; core::Curve curve; };
struct Macro { core::MacroId id; std::string name; float value = 0.5F;
               std::vector<MacroTarget> targets; };
```

A macro is a parameter (`macro.<name>`, 0..1), so it can be automated, linked to
a controller, MIDI-learned (Phase 9) and pad-mapped (Phase 11). `MacroCompile`
runs at snapshot build, like `ControllerCompile`. It folds each macro into its
targets' automation lanes as `min + (max-min) * curve(value)`. When a target is
also automated, the macro offsets it in the normalised domain. It does not
replace the automation, so drawn curves stay audible.

Every rack in Phases 13–19 exposes **at most 8 macros**. The names are fixed by
category (§4.11), so "Macro 1" means the same kind of thing across all leads.

### 4.3 Per-voice modulation — closes G3

Added to `va`, `wavetable`, `fm`, `additive` and the new `physical`. Every
default is inert.

| Parameter | Range | Notes |
|---|---|---|
| `pitchEnv.amount` | −48..48 st | start offset; 0 = off |
| `pitchEnv.time` | 0.001..2 s | to within 1 % of the target, with `curve=` honoured |
| `modEnv1.attack/decay/sustain/release`, `modEnv2.*` | as `env.*` | curves first-class (§3.3.12) |
| `lfo1.sync`, `lfo2.sync` | 0..12 | 0 = Hz; 1..12 = 1/64 … 4 bars, incl. dotted/triplet |
| `lfo1.delay`, `lfo1.fade` | 0..5 s | vibrato that arrives late |
| `mod.1..8.source` | enum | none, modEnv1, modEnv2, lfo1, lfo2, velocity, key, modwheel, aftertouch, random-per-note, pitchEnv |
| `mod.1..8.dest` | enum per type | pitch, osc1/osc2 pitch, pulseWidth, cutoff, resonance, level, pan, drive, unison.detune, wt.position, warp.amount, fm op1..6 level, fm feedback, exciter.*, modal.decay |
| `mod.1..8.amount` | −1..1 | bipolar; scaled to the destination's range |
| `voice.mode` | 0..2 | poly, mono (retrigger), legato (no retrigger, glide only on overlap) |
| `glide.mode` | 0..1 | always / legato only |

VA's existing `lfoN.target`/`lfoN.depth` stay and keep working. They are sugar
for one matrix slot, and both paths are tested to produce identical output.

Wavetable is extended to the modern shape the hyperpop and hardcore packs need:
`osc2.table/position/semi/fine/level`, `sub.level`, `noise.level`,
`filter.type` (VA's five), `filterEnv.*`, `filter.envAmount`, and
`warp.mode/amount` (none, hard sync, bend+, bend−, PWM, asym, FM-from-osc2,
mirror, quantise).

*Why a matrix and not more fixed knobs:* the research turned up one recurring
pattern. Brightness follows loudness (Risset's brass, Chowning's FM index), pitch
dives at note start (hoover, laser, kick tails), and vibrato arrives late
(strings, screeches). Each is "source X drives destination Y", and a fixed knob
for every pairing is how iteration one grew 30 one-off fields.

### 4.4 The `physical` instrument — closes G4

One instrument, four models, one exciter-resonator-body pipeline.

```
exciter (pick | hammer | mallet | bow | breath | reed)
   -> resonator (waveguide string | modal bank | waveguide tube)
   -> body (none | guitar | violin | cello | pan skirt | user IR zone)
   -> env, mod matrix, level
```

#### 4.4.1 `model=0` pluck — extended Karplus–Strong / commuted waveguide

`string.decay` (s to −60 dB at C4, key-scaled), `string.damping` (loop low-pass),
`string.brightness` (excitation low-pass), `string.dispersion` (allpass stiffness:
piano-ish inharmonicity), `exciter.position` (pick position comb, 0..0.5),
`exciter.hardness`, `mute` (palm-mute: damping up, decay down, a velocity-
scaled thunk), `body.type`, `body.mix`. Commuted synthesis is used because its body
IR replaces a resonator model at no per-voice cost (Smith; the Sondius guitar ran
under 2 % CPU per voice on a 120 MHz Pentium). An electric guitar is this
model into a Clipper/Overdrive and a cabinet IR (`gen_irs.py`, §4.10).

#### 4.4.2 `model=1` bow — waveguide string with a friction table

`bow.pressure`, `bow.speed`, `bow.position`, plus the string parameters. Used for
expressive *solo* lines and for gap-filling where the libraries have no
articulation. Sections stay sample-based (Phase 16). Commuted bowing is
acceptable as a fallback if the friction model is too expensive at 16 voices.
Jaffe and Smith treat bowing as a periodically plucked string.

#### 4.4.3 `model=2` modal — decaying resonator bank

`modes.table` selects a shipped ratio table. `mode.1..24.ratio/gain/decay`
override any entry (`modes.table=0` is all custom). `modes.decayTilt` makes high
modes decay faster. Exciter is mallet (hardness → contact time) or hammer.

Shipped tables (`ModeTables.h`, each with its citation in a comment):

| id | table | ratios (first modes) | source |
|---|---|---|---|
| 1 | free bar | 1, 2.76, 5.40, 8.93 | Euler–Bernoulli free-free bar |
| 2 | marimba (tuned) | 1, 3.9, 9.2 (≈ 1 : 4 : 10) | undercut bars, tuned 4th/10th |
| 3 | xylophone | 1, 3.0, 6.0 | tuned to the twelfth |
| 4 | vibraphone | 1, 4.0, 10.0 | as marimba, aluminium decay |
| 5 | glockenspiel | 1, 2.76, 5.40 | steel bars, near free-free |
| 6 | tubular bell | strike tone from modes 4, 5, 6 ≈ 2 : 3 : 4 | CCRMA percussion notes |
| 7 | timpani | 1, 1.5, 2.0, 2.5, 3.0 | kettle-tuned near-harmonic modes |
| 8 | steel pan | 1, 2, 3 (+ 4 on bass notes) | Rossing & Hansen: octave + twelfth |
| 9 | handpan | 1, 2, 3 with ±3 cent spread | same tuning practice |
| 10 | church bell | 0.5, 1, 1.2, 1.5, 2, 2.5, 3 | hum, prime, tierce, quint, nominal |
| 11 | tine (Rhodes) | 1, 7.1, 20 + tonebar | electric piano tine + tonebar |
| 12 | reed (Wurlitzer) | 1, 6.3, 17.6 | cantilever reed |
| 13 | plate/gong | 1, 1.59, 2.14, 2.30, 2.65, 2.92 … | circular plate (0,1),(1,1)… |
| 14 | kalimba/mbira | 1, 5.4, 13.5 | plucked cantilever tine |
| 15 | glass | 1, 2.32, 4.25 | wine glass bending modes |

Each table needs its ratios measured against a library sample (VSCO/VCSL, §4.8)
in tranche B, with the measured values replacing the textbook ones.
`tests/cpp/physical/mode_tables_match_references` keeps them honest.

#### 4.4.4 `model=3` tube — waveguide bore with jet or reed

`tube.kind` (flute jet, clarinet reed, conical/sax reed), `breath.pressure`,
`breath.noise`, `reed.stiffness`, `jet.ratio`. The clarinet's odd-harmonic
spectrum falls out of the closed cylinder. This covers instruments the CC0
libraries lack (alto/soprano/bari sax, English horn, bass clarinet,
contrabassoon) at "good synth" rather than "sampled" realism. Phase 16 records
which instruments rely on it.

#### 4.4.5 Budget

16 voices of each model render a 4-bar phrase within 25 % of one 256-frame block
at 48 kHz on the Phase 10 benchmark machine (`physical_cpu_budget`).

### 4.5 DrumSynth models — closes G6

Appended after `Kit` so existing values keep their meaning:

| model | id | circuit-level recipe |
|---|---|---|
| Cowbell | 6 | oscillators 5 + 6 of `kMetal` (540, 800 Hz), band-pass ≈ 2.6 kHz, two-stage decay (fast 15 ms + `decay`) — the 808 cowbell |
| Cymbal | 7 | all six oscillators, split: high band (HP 7 kHz, short) + body band (BP 3.4 kHz, `decay` up to 4 s) |
| Rim | 8 | bridged-T ping at ~1.7 kHz + a 1 ms click, decay ≤ 40 ms |
| Clave | 9 | bridged-T ping at ~2.5 kHz, decay 20–60 ms |
| Shaker | 10 | high-passed noise through a 2-stage envelope, `tone` = HP cutoff |
| Conga | 11 | tuned tom with pitch blip and higher Q, three tunings by key |

Plus one parameter for all models, `character` (0..2: 808, 909, 707-ish), which
selects the voicing tables (909 kick: sine + click sample-ish transient, faster
drop; 909 snare: two triangle-ish oscillators + noisier snappy). `tone` and
`decay` keep their cross-model meaning (DrumSynthParams.h's rule).

The 808 numbers are the ones `DrumSynth.cpp` already uses (205.3, 304.4, 369.6,
522.7, 540, 800 Hz). Real units vary ("large differences" between machines,
TipTop Audio's measurements), so `tune` is the honest knob, not a correction.

### 4.6 Effects — closes G5

| Type | Parameters | Why the packs need it |
|---|---|---|
| `Clipper` | `mode` (soft, hard, sine-fold, foldback, asym-tube), `drive` 0..48 dB, `ceiling`, `oversample` (1/2/4/8/16x), `dcBlock` | Every hardstyle kick chain is EQ → clip → EQ → clip ("hard clipping … common in rawstyle"). The tanh `Distortion` and the pedal `Overdrive` are not clippers. |
| `Shaper` | `curve` (a `core::Curve` multi-knot transfer), `drive`, `mix`, `oversample` | custom waveshaping; the "phase distortion" reverse-bass step |
| `Filter` | `type` (ladder, LP, BP, HP, notch, comb+, comb−, formant), `cutoff`, `resonance`, `drive`, `lfo.rate/sync/shape/depth`, `follow.amount/attack/release`, `keyInput` | Madeon-style filtered chords, risers, build-up sweeps. Only EQ cuts existed. |
| `Resonator` | `voices` 1..4, `note1..4` (semitones from C4), `feedback`, `damping`, `mix` | "a comb filter can give a nice formant sound" (screeches), metallic hyperpop, tuned noise |
| `Multiband` (ext) | `bandN.upThreshold`, `bandN.upRatio`, `depth`, `time`; shipped `preset=OTT` | OTT is on almost every hyperpop and hardstyle lead; downward-only could not do it |
| `VolumeShaper` | `curve` (Curve, one cycle), `length` (1/16 … 1 bar), `depth`, `steps` (16-step gate mode) | sidechain shapes without a key (Kickstart/LFOTool), trance gates |
| `Ensemble` | `voices` 2..8, `mode` (Juno I, II, I+II, BBD, dimension), `rate`, `depth`, `width` | Alpha Juno chorus "provides most of the body" of the hoover; Juno pads |
| `Tape` | `wow.rate/depth`, `flutter.rate/depth`, `saturation`, `hiss`, `age` (HF loss), `speed` | Memphis/phonk tape, C418 warmth, lo-fi ambient |
| `Hall` | `size`, `decay` 0.1..60 s, `predelay`, `damping`, `modulation`, `shimmer` (0..1), `shimmerInterval` (+12/+7/+19), `width`, `freeze` | ambient tails beyond Freeverb, shimmer pads, euphoric hardstyle space |
| `Rotary` | `speed` (slow/fast), `ramp`, `hornLevel`, `drumLevel`, `distance`, `drive` | Hammond/organ in band and Dorian-Concept keys |
| `PitchCorrect` | `key`, `scale`, `retune` 0..400 ms, `humanize`, `formant` (keep/shift st), `vibratoPass` | hyperpop hard-tune ("hard tuning with a fast retune speed"); vocal chains |

All effects follow Phase 4's `Effect` contract (`processBlock`, `clone()` for
offline render, atomic params, zero allocation). Each gets a numerical test (a
known transfer curve, an impulse response, a measured pitch) and golden hashes at
three settings, as every Phase 4 effect has.

### 4.7 Samples at scale — closes G7 (engine half)

#### 4.7.1 Zone extensions

`SampleZone` gains:

- `switchLo/switchHi`: a keyswitch range. Zones belong to the group whose switch
  key was pressed last (per channel), so one sampler preset can hold sustain,
  staccato, pizzicato and tremolo at once (VSCO's articulations).
- `trigger` (attack, release). Release zones fire on note-off with the note's
  velocity and an `rtDecay` gain fall-off by held time. These are the piano and
  harpsichord release noises.
- `xfadeLo/xfadeHi`: equal-power crossfade between dynamic layers instead of a
  hard switch (Iowa's pp/mf/ff). The source is the sampler's new `xfade.source`
  parameter: velocity (default), or the new `dynamics` parameter. `dynamics` is
  automatable and CC1-learnable, and it moves *while a note sustains*, which is
  how every orchestral library plays a swell. Phase 16's sustains depend on it.
- `offBy`/`group`: choke groups (open/closed hats, marching snare chokes).

Round-robin already exists and is kept.

#### 4.7.2 SFZ import

VSCO 2 CE and VCSL ship SFZ mappings. Writing thousands of zone lines by hand
is exactly the error-prone work this phase removes. The parser accepts the
subset these libraries use: `<control> default_path`, `<global>/<group>/<region>`,
`sample`, `lokey/hikey/key`, `pitch_keycenter`, `lovel/hivel`, `seq_length/seq_position`,
`trigger=release`, `rt_decay`, `loop_mode/loop_start/loop_end`, `ampeg_attack/decay/sustain/release`,
`tune/transpose/volume/pan`, `group/off_by`, `sw_lokey/sw_hikey/sw_last`,
`xfin_*/xfout_*`. An unknown opcode is a warning with line and column and is
**listed** in the import report, never dropped silently (§6 rule 4's spirit).

The parser also takes the ARIA/SFZ v2 opcodes that Plogue's converters
(sforzando, SF22SFZ) write, so a bank converted that way loads directly:

- `<master>` headers;
- `offset`/`end`, for regions that slice one shared WAV;
- `volume` per region;
- `<curve>` headers (`v000`–`v127`, 128 points) and `amplitude_curvecc11`, which
  selects one;
- `set_ccN` power-on defaults;
- `amplitude_oncc11`;
- `egNN_time/level/shape` with `egNN_pitch` and `egNN_sustain`, which map onto a
  §4.3 mod envelope;
- `lfoNN_wave/freq/delay/phase/pitch/amplitude` and `lfoNN_pitch_onccN`, which map
  onto the §4.3 LFOs.

The local `ARIAConverted/` bank is the fixture for these: the Windows `gm.dls`
converted to SF2 and then to SFZ by sforzando (235 files, 1498 regions, one shared
24 kHz 16-bit WAV, melodic banks 000–009 plus the drum bank 128). It is git-ignored
(Roland-copyrighted) and tests that use it skip when it is absent.
Its `eg06` pitch envelopes and `lfo06`/`lfo07` vibrato are what make the Windows
sound recognisable, so `sfz_import_aria_gm` (§5) checks them, not just the key map.

#### 4.7.4 SF2 and DLS import — closes G8 (format half)

SFZ was the only importer at first because every chosen library ships it. Banks
were put in scope on 2026-11-02 because the user wants a Fruity LSD-style GM
player. Converting a bank to SFZ outside adX loses its modulators and DLS
articulation, and SFZ has nothing for GM program and bank switching.

**Per-zone articulation first.** Right now the sampler's `env.*` is per instrument
and it has no filter. In SF2 and DLS every zone has its own. `SampleZone` gets an
optional `ZoneArticulation`, and with it absent the zone renders exactly as it does
today, so every golden hash stays put:

- a DAHDSR amp envelope (delay, attack, hold, decay, sustain, release), plus SF2's
  key-to-hold and key-to-decay scaling;
- a DAHDSR mod envelope routed to pitch and filter cutoff;
- a 2-pole resonant low-pass (SF2 `initialFilterFc`/`Q`: the TPT SVF from `engine/dsp`);
- a vibrato LFO (pitch only) and a mod LFO (pitch, cutoff, volume), each with a delay;
- `exclusiveClass`, mapped onto the §4.7.1 `group`/`offBy` choke (so open and
  closed hats cut each other off, as in GM).

These are per-voice and use the §4.3 envelope and LFO code, not a second
implementation.

**The parsers.** Both share a RIFF walker that checks every chunk size against
its parent and the file. Truncated or hostile files get a diagnostic, never an
out-of-bounds read, and each parser is fuzzed (§5). Parsing happens on the main
thread at load time. Samples go into the existing `SamplePool` as float, so the
audio thread never sees a bank file.

- **SF2 2.01/2.04:** `phdr → pbag → pgen/pmod → inst → ibag → igen/imod → shdr`,
  global zones included. Preset-level generators add to instrument-level ones,
  as the spec says. `sm24` gives 24-bit samples. Sample modes 0/1/3 map to
  `LoopMode` Off, Continuous and Sustain-loop-then-release. Units are converted
  once in `Generators.cpp`: timecents → s, centibels → dB, absolute cents → Hz.
- **Modulators:** the SF2 2.04 default set is always applied: velocity →
  attenuation and cutoff, CC1 → vibrato depth, CC7/CC11 → volume, CC10 → pan,
  pitch wheel with RPN 0 range. A bank's own `pmod`/`imod` entries are mapped
  when their source and destination are in that same set, and land in the import
  report otherwise.
- **DLS level 1 and 2:** `lins` instruments, `rgn`/`rgn2` key and velocity ranges,
  `wsmp` (unity note, fine tune, attenuation, loops), `wlnk` into the `wvpl` wave
  pool, and `art1`/`art2` connection blocks mapped to the same `ZoneArticulation`.
  DLS's drum flag (`F_INSTRUMENT_DRUMS`) marks a percussion bank.
- **Not read:** SF3 (Ogg-compressed SF2) and SF2 `sfInstMod` sources outside
  the default set. Both are named in the import report, not ignored silently.

Rather than taking FluidSynth's LGPL code, adX does its own parsing. Both formats
are small, fully documented RIFF schemas, and adX needs their zones in its own
types anyway. TinySoundFont (MIT) and FluidSynth renders are **references only**,
used in `sf2_matches_reference` (§5).

#### 4.7.5 The `gm` instrument — closes G8 (player half)

This is the Fruity LSD equivalent: a new instrument type, `gm`, that plays a bank
by GM program number.

| Param | Range | Meaning |
|---|---|---|
| `bank` | path | an `.sf2` or `.dls`. Default `system:gm.dls` (the Windows GS Wavetable bank at `%SystemRoot%\System32\drivers\gm.dls`), or the shipped GM bank when that file doesn't exist |
| `program` | 0–127 | GM program, shown by name ("Acoustic Grand Piano") |
| `bankMsb` / `bankLsb` | 0–127 | GS/XG variation banks, when the bank has them |
| `drums` | bool | play bank 128 (the GM kit). Also on automatically for MIDI channel 10 in part mode |
| `part` | single / 16-part | **single**: one program per channel, the normal case. **16-part**: one channel answers all 16 MIDI channels, each with its own program, volume, pan and bend, the way LSD does |

- Program change, bank select (CC0/CC32), CC1/7/10/11/64, RPN 0 (bend range),
  All Notes Off and Reset All Controllers are honoured. These come from live MIDI
  input (Phase 9 §4.1) and, in 16-part mode, from note clips that carry
  per-note MIDI channels.
- **MIDI import gets a "Play on one GM channel" option.** The Phase 9 importer
  already splits a file per channel. With this option those channels become
  parts of a single 16-part `gm` channel and keep their program changes. A
  dropped-in `.mid` then sounds right with no setup.
- `GmBankCache` parses a bank once, no matter how many channels use it. Even
  FluidR3 GM (~140 MB) is held in RAM once.
- **Until the DLS parser lands, the `gm` default is `ARIAConverted/`.** The
  `gm` instrument reads a bank folder as well as a file (one SFZ per program,
  `NNN/NNN_name.sfz`, drums in `128/`), so it works on the user's converted
  Windows bank from the first build. The direct `gm.dls` path comes later and
  is checked against that conversion.
- **Licences.** `gm.dls` belongs to Microsoft and is not redistributable. It is
  read from the user's own Windows install and never copied into a project,
  render cache or installer (`REDISTRIBUTABLE=no`, enforced by `licence_gate`,
  as with Iowa MIS). The bank adX ships is fetched through the library store
  (§4.8) with a pinned hash. FluidR3_GM (MIT) is the candidate; GeneralUser GS
  is the alternative if its licence checks out when the manifest is written.
- A preset is one program at heart, so `import_soundfont.py` can also turn any
  bank preset into a plain `.adxpreset` sampler for anyone who wants to edit it
  beyond what `gm` exposes.

#### 4.7.3 `lib://` paths

A zone path `lib://vsco2ce/Strings/Violin Section/susVib/...wav` resolves against
the library store (§4.8). When the library is absent, the preset still loads:
the browser shows it with a "needs library" badge, and the channel plays the
preset's declared `FALLBACK=` (a synthesized preset from the same pack) and
records a diagnostic. A missing library never plays silence with no explanation,
for the same reason a pad that does nothing is the worst live failure
(phase_11.md §4.4).

### 4.8 The library store — closes G7 (data half)

Libraries are **data, not dependencies**: no code links against them, so §9's
dependency rule is satisfied by recording them in `pack.adxpack`, not by adding
anything to CMake.

```
%LOCALAPPDATA%\adX\libraries\
  vsco2ce@<commit>\        3.1 GB   CC0-1.0   (github.com/sgossner/VSCO-2-CE)
  vcsl@<commit>\           5.9 GB   CC0-1.0   (github.com/sgossner/VCSL)
  salamander@v3\          ~1.9 GB   CC-BY-3.0 (credit kept in THIRD-PARTY.md)
  iowa-mis\               ~6 GB     "free to use without restrictions" - fetch only
  rendered\<pack>\        ~2-6 GB   produced by render_multisample.py / kick_lab.py
```

**Budget: 100 GB** (the user's ceiling, 2026-10-25). The plan above uses about
25 GB at its peak, so there is room for a second piano or a larger rendered set
if a human gate asks for one.

`tools/packs/fetch_libraries.py` downloads each library at a **pinned commit**,
verifies a committed SHA-256 manifest (`engine/preset/packs/<pack>/libraries.sha256`),
and refuses a mismatch. Iowa MIS's terms grant unrestricted *use* but say
nothing about redistribution. It is therefore fetched from the university by
the user's own machine and is never bundled in the installer. `licence_report.py`
fails the build if a `REDISTRIBUTABLE=no` asset ends up in an installer
component.

**CI never downloads 25 GB.** Per push, sample-based presets are tested through
their fallbacks plus a ≤ 200 MB "smoke subset" (one region per instrument),
cached by `actions/cache` keyed on the manifest hash. Full sampled-preset golden
hashes run locally (`ctest -L libraries`) and in the full check, as `[.slow]`
does today.

### 4.9 Rendering tools

- **`render_multisample.py`** renders any preset or rack through `adx render`
  (Phase 8). The default is every 3 semitones × 3 velocities × 2 round-robins
  (the RR comes from the seed). It writes 24-bit WAV and a sampler preset with
  `lib://rendered/...` zones. This serves physical models that are too costly
  at full polyphony, and lets the Classical pack offer "adX-rendered" variants
  that are reproducible from source.
- **`kick_lab.py`** is the hardstyle resample loop, automated. Producers
  "render to audio, reprocess, then transpose per note … the loop can run more
  than once". The tool renders a kick rack once per semitone across E1–E2,
  passes each through the rack's post-chain again N times (configurable), and
  builds a sampler preset with one zone per key. The tail is then tuned per note
  by construction rather than by resampling an already-distorted kick, which is
  the quality difference producers chase. It is deterministic, so golden-hashable.

### 4.10 Generators

- **`gen_wavetables.py`**: numpy, seeded, written as 2048-sample-frame WAV tables:
  - `hypersaw`: saw → phase-offset saws, 64 frames;
  - `juno-pwm-saw`: a saw with one and two "holes" cut by a pulse. The research
    disagrees on how the Alpha Juno makes its PWM saw (saw × inverted pulse vs.
    two counter-detuned saws), so both are generated as separate tables, and
    the hoover human gate picks;
  - `formant-vowels`: A-E-I-O-U glottal tables;
  - `fm-sweeps`: 2-op FM at index 0..12;
  - `ppg-digital`: stepped/harsh digital frames;
  - `chip`: NES pulse 12.5/25/50 %, triangle (4-bit stepped), Game Boy wave;
  - `organ-drawbars`: nine-drawbar registrations;
  - `vocal-ah` and `growl` tables for neuro/hyperpop.
- **`gen_irs.py`**: seeded synthetic IRs, written as 48 kHz WAV:
  - small/medium/large rooms;
  - plates;
  - a spring;
  - guitar cabinets 1×12, 2×12 and 4×12 (filtered-noise approximations of
    measured responses);
  - acoustic guitar and violin bodies for `physical`'s commuted model;
  - an orchestral stage set (front/mid/back early reflections).

Generated assets are committed (they are small), so a fresh clone with no
library store still has every synthesized preset complete.

### 4.11 Audition, taxonomy, loudness

- **Categories** (fixed, so browsing works across packs): `Drums/{Kick,Snare,Clap,Hat,Cymbal,Perc,Kit,Break}`,
  `Bass/{Sub,808,Reese,Synth,Acoustic}`, `Lead`, `Pluck`, `Keys/{Piano,EP,Organ,Mallet}`,
  `Pad`, `Strings`, `Brass`, `Woodwind`, `Guitar`, `Choir/Vocal`, `FX/{Riser,Impact,Texture}`,
  `Chain/{Kick,Bus,Vocal,Master,Space}`.
- **Fixed macro slots per category** (example: every `Lead` uses 1 Brightness,
  2 Detune, 3 Vibrato, 4 Drive, 5 Attack, 6 Release, 7 Space, 8 Width).
- **`audition.py`** plays a category-standard phrase through each preset at its
  hinted tempo and root. Leads get a 2-bar melody, pads a 4-voice chord, kicks
  a 4-on-the-floor bar with root moves, and so on. It writes the WAV, a spectrum
  PNG, integrated LUFS and true peak. The browser plays the cached preview on
  click without loading the preset.
- **Loudness window:** each preset's audition LUFS must sit within ±2 LU of
  its category target (Kick −8, Lead −12, Pad −16, Keys −14, FX −14 …), which
  is enforced by `pack_loudness_window`, so browsing never jumps by 15 dB.

---

## 5. Tests

| Test | Asserts |
|---|---|
| `golden_unchanged_by_phase12` | **the regression gate**: every hash recorded at Phase 11's end is identical in all three configurations |
| `rack_roundtrip` | `.adxrack` parse → write → parse is byte-identical; unknown keys preserved and warned |
| `rack_insert_one_undo` | inserting a 3-layer rack with chain, send and 2 macros is one undo entry; undo restores byte-identical `.adx` |
| `rack_expands_to_ordinary_entities` | after insertion the project contains only Channel/Insert/Route/Macro — no rack entity |
| `save_rack_inverse` | SaveRack(InsertRack(r)) ≡ r for every shipped demo rack |
| `macro_targets_compile` | a macro at 0, 0.5 and 1 gives exactly `min + (max−min)·curve(v)` on each target, sample-accurate |
| `macro_offsets_automation` | an automated target plus a macro sums in the normalised domain |
| `modmatrix_each_source_dest` | table-driven: every source × every destination of each type moves the output measurably and only the destination |
| `va_lfo_target_equals_matrix_slot` | VA's legacy `lfo1.target` and the equivalent matrix slot render bit-identically |
| `pitch_env_exact` | the measured instantaneous pitch follows amount/time/curve within 1 cent after 5 ms |
| `legato_no_retrigger` | overlapping notes in legato mode: env not restarted, glide applied; in mono mode restarted |
| `lfo_sync_tempo_change` | a synced LFO stays phase-locked across a tempo ramp |
| `physical_pluck_pitch` | 40 notes A0–C8 within ±3 cents (fractional-delay tuning) |
| `physical_pluck_decay` | T60 matches `string.decay` within 10 % at C4 |
| `mode_tables_match_references` | each table's first three modes are within 2 % of the ratios measured from its reference sample |
| `physical_cpu_budget` | §4.4.5 |
| `drum_new_models` | cowbell spectrum peaks near 540 and 800 Hz (× tune); clave/rim decay windows; shaker HP |
| `clipper_transfer` | each mode's static curve matches its formula; 16x oversampled aliasing < −90 dBFS at 10 kHz input |
| `ott_upward` | a −40 dBFS band rises by the configured upward ratio; a −6 dBFS band is pulled down |
| `hall_decay` | measured RT60 within 10 % of `decay` at 0.5, 4 and 30 s |
| `pitchcorrect_snaps` | a 30-cent-flat sine snaps to the scale degree within `retune` ms |
| `volume_shaper_sync` | the gain curve repeats exactly every `length` at 4 tempos |
| `effects_no_alloc_new` | every new effect, 60 s under the allocator hook: 0 violations |
| `sampler_keyswitch` | the last switch key selects the group; switch keys themselves are silent |
| `sampler_release_trigger` | a release zone fires on note-off with `rt_decay` applied |
| `sfz_import_vsco_vcsl` | every SFZ in the smoke subset imports; unknown opcodes are reported, not dropped |
| `zone_articulation_inert` | a zone without `ZoneArticulation` renders bit-identically to before; part of the golden gate |
| `zone_articulation_dahdsr` | each stage's measured length and level match the generator values within 1 ms / 0.1 dB, key-to-hold/decay scaling included |
| `sf2_generator_units` | table-driven timecents/centibels/absolute-cents conversions match the SF2 2.04 formulas exactly |
| `sf2_preset_adds_instrument` | preset-level generators add to instrument-level ones; global zones apply; ranges intersect |
| `sf2_loop_modes` | modes 0/1/3 on `tiny_gm.sf2`: loop points sample-exact, mode 3 leaves the loop on release |
| `sf2_exclusive_class` | closed hat chokes open hat within one block |
| `sf2_default_modulators` | velocity → attenuation and cutoff, CC1 vibrato, CC7/11/10 and bend range all audible and correct |
| `sf2_matches_reference` | for each fixture program, pitch within 1 cent and the amp envelope within 0.5 dB of a TinySoundFont render, sampled every 10 ms |
| `dls_parse_and_map` | `tiny_gm.dls` loads; `art1`/`art2` map to the same articulation as the equivalent SF2 fixture |
| `dls_system_gm` | (local, skipped when the file is absent) the system `gm.dls` loads all 128 programs plus the drum kit, and C4 on each one is non-silent |
| `soundfont_fuzz` | 10k mutated SF2/DLS files under ASan: no crash, no out-of-bounds read; every reject has a diagnostic |
| `soundfont_unknown_reported` | unmapped generators and modulators, and SF3, show up in the import report |
| `gm_program_change` | a program change mid-clip switches the next note's zones; ringing notes keep theirs |
| `gm_16part_midi_import` | a 16-channel `.mid` imported to one `gm` channel renders the same as 16 single-part channels set the same way |
| `gm_drums_channel10` | in part mode, channel 10 plays bank 128 without being told to |
| `gm_bank_shared` | 20 `gm` channels on one bank add the bank's RAM once |
| `gm_no_alloc` | 60 s of 16-part GM playback with program changes under the allocator hook: 0 violations |
| `gm_dls_never_copied` | saving, render-caching or packaging a project that uses `system:gm.dls` writes none of its bytes |
| `sfz_import_aria_gm` | (local, skipped when `ARIAConverted/` is absent) all 235 files import with zero dropped opcodes; each program's regions cover the same keys as `gm.dls`; a `eg06` pitch dive and `lfo07` vibrato audibly move a Reed Organ note |
| `lib_path_fallback` | a missing library loads the preset, plays the fallback and raises one diagnostic |
| `fetch_verifies_hashes` | a corrupted file in the store is detected and re-fetched |
| `licence_gate` | a `REDISTRIBUTABLE=no` asset in an installer component fails the build |
| `kick_lab_deterministic` | two runs of `kick_lab.py` on the same rack produce identical WAVs |
| `audition_loudness` | the audition tool's LUFS matches Phase 4's BS.1770-4 meter within 0.1 LU |
| `browser_audition_click` | clicking a preset plays its cached preview with 0 engine graph changes |

---

## 6. Definition of done

- [ ] `golden_unchanged_by_phase12` passes on Debug, RelWithDebInfo and Release.
- [ ] Every test in §5 passes locally and in CI (library-tagged ones in the full check).
- [ ] **The demonstration rack set exists, one per gap**, each rendered and
      listened to: G1 a 3-layer kick rack with chain and ducked send; G2 the same
      with two macros; G3 a hoover-style patch with a pitch dive and late
      vibrato; G4 a plucked guitar, a marimba and a bowed cello; G5 an OTT'd
      clipped lead into a shimmer hall; G6 an 808 cowbell melody; G7 a
      keyswitched VSCO violin section from SFZ; G8 the WannaCry `.mid` imported
      onto one 16-part `gm` channel, played once on the system `gm.dls` and once
      on the shipped GM bank.
- [ ] `docs/sound-design.md` written; `docs/adx-format-v2.md` covers `[MACRO]`, `.adxrack`, `pack.adxpack`, `lib://`.
- [ ] Library fetch verified for VSCO 2 CE, VCSL and Salamander, run by the agent on
      this machine (fetch, hash check, and one keyswitched render per library).
- [ ] **Human gate:** the user plays the demonstration set in adX and says it is
      ready for pack building, recorded in their own words. (judgement) Before the
      user is asked, an agent has checked that every rack loads, renders without
      `adx glitch` findings, and appears in the browser (FINAL_PLAN §9).
- [ ] FINAL_PLAN §10 row and STATE.md updated; launcher rebuilt and checked.

---

## 7. Out of scope

| Thing | Status |
|---|---|
| The packs themselves | Phases 13–19 |
| Kontakt/NKI, EXS import | not planned. Proprietary or undocumented formats; convert to SFZ externally |
| SF3 (compressed SF2), SF2 modulators outside the default set | reported on import, not played. Revisit if a bank the user wants depends on them |
| Disk streaming of samples | not planned unless `pack_memory_budget` (Phase 16) fails. Libraries load into RAM per preset |
| Neural timbre transfer / DiffSinger | stays behind the optional ONNX flag (§8.2) |

---

## 8. Handoff to Phase 13

Phase 13 needs: `.adxrack` and macros, DrumSynth's new models, Clipper,
Filter, VolumeShaper, Tape, `audition.py`, the category taxonomy and loudness
targets, and VCSL fetched (its snares, toms, cymbals and hats seed the acoustic kits).

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| Adding parameters to existing types moves golden hashes | inert defaults, and the gate test is the first thing built (§3 step 1) |
| Physical models sound "synthy" next to samples | they are positioned as the gap-fillers and the expressive solo option. Phase 16 records per instrument which source won its human A/B |
| PitchCorrect (YIN + PSOLA) is a research project on its own | ship hard-tune (retune 0–40 ms) first, which is what hyperpop needs; natural correction may follow |
| Library upstreams change or vanish | pinned commits + SHA-256 manifests; the store can be re-seeded from a user backup |
| Iowa MIS redistribution | fetch-only, never bundled; `licence_gate` enforces it |
| `gm.dls` is Microsoft's and leaks into a shared project or installer | read in place only; `gm_dls_never_copied` and `licence_gate`; a project shared with a non-Windows machine falls back to the shipped bank with a diagnostic |
| SF2 banks are mapped differently by every player, so "sounds wrong" is subjective | the SF2 2.04 spec is the authority; TinySoundFont is the numeric reference; the G8 demo is human-gated |
| Bank parsing is an attack surface (users load files off the internet) | the RIFF walker checks every size; `soundfont_fuzz` runs under ASan in the full check |
| 8 effects + 1 instrument + modulation layer is a lot for one phase | tranches A/B/C, as Phase 4; Phase 13 needs only part of it (§8) and may start once those parts are done |

---

## 10. Sources

Research for this phase and the packs, consulted 2026-10-25:

- Smith, J.O., *Physical Audio Signal Processing* (CCRMA): [strings](https://ccrma.stanford.edu/~jos/pmupd/Strings.html), [FM brass / Risset](https://ccrma.stanford.edu/~jos/sasp/FM_Brass.html)
- Essl, *Physical Wave Propagation Modeling*, Princeton TR-659-02 — [modal vs. waveguide](https://www.cs.princeton.edu/techreports/2002/659.pdf)
- Jaffe & Smith 1995, commuted bowed strings — [pdf](https://ccrma.stanford.edu/%7ejos/pdf/JaffeSmith95.pdf)
- CCRMA percussion notes — [modes of percussion](https://ccrma.stanford.edu/courses/150-spring-2003/percussion.html); Hibberts — [tubular bells](https://www.hibberts.co.uk/tubular-bells/)
- Rossing & Hansen — [steelpan acoustics](https://www.hangblog.org/the-caribbean-steelpan-and-some-offsprings), [bass steelpan modes](https://labs.sonicfield.org/library/vibrational-modes-and-sound-spectra-of-bass-steelpans)
- Werner, Abel & Smith, TR-808 cymbal (ICMC-SMC 2014) — [pdf](https://speech.di.uoa.gr/ICMC-SMC-2014/images/VOL_2/1453.pdf); Sound On Sound — [practical snare synthesis](https://www.soundonsound.com/techniques/practical-snare-drum-synthesis)
- Versilian Studios — [VSCO 2 CE](https://versilian-studios.com/vsco-community/) (CC0), [VCSL](https://github.com/sgossner/VCSL) (CC0)
- University of Iowa — [Musical Instrument Samples](https://theremin.music.uiowa.edu/MIS.html) ("used for any projects, without restrictions")
- Salamander Grand Piano — [sfz repository](https://github.com/sfzinstruments/SalamanderGrandPiano) (CC BY 3.0)
- SoundFont 2.04 specification (E-mu/Creative, 2005); MMA *Downloadable Sounds Level 1* and *Level 2* specifications
- TinySoundFont — [github.com/schellingb/TinySoundFont](https://github.com/schellingb/TinySoundFont) (MIT, reference renders only); FluidR3_GM (MIT)

---

## 11. Corrections

Where the build differs from the plan above, and why. Recorded as the work lands.

1. **Effects grouped by family, not one file each (§2).** Clipper is
   `engine/effects/Clipper.h/.cpp`; Filter and Resonator share
   `engine/effects/Filter.h/.cpp`; VolumeShaper, Tape, Ensemble and Rotary share
   `engine/effects/Motion.h/.cpp`. That's the existing convention (`Modulation.cpp`
   holds Chorus, Flanger, Phaser and Tremolo). The type names and parameters are as
   §4.6 says.
2. **Clipper is always 16x (§4.6 `oversample`).** Two nested `Oversampler4x`
   stages; there's no 2x or 8x stage, and an `oversample` choice would change the
   effect's latency at run time, which the slot's latency compensation can't follow.
   The latency is fixed at `kClipperLatency` (50 frames). `clipper_transfer` checks
   soft-clip aliasing below −90 dBc on a 10 kHz tone at +12 dB drive. Hard clip is
   checked at −60 dBc, because its corner has harmonics far past 16x's Nyquist. The
   plan's −90 dBFS holds for the smooth curves.
3. **VolumeShaper's curve is a `shape` choice for now (§4.6 `curve`).** Pump, ramp
   up, ramp down and sine, plus the 16-step gate (`steps`, a bitmask). A `core::Curve`
   parameter needs the curve-valued parameter path that `Shaper` will add, and
   VolumeShaper will take it then. The phase comes from exact fractional beats
   through the tempo map (GrossBeat's `beatsAt`), so `volume_shaper_sync` holds a
   repeat within 1e-5 at 90, 100, 120 and 150 BPM.
4. **The Resonator has no `mix` parameter of its own.** Every slot already has
   `mix` (Phase 4's slot parameters).
5. **Modulated effects run while bypassed.** Ensemble, Filter, Resonator and Rotary
   build their output from recent input, so `effect_bypass_is_click_free` would hear
   stale history on un-bypass. They override `runsWhileBypassed()`, as Chorus and
   Flanger do.
