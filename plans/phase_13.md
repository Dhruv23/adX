# Phase 13 — Pack: Core Drums & Machines · M

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `engine/preset/packs/core/` — drum machines, acoustic kits, 808 basses, transitions FX, bus chains, drum pattern templates |
| **FINAL_PLAN refs** | §5.9, §5.12, §7 Phase 13 |
| **Entry criteria** | [phase_12.md](phase_12.md) §8 handoff items done (racks, macros, DrumSynth models, Clipper, Filter, VolumeShaper, Tape, audition, VCSL fetched) |
| **Next** | [phase_14.md](phase_14.md) |
| **Pack id** | `core` — every later pack lists `REQUIRES=core` |

---

## 1. Objective

Every pack after this one needs drums, transitions and an 808. Building them
once, here, means Hyperpop, Hardcore, Breakcore and Phonk reference
`core/...` instead of each growing its own slightly different 808 snare.

This is also the cheapest pack, and so the right one to prove Phase 12's
tooling end to end: rack authoring, audition, the loudness window, golden
hashes, `lib://` samples with fallbacks, and the human gate.

---

## 2. Research summary

**TR-808** (synthesis is the 808, so these are the recipes):

- **Kick:** a bridged-T network rung by the trigger. It decays on its own with no
  VCA, and its pitch falls slightly over a long decay (Computer Music, *The
  808's bridged T network*). In adX: DrumSynth `model=0, character=0`, small
  `pitchDrop` (3–7 st) over a long `pitchTime`, and `decay` 0.3–2.5 s for the
  "808 bass".
- **Snare:** two bridged-T oscillators plus high-passed noise, with a "snappy"
  contour (Sound On Sound, *Practical snare synthesis*). A Japanese reference
  lists about 238/476 Hz for the two oscillators. The units differ, so `tune`
  covers it.
- **Clap:** noise through a band-pass, with a repeating sawtooth envelope for
  the multiple bursts and a longer envelope for the "reverb" tail.
- **Hats, cymbal, cowbell:** one bank of six square oscillators (205.3, 304.4,
  369.6, 522.7, 540, 800 Hz). Oscillators 5–6 are the cowbell. The hats and
  cymbal band-pass and high-pass the sum, with separate high and body envelopes
  (Werner/Abel/Smith; Misfit Audio's primer). `DrumSynth.cpp` already holds
  `kMetal`; Phase 12 added Cowbell/Cymbal.

**TR-909:** analog kick (sine with a fast pitch sweep plus a click), analog
snare and toms, sampled hats and cymbals. In adX this is `character=1`. A
genuinely sampled 909-style hat is synthesized here (Metal bank with a shorter,
brighter body) because no CC0 909 recording is in the chosen libraries.

**Acoustic kits:** VCSL ships CC0 bass drums, five snares (three modern, one
rope-tension, one legacy set), toms, hi-hat, suspended/clash cymbals,
tambourines, shakers, woodblock, agogo, cowbells, claves, triangles, bongos,
congas, cajón and more (tree inventory, 2026-10-25). VSCO 2 CE adds marching
snare/tenor/bass drums.

**Transitions:** risers are filtered noise and pitched sweeps; impacts are a sub
drop with a noise burst into a long reverb. Every guide in the Hardcore and
Hyperpop research uses these between sections, so they live here.

---

## 3. Deliverables

```
engine/preset/packs/core/
  pack.adxpack                 id core, libraries: vcsl, vsco2ce (optional)
  libraries.sha256
  drums/*.adxpreset            single voices (§4.1)
  kits/*.adxrack               multi-layer kits, one layer per GM key (§4.2)
  bass/*.adxpreset|.adxrack    the 808 family (§4.3)
  fx/*.adxrack                 risers, impacts, sweeps (§4.4)
  chains/*.adxrack             effect-only racks (§4.5)
  patterns/*.adx               drum pattern fragments, mini-notation (§4.6)
docs/examples/core_demo.adx    a 32-bar track built only from this pack
tests/golden/instruments.txt   + one hash per preset/rack
```

---

## 4. Catalogue

Notation: `ds` DrumSynth, `va` VA, `smp` sampler (`lib://`), `→` insert chain.
Values are starting points. Each preset is tuned by ear against the audition
reel, and the shipped values are what the human gate approved. Fallbacks for
`smp` presets are in brackets.

### 4.1 Drum voices (~38)

| Name | Engine | Recipe |
|---|---|---|
| 808 Kick Short | ds | model 0, char 0, tune −5, decay 0.35, pitchDrop 5, pitchTime 0.06, click 0.2 |
| 808 Kick Long | ds | as above, decay 1.6, keyTrack 1 (doubles as bass) |
| 909 Kick | ds | char 1, pitchDrop 24, pitchTime 0.025, click 0.7, decay 0.45, drive 2 |
| 909 Kick Hard | rack | 909 Kick → Clipper soft 9 dB → EQ low-shelf +3 @60 |
| House Kick | ds | char 1, decay 0.3, tone 0.6, sub 0.3 |
| Techno Rumble Kick | rack | 909 Kick + send → Hall 2.5 s → Filter LP 180 Hz → VolumeShaper 1/4 duck (the "rumble") |
| Trap Kick | ds | char 0, decay 0.5, click 0.6, drive 3 |
| Acoustic Kick | smp | VCSL Bass Drum 2, 3 velocity layers, xfade [909 Kick] |
| 808 Snare | ds | model 1, char 0, tune 0, tone 0.45, noise 0.6, decay 0.18 |
| 909 Snare | ds | char 1, tone 0.6, noise 0.75, decay 0.22 |
| Trap Snare | rack | 909 Snare + 808 Clap layer, → Clipper soft 4 dB |
| Rimshot 808 | ds | model 8 |
| Acoustic Snare | smp | VCSL Snare Modern 1, RR + 3 velocities [909 Snare] |
| Rope Snare | smp | VCSL Snare Rope Tension [808 Snare] |
| Piccolo Snare | smp | VCSL Snare Modern 3, tune +300 cents [909 Snare] |
| Marching Snare | smp | VSCO "VSCO 1 Percussion/drums/snare" [909 Snare] |
| 808 Clap | ds | model 3, char 0, decay 0.3 |
| Stacked Clap | rack | 808 Clap + VCSL Claps (smp) + Snap, → Hall 0.6 s mix 0.15 |
| Snap | ds | model 3, decay 0.05, tone 0.9 |
| 808 Closed Hat | ds | model 2, decay 0.05, tone 0.7 |
| 808 Open Hat | ds | model 2, decay 0.45; choke group with Closed Hat |
| 909 Hat | ds | model 2, char 1, decay 0.06, tone 0.9 |
| Trap Hat | ds | model 2, decay 0.035, tone 0.85 — tight enough for 1/32 rolls |
| Acoustic Hat | smp | VCSL Hi-Hat Cymbal, choke open/closed/pedal [808 Closed Hat] |
| 808 Cymbal | ds | model 7, decay 2.5 |
| Crash | smp | VCSL Clash Cymbals 1 [808 Cymbal] |
| Suspended Cymbal | smp | VCSL Suspended Cymbal 1 (incl. rolls) [808 Cymbal] |
| Reverse Crash | smp | Crash, reversed via Phase 8 destructive edit, rendered to `lib://rendered/core` [808 Cymbal → Shaper reverse-env] |
| 808 Cowbell | ds | model 6, char 0, decay 0.25, keyTrack 1 — playable melodically (phonk) |
| 808 Clave | ds | model 9 |
| Shaker | ds | model 10, tone 0.75 |
| Tambourine | smp | VCSL Tambourine 1 [Shaker] |
| 808 Tom Low/Mid/High | ds | model 4, tune −7 / 0 / +5, decay 0.5 |
| 808 Conga Low/Mid/High | ds | model 11 |
| Woodblock | smp | VCSL Woodblock [808 Clave] |
| Agogo | smp | VCSL Agogo Bells [808 Cowbell, tune +12] |
| Triangle | smp | VCSL Triangles [physical modal table 1, high] |

### 4.2 Kits (~8, racks, GM-mapped: 36 kick, 38 snare, 39 clap, 42/46 hats, 49 crash …)

`808 Kit`, `909 Kit`, `707 Machine Kit` (char 2), `Trap Kit`, `Lo-Fi Kit`
(808 Kit → Tape age 0.6, hiss −42 dB → Bitcrush 12 bit mix 0.3), `Acoustic Studio Kit`
(VCSL, with choke and round-robin), `Marching Kit` (VSCO snare/tenors/bass),
`Hyper Kit` (909 voices → Clipper hard 10 dB → OTT 0.3, the base for Phase 14).

Each kit rack has its own drum bus chain (`Drum Bus` below) and macros 1 Tune,
2 Decay, 3 Drive, 4 Room, 5 Hat Length, 6 Snare Tone, 7 Kick Level, 8 Width.

### 4.3 808 basses (~6)

| Name | Recipe |
|---|---|
| 808 Clean | va: osc1 sine, osc2 off, env A 2 ms D 1.8 s S 0 R 0.2, pitchEnv +7 st / 0.05 s, voice.mode legato, glide 0.06 |
| 808 Long Glide | as Clean, S 0.7 (holds), glide 0.12, glide.mode legato |
| 808 Distorted | 808 Clean → Overdrive 20 dB tone 3.5 k → Clipper soft 6 dB → EQ HP 25 Hz |
| 808 Punchy | ds model 0 keyTrack 1 decay 1.2, click 0.5 → Saturation tape 6 dB |
| 808 Reese | 808 Clean + layer va saw ±12 cents, Filter LP 400 Hz, gain −10 dB |
| Sub Sine | va sine, env A 5 ms S 1 R 0.08, no pitch env — the clean sub every pack layers under its bass |

Macros: 1 Glide, 2 Decay, 3 Drive, 4 Punch (pitchEnv amount), 5 Tone, 8 Sub Level.

### 4.4 Transition FX (~12)

| Name | Recipe |
|---|---|
| Noise Riser 8 | va noise 1 → Filter HP, cutoff macro-automated 200 Hz → 8 kHz over 8 bars via the shipped pattern's automation clip; → Hall 3 s |
| Noise Riser 16 | as above, 16 bars |
| Pitch Riser | va saw unison 8, pitchEnv −24 st over 8 bars (curve exponential(−0.4)), Filter BP follow |
| Downlifter | noise → Filter LP 8 kHz → 200 Hz, 2 bars |
| Sub Drop | va sine, pitchEnv +0 → −24 st over 1.5 s (`pitchEnv.amount=-24` with inverted curve) |
| Impact | rack: Sub Drop + noise burst (ds Shaker decay 0.4) + Crash, → Hall 6 s mix 0.35 |
| Reverse Swell | Reverse Crash → Hall shimmer 0.2 |
| Uplifter | noise → Resonator 3 voices (root, 5th, octave) feedback 0.85 → Filter HP sweep |
| Laser Zap | va saw, pitchEnv +36 st / 0.12 s, → Delay 1/16 dotted fb 0.4 |
| Tape Stop | chain: GrossBeat pattern TapeStop |
| Snare Roll Builder | Trap Snare + the `roll_build.adx` pattern (1/4 → 1/32 over 8 bars, velocity ramp) |
| White Noise Wash | noise → Filter BP slow LFO → Hall 8 s, the breakdown bed |

### 4.5 Chains (~8, effect-only racks)

`Drum Bus` (Compressor 4:1 att 10 ms rel 80 ms → Saturation tape 4 dB → Clipper soft ceiling −1),
`Parallel Crush` (send → Distortion 12 → Compressor 10:1 → EQ HP 100, return −8 dB),
`Kick Pump 1/4` (VolumeShaper, 1/4, depth 0.8, curve exponential(0.4)),
`Kick Pump 1/8`, `Lo-Fi Bus` (Tape + Bitcrush), `Room 0.6 s` (Hall small), `Glue Master`
(Compressor 2:1 slow → Clipper soft → Limiter −0.3 dBTP), `Trance Gate 16` (VolumeShaper steps).

### 4.6 Pattern templates (~16, `.adx` fragments using mini-notation)

`four_on_floor`, `house_offbeat_hats`, `trap_hats_rolls` (`hh*8 hh*16?` with seeded
probability), `boom_bap`, `breakbeat_funky`, `half_time`, `dnb_two_step`,
`roll_build`, `gabber_pump`, `phonk_cowbell` (scale-degree melody on 808 Cowbell),
`jersey_club`, `uk_garage_2step`, `amapiano_log` (808 Conga), `hardstyle_kick_bass`,
`happy_hardcore`, `breakcore_7_4`. Patterns are text, placed through the browser
like any `.adx` clipboard fragment (Phase 6's clipboard is `.adx` text).

---

## 5. Tests

| Test | Asserts |
|---|---|
| `pack_core_loads` | every file loads with zero diagnostics |
| `pack_core_golden` | one hash per preset/rack, all three configurations |
| `pack_loudness_window` | every audition within ±2 LU of its category target |
| `pack_core_fallbacks` | with no library store, every `smp` preset plays its fallback and raises exactly one diagnostic |
| `pack_core_kits_gm_map` | each kit's layers sit on the documented GM keys, choke groups correct |
| `pack_core_no_alloc` | every preset/rack, 30 s under the allocator hook |
| `pack_core_cpu` | the heaviest kit, 16th-note pattern at 174 BPM, < 10 % of a 256-frame block |
| `pack_core_demo_renders` | `core_demo.adx` renders offline bit-identically to realtime capture |
| `pattern_templates_parse` | every pattern fragment parses and pastes as one undo step |

---

## 6. Definition of done

- [ ] All §5 tests pass (library-tagged ones locally and in the full check).
- [ ] `docs/examples/core_demo.adx` exists, built only from `core`.
- [ ] **Human gate:** the user auditions every category in the browser, A/Bs the
      808/909 kits against references of their choice, and approves. Rejected
      presets are retuned or cut. The verdict is recorded in their words, per category.
      (judgement)
- [ ] FINAL_PLAN §10, STATE.md, launcher check.

---

## 7. Risks

| Risk | Mitigation |
|---|---|
| "808" means a dozen different machines' worth of expectations | `tune`/`character` macros, and the human gate picks the defaults |
| VCSL kit pieces vary in level and noise floor | the SFZ import report flags level outliers. Trim/normalise via Phase 8 destructive ops into `lib://rendered/core` when needed (documented per file) |
| Pattern templates feel generic | they are starting points; the generators (Phase 10 `RiffGenerator`) can vary them with a seed |

## 8. Sources

[Sound On Sound — practical snare synthesis](https://www.soundonsound.com/techniques/practical-snare-drum-synthesis) ·
[TR-808 bridged-T (Computer Music)](https://www.pressreader.com/australia/computer-music/20190220/282381220809560) ·
[Misfit Audio 808 cymbal primer](https://docs.cherryaudio.com/misfit-audio/transistor-808-cymbal) ·
[Werner/Abel/Smith, TR-808 cymbal](https://speech.di.uoa.gr/ICMC-SMC-2014/images/VOL_2/1453.pdf) ·
[sequencer.de — handclaps](https://www.sequencer.de/synthesizer/threads/jomox-mod-brane-11-kann-das-claps.97691/) ·
[TR-808 reference (scrapbox)](https://scrapbox.io/0b5vr/TR-808) ·
[VCSL](https://github.com/sgossner/VCSL)
