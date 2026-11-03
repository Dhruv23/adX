# Phase 16 — Pack: Classical (orchestra & band) · XL

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `engine/preset/packs/classical/` — every instrument of the symphony orchestra, the concert/marching band, and the rock/jazz band; articulations, sections, seating, orchestral templates |
| **FINAL_PLAN refs** | §5.3 (sampler, `physical`), §5.12, §7 Phase 16 |
| **Entry criteria** | [phase_15.md](phase_15.md) §7 complete; Phase 12's sampler extensions, SFZ import and library store working with VSCO 2 CE, VCSL, Salamander and Iowa MIS fetched |
| **Next** | [phase_17.md](phase_17.md) |
| **Pack id** | `classical` · `REQUIRES=core` |
| **Sources allowed** | pure synthesis · adX-rendered multisamples · CC0/free libraries (user decision, 2026-10-25); library budget ≤ 100 GB |

---

## 1. Objective

Every instrument seen in an orchestra or a band, playable from adX with
articulations, dynamics and realistic placement. Each instrument uses **the
most realistic source available** and has a **synthesized fallback** so it never
plays silence. A pack this large risks quiet gaps, so it is driven by one
instrument matrix (§4). Every row has a source, its articulations, a fallback
and a human verdict.

---

## 2. Research summary

**Libraries** (inventoried from the repositories' trees, 2026-10-25):

- **VSCO 2 CE** (CC0, 3.1 GB, 3,168 WAVs):
  - strings: violin/viola/cello sections, solo violin, solo contrabass, each
    with sus-vib, pizz, spiccato and tremolo; harp;
  - brass: F horn (sus, stac, mute), trumpet (sus, susvib, stac, harmon and
    straight mutes), tenor trombone (sus, stac, vib), an "old trombone"
    (sustain, short, vibrato, fall, buzz), tuba;
  - woodwinds: piccolo, flute (non-vib, vib, expressive vib, stac), oboe,
    clarinet, bassoon;
  - keys: organ (loud/quiet), upright piano;
  - percussion: glock, marimba, xylophone, timpani + rolls, and a "VSCO 1"
    set (marching snare/tenor/bass, cymbals, gong, triangle, log drum, shaker).
- **VCSL** (CC0, 5.9 GB, ~4,200 files):
  - concert and folk harp;
  - grand pianos (Steinway B, Kawai) and uprights (Knight, Yamaha);
  - five harpsichords;
  - pipe and Renaissance organs;
  - recorders (soprano–bass), ocarinas, tenor sax, saxello, harmonica;
  - tuned percussion: glockenspiel, xylophone, marimba, vibraphone, tubular
    bells (3 sets), tubular glockenspiel, hand chimes;
  - orchestral percussion: two timpani sets (one with 204 samples), snares,
    bass drums, toms, clash and suspended cymbals, gongs, triangles,
    tambourines, ratchet, slapstick, anvil, mark tree, bell tree, sleigh bells,
    woodblock, claves;
  - TX81Z FM pianos.
- **Iowa MIS** ("used for any projects, without restrictions"; fetch-only,
  per Phase 12 §4.8). Anechoic, three dynamics, note by note. Its catalogue is
  the gap-filler for instruments the CC0 sets lack: alto/bass flute, E♭ and
  bass clarinet, soprano and alto sax, bass trombone and crotales. **The exact
  holdings are verified against the MIS page in tranche A before any row
  depends on them.**
- **Salamander Grand** (CC BY 3.0, later reported public domain; credit kept
  either way).

**Synthesis principles**, for the fallbacks and gap-fillers:

- *Brass:* brightness tracks loudness. Risset found brass bandwidth roughly
  proportional to amplitude, and Chowning's FM brass ties the modulation index
  to the carrier envelope. In adX: `fm` with `mod: modEnv1 → op2 level` and
  `velocity → op2 level`, or `additive` with a velocity → filter matrix slot.
- *Strings:* `physical` bow for solos; sections need many independent players,
  so sections stay sampled.
- *Clarinet:* odd harmonics come from the closed cylinder (`physical` tube).
- *Bells, mallets, timpani, celesta:* modal tables (Phase 12 §4.4.3).
- *Piano/harp/guitar:* commuted waveguide (pluck/hammer).

---

## 3. Conventions

- **Articulations** are one sampler preset per instrument with keyswitches on
  C0 upward (C0 sustain, C♯0 staccato, D0 pizzicato, D♯0 tremolo, E0 mute, F0
  vibrato-less …). The same switch key means the same articulation in every
  preset. Each articulation also ships as its own preset for users who prefer
  one track per articulation.
- **Dynamics:** sustains crossfade on `dynamics` (CC1, Phase 12 §4.7.1); shorts
  use velocity.
- **Release triggers** on piano, harpsichord, harp and pizzicato where the library
  has them.
- **Seating:** every preset has a default pan and a `Stage` send. Stage racks
  are Convolution early reflections per section (front/mid/back,
  `gen_irs.py`) feeding a shared `Concert Hall` (Hall 2.4 s). The default pans:
  violins I −40, violins II −20, violas +15, cellos +35, basses +45, woodwinds
  centre-back, horns −25 back, trumpets +15 back, trombones/tuba +35 back,
  timpani centre-back, harp −45.
- **Humanize** on section presets: per-note random start (±8 ms) and tuning (±4 cents) via the mod matrix's random source.
- **Naming:** `<Instrument> <Articulation>`, with a `(Synth)` or `(Rendered)`
  suffix only when the source isn't a library.
- Source letters below: **V** VSCO 2 CE, **C** VCSL, **I** Iowa MIS, **S** Salamander, **P** `physical`, **F** `fm`, **A** `additive`, **R** adX-rendered multisample, **D** DrumSynth.

---

## 4. Instrument matrix

### 4.1 Strings

| Instrument | Primary | Articulations | Fallback / gap-fill |
|---|---|---|---|
| Violin section | V | sus vib, spic, pizz, trem | A saw-ensemble strings (Synth) |
| Violin solo | V | arco vib, spic, pizz, trem | P bow (expressive legato solo) |
| Viola section | V | sus vib, spic, pizz, trem | A |
| Cello section | V | sus vib, spic, pizz, trem | A |
| Cello solo | P bow + R | legato, sus | V section thinned (no solo cello in V/C; I if present) |
| Contrabass solo | V | sus vib, sus non-vib, spic, pizz, trem | A |
| Contrabass section | V solo ×3 layered, detuned ±5 c, offset ±12 ms | — | — |
| Harp (concert) | C + V | pluck, harmonics where present, gliss template | P pluck, body harp |
| Full strings | rack: the five sections, divisi off/on | sus, spic, pizz | — |

### 4.2 Woodwinds

| Instrument | Primary | Articulations | Fallback / gap-fill |
|---|---|---|---|
| Piccolo | V | sus, stac | P tube flute +12 |
| Flute | V | sus non-vib, sus vib, expressive vib, stac | P tube flute |
| Alto flute | I | sus | P tube flute −5 |
| Oboe | V | sus, vib, stac | P tube (reed, conical) |
| English horn | **P** tube conical, tuned F | sus, stac | R of P (gap: no CC0 recording) |
| Clarinet (B♭) | V | sus long, stac | P tube clarinet |
| Clarinet E♭ | I | sus | P tube clarinet +5 |
| Bass clarinet | I | sus | P tube clarinet −12 |
| Bassoon | V | sus, vib, stac | P tube conical |
| Contrabassoon | **P** + V bassoon −12 blended | sus | gap noted |
| Recorders S/A/T/B | C | sus | P tube flute |
| Ocarina | C | sus | P tube flute, breath 0.3 |

### 4.3 Brass

| Instrument | Primary | Articulations | Fallback |
|---|---|---|---|
| Trumpet | V | sus, sus vib, stac, harmon mute, straight mute | F brass (Risset/Chowning: index follows env) |
| Cornet | **F** brass, darker (gap) | sus, stac | R |
| Flugelhorn | **F** brass, op2 ratio 1, low index | sus | R |
| F horn | V | sus, stac, mute | F brass, LP 2 kHz |
| Horn section (a4) | rack: V horn ×4, offsets/detune | sus, stac | — |
| Tenor trombone | V (+ "old trombone" falls/buzz) | sus, vib, stac, fall, buzz | F |
| Bass trombone | I | sus | F −5 st |
| Euphonium / baritone | **F** brass mellow (gap) | sus, stac | R |
| Tuba | V | sus, stac | F −12 |
| Brass ensemble | rack: trumpets, horns, trombones, tuba | sus, stac | — |

### 4.4 Percussion

| Instrument | Primary | Notes | Fallback |
|---|---|---|---|
| Timpani | C (Timpani 2, 204 samples) + V rolls | hits, rolls, damped | P modal table 7 + D tom |
| Orchestral bass drum | C Bass Drum 1–2 | hits, roll | D kick char 0, long |
| Concert snare | C Snare Modern + Rope | hits, rolls, rim | core/909 Snare |
| Clash cymbals | C Clash 1–2 | crash, choke | D cymbal |
| Suspended cymbal | C Suspended 1–2 | hits, rolls | D cymbal |
| Tam-tam / gong | C Gong 1–2 + V gong | hits | P modal 13 |
| Triangle | C Triangles (63) | open, muted, roll | P modal 1 high |
| Tambourine | C Tambourine 1–2 | hits, shake, roll | D shaker |
| Glockenspiel | C + V | — | P modal 5 |
| Xylophone | C + V | — | P modal 3 |
| Marimba | C + V | rolls template | P modal 2 |
| Vibraphone | C | motor on/off via Tremolo, pedal = sustain | P modal 4 |
| Tubular bells | C (3 sets) | — | P modal 6 |
| Crotales | I | — | P modal 1 |
| Celesta | **P** modal (gap: no CC0 celesta) | — | F bell, R |
| Wood block / temple blocks | C Woodblock | — | D clave |
| Ratchet, slapstick (whip), anvil, sleigh bells, mark tree, bell tree, flexatone, vibraslap, castanets (D clap short, gap) | C | one-shot kit "Orchestral FX Perc" | D |

### 4.5 Keyboards and choir

| Instrument | Primary | Fallback |
|---|---|---|
| Concert grand | S or C Steinway B (both shipped; gate picks the default) | P hammer piano (Synth) |
| Kawai grand | C | — |
| Upright | C Knight/Yamaha, V upright | — |
| Harpsichord (English, Flemish, French, Italian) | C, with release triggers | P pluck, high brightness, body harpsichord |
| Pipe organ | C Pipe + Renaissance, V organ loud/quiet | A drawbar/rank organ (principal 8′, 4′, mixture, flute 8′ registrations) |
| Celesta | see percussion | — |
| Harmonium | A reed organ (gap) | — |
| Choir (synth) | A formant choir: vowels ah/oh/ee/oo/mm, morph = `formant.morph`, Ensemble 6 voices → Hall | — |
| Choir (sung) | Phase 19 Voice presets — needs the user's own UTAU bank | A choir |

### 4.6 Concert and marching band

| Instrument | Primary | Fallback |
|---|---|---|
| Soprano sax | I | P tube conical |
| Alto sax | I | P tube conical |
| Tenor sax | C (105 samples) | P |
| Baritone sax | **P** tube conical −12 (gap) | R |
| Saxello | C | — |
| Cornet, euphonium, flugelhorn | see brass | — |
| Mellophone | F brass, between horn and trumpet (gap) | — |
| Sousaphone | V tuba, brighter EQ | — |
| Marching snare, tenors (quads), bass drums | V "VSCO 1 Percussion/drums" | D |
| Glockenspiel (bell lyre) | C | P modal 5 |
| Harmonica | C (3 instruments) | — |

### 4.7 Rock / jazz band

| Instrument | Primary | Notes |
|---|---|---|
| Drum kit | core/Acoustic Studio Kit (VCSL) | ride via C suspended cymbal, played with stick samples |
| Electric guitar clean / crunch / lead | P pluck → Overdrive → cab IR | shared with hyperpop/Power Chord Dist, voiced for rock |
| Acoustic guitar (steel, nylon) | P pluck, body guitar | strum tool |
| Bass guitar finger / pick / slap | P pluck | slap = exciter hammer + pop zone |
| Upright jazz bass | V contrabass pizz | — |
| Rhodes | F (alg 5, tine "bark" op ratio 14) + P modal 11 layer | → Tremolo stereo |
| Wurlitzer | P modal 12 → Overdrive light → Tremolo | shared with ambient |
| Hammond | A drawbars (888000000, 838000000, 886000000 registrations), key-click noise → Rotary | percussion via pitchEnv-free 2nd/3rd harmonic decay layer |
| Clavinet | C TX81Z Clavisynth or P pluck, bright, Filter env | — |
| Vibes (jazz) | C vibraphone + Tremolo motor | — |

**Total: ~95 instruments, ~160 presets** including per-articulation presets and section racks.

---

## 5. Templates and racks

- `Full Orchestra` template: all sections on the stage racks, routed to section
  buses (Strings, Woodwinds, Brass, Percussion, Keys) and a `Concert Hall`
  return. It doubles as `docs/examples/classical_demo.adx`.
- `Chamber Strings`, `String Quartet` (solos, physical bow for cello/viola gaps).
- `Concert Band`, `Marching Band`, `Big Band` (saxes, trumpets, trombones,
  rhythm section), `Rock Band`, `Jazz Trio`.
- Effect racks: `Stage Front/Mid/Back`, `Concert Hall`, `Cathedral` (Hall 6 s),
  `Studio Room`, `Orchestral Master` (gentle Compressor 1.5:1 → Limiter −1 dBTP,
  no clipping).

---

## 6. Tests

| Test | Asserts |
|---|---|
| `pack_classical_loads/golden/no_alloc` | as every pack; library-tagged |
| `classical_matrix_complete` | every row of §4 has a loadable preset, a fallback that loads with no libraries, and a §10 verdict |
| `keyswitch_convention` | every multi-articulation preset maps the same articulation to the same switch key |
| `range_coverage` | each preset sounds across its instrument's written range (no silent keys inside the range; keys outside it are silent) |
| `tuning_a440` | every sampled zone's measured pitch within ±8 cents of its key after `tune` (catches mis-mapped SFZ rows) |
| `dynamics_crossfade_smooth` | a CC1 sweep on a sustain shows no step > 0.5 dB |
| `release_triggers` | piano/harpsichord release zones fire |
| `seating_defaults` | the Full Orchestra template's pans match §3 |
| `pack_memory_budget` | each preset ≤ 1.5 GB resident; the Full Orchestra template ≤ 12 GB. Over budget → this phase adds disk streaming before closing (Phase 12 §7) |
| `licence_gate` | no Iowa MIS file in an installer component; Salamander credit present |
| `pack_classical_demo_renders` | as every pack |

---

## 7. Definition of done

- [ ] §6 passes.
- [ ] §10 verdict table complete: per instrument, which source won the A/B and why.
- [ ] **Human gate**, held section by section: strings, woodwinds, brass,
      percussion, keys/choir, band. The user plays each preset and approves,
      and the verdicts are recorded in their words. A rejected row is fixed or
      demoted to "(Synth) — known weak". (judgement)
- [ ] FINAL_PLAN §10, STATE.md, launcher check.

---

## 8. Out of scope

Legato transition samples (none of the libraries record them; the bow model
and `voice.mode=legato` approximate it) · auto-divisi · notation/score view ·
purchasing commercial libraries.

## 9. Risks

| Risk | Mitigation |
|---|---|
| CC0 libraries have few dynamics and round-robins | crossfade + humanize; rendered physical layers can add a dynamic where missing |
| RAM | `pack_memory_budget`; streaming only if needed |
| Iowa holdings differ from what this plan assumes | verified first in tranche A; any missing instrument falls back to the `P`/`F` column, which already exists for every row |
| Gap instruments (English horn, contrabassoon, bari sax, cornet, euphonium, mellophone, celesta, castanets, harmonium) sound synthetic | they are labelled honestly and documented as known weak until a CC0 source appears; the user may drop their own samples into a user preset |

## 10. Verdicts

*(instrument · candidates A/B'd · winner · notes · user's words)*

---

## 11. Sources

[VSCO 2 CE](https://versilian-studios.com/vsco-community/) · [VSCO 2 CE repository](https://github.com/sgossner/VSCO-2-CE) ·
[VCSL](https://github.com/sgossner/VCSL) · [VCSL site](https://versilian-studios.com/vcsl/) ·
[Iowa MIS](https://theremin.music.uiowa.edu/MIS.html) · [MIS announcement (Fritts, 2012)](https://auditory.org/postings/2012/569.html) ·
[Salamander Grand (sfz)](https://github.com/sfzinstruments/SalamanderGrandPiano) ·
[FM brass / Risset (CCRMA)](https://ccrma.stanford.edu/~jos/sasp/FM_Brass.html) ·
[Risset patent text](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/4018121) ·
[CCRMA percussion acoustics](https://ccrma.stanford.edu/courses/150-spring-2003/percussion.html) ·
[Timpani modelling (Purdue)](https://docs.lib.purdue.edu/dissertations/AAI8825522) ·
[Tubular bells (Hibberts)](https://www.hibberts.co.uk/tubular-bells/) ·
[Commuted synthesis, strings (CCRMA)](https://ccrma.stanford.edu/~jos/pmupd/Strings.html) ·
[DX7 brass algorithm 22 (Making Music, 1987)](https://www.muzines.co.uk/articles/technically-speaking/13195?theme=1)
