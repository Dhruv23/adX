# Phase 14 — Pack: Hyperpop · L

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `engine/preset/packs/hyperpop/` |
| **FINAL_PLAN refs** | §5.9, §5.12, §7 Phase 14 |
| **Entry criteria** | [phase_13.md](phase_13.md) §6 complete; Phase 12 fully done (this pack needs `physical`, PitchCorrect, OTT, Wavetable warp) |
| **Next** | [phase_15.md](phase_15.md) |
| **Pack id** | `hyperpop` · `REQUIRES=core` |
| **Artist references** | Porter Robinson, Ninajirachi, Madeon, glaive, Tiffany Day, underscores (and the genre's roots: SOPHIE, A. G. Cook / PC Music, 100 gecs) |

---

## 1. Objective

A pack that makes the sound of hyperpop and its neighbours: maximal supersaws,
distorted gliding 808s, glassy plucks and bells, pitched and hard-tuned vocals,
General-MIDI nostalgia, house-filtered chords, emo/digicore guitars, and the
glitch FX that hold it together. Each of the six reference artists gets a
sub-folder of sounds that answer to what is documented about their work.

---

## 2. Research summary

What is documented, per artist (sources §9). Where little is written about an
artist's sound design, the plan says so rather than inventing a signal chain.
Those artists are covered by the listening sessions in §3.

| Artist | What is documented | What it means for the pack |
|---|---|---|
| **Porter Robinson** | Produces in FL Studio. Programs most basses in **Sytrus and 3xOSC**, and in a 2024 stream reportedly used 3xOsc "for practically every synth". *Worlds* drew on **General MIDI** sounds of PS1/N64-era consoles, "loud, melodic, at 90 bpm, cutesy, with vocaloid". Vocaloid **Avanna** sings "Sad Machine". *Nurture* pitch-shifts vocals (Manipulator). Sometimes uses u-he Diva. | A **GM-nostalgia** set (low-rate, slightly crunchy GM-style instruments); simple 3-oscillator saw/square/sine patches rather than wavetable complexity; FM basses (Sytrus is FM); a vocal-synth duet chain (Phase 19 Voice); pitch-shifted vocal chains. |
| **Madeon** | FL Studio ("objectively the most advanced DAW"). *Good Faith* layers a **chopped-up choir vocal** with shimmering synths and runs his own voice through effects "until it sounds almost like a synth"; gospel choirs over pitch-shifted samples. Uses Diva. | **Filtered house chord stabs** with sidechain, Diva-style warm ladder polys, choir-chop sampler racks, a **vocal-to-synth** chain (Vocoder/Formant/PitchCorrect). |
| **Ninajirachi** | Laptop-only, self-taught via tutorials, FL Studio early on. *I Love My Computer*: "sawtooth synths and sugary vocals … layered and chopped", gritty synths that "mimic a computer beginning to start up", "gears grinding". | **Layered/chopped saw leads**, complextro-style gritty FM/wavetable basses, "computer" glitch FX (startup, grind). |
| **glaive** | Nothing specific was found about their production. The genre (digicore) is described as "lots of layered plucky leads" and "metallic, clean and digital tones". They tour with Tiffany Day (2026). | Glassy plucks, digital bells, **guitar** (the emo side) from `physical` pluck, hard-tuned vocal chain. Listening session required. |
| **underscores** | Not found in the research. Known from listening to mix pop-punk/emo guitar with hyperpop electronics. | **Clean emo arpeggio guitar**, **distorted power-chord guitar** (Karplus–Strong + distortion: "full chords sound muddy … power chords" and palm mutes "chug"), bass guitar. Listening session required. |
| **Tiffany Day** | Shifted from bedroom/indie pop to hyperpop in 2025; *Halo* (2026) is "glitchy synths, aggressive bass and Y2K aesthetics". | Y2K trance plucks, aggressive distorted bass, glitch FX, airy pads. |

**Genre-wide** (tutorial consensus): 808s built from a saw over a sine,
low-passed near 1 kHz and overdriven; hard-tune with a fast retune plus formant
manipulation; drums "crushed with distortion"; supersaws from detuned unison
with constant filter and pitch movement; bright wavetables with heavy
modulation.

---

## 3. Listening sessions (the research the web cannot do)

For glaive, underscores, Tiffany Day and Ninajirachi, the documented material
is thin. Before tranche B, the user names **3 reference tracks per artist**,
and they stay on the user's machine and are never committed. For each sound
family, the agent and user:

1. loop the reference at the moment the sound is exposed;
2. run Phase 10's analyzers on it: spectrum, the timbre fit
   (`TimbreAnalyzer`, which "derives an additive patch from a reference mixdown
   using MIDI notes as ground truth"), key/BPM;
3. build the preset from §4's starting recipe and A/B it at matched loudness;
4. record in §10 of this file what was matched and what was not.

The extracted *parameters* are committed. The audio is not.

---

## 4. Catalogue (~64)

Engines: `va`, `wt` (Wavetable, Phase 12 shape), `fm`, `phys` (`physical`),
`smp`, rack = `.adxrack`. Macro slots follow the Phase 12 category table.

### 4.1 Supersaws and leads (~12)

| Name | Recipe |
|---|---|
| Hyper Saw | va: osc1 saw, osc2 saw +12 st level 0.35; unison 8, detune 32 cents, spread 0.9; filter LP 9 kHz res 0.1; env A 3 ms D 0.4 S 0.8 R 0.25 → OTT depth 0.45 → Clipper soft 4 dB → Hall 2.2 s mix 0.18. Macro 1 Brightness (cutoff 2–16 kHz), 2 Detune (12–60 c) |
| Gecs Saw | Hyper Saw + Overdrive 22 dB → Bitcrush 11 bit mix 0.2; mono legato, glide 40 ms |
| Worlds Saw Chords | va two saws ±9 c, unison 4, ladder LP 3.2 kHz, env A 15 ms S 1 R 0.6 → Ensemble II → VolumeShaper 1/4 (sidechain shape) → Hall 3 s. Built for 90 BPM chords |
| 3xOsc Square Lead | va: osc1 square, osc2 saw −12 st level 0.4, sub 0.3, no unison; vibrato via lfo1 sync 1/8 to pitch 0.06 delayed 0.3 s → Delay 3/16 fb 0.35 — the plain 3-oscillator lead the Porter research points to |
| Sad Machine Lead | 3xOsc Square Lead, filter LP 4 kHz, Hall 4 s mix 0.3 |
| Chopped Saw Lead | Hyper Saw → GrossBeat StutterEighth depth 0.6 → VolumeShaper 16-step gate (Ninajirachi "layered and chopped") |
| Y2K Trance Lead | wt hypersaw table, unison 7, detune 28 c, filter LP env +0.4 → Delay 1/8 dotted pingpong → Hall |
| Digital Bell Lead | fm: alg 5, op1 ratio 1, op2 ratio 3.5 (bell), op2 decay 0.6 s; mod1 velocity→op2 level 0.5 → Hall shimmer 0.25 |
| Squeak Lead | wt formant-vowels, mod1 lfo1 (sync 1/16) → position 0.7; pitchEnv +5 st 30 ms |
| Talkbox Lead | va saw + noise → Vocoder (carrier saw, keyed from a vocal insert) → Filter BP 1.2 kHz → Clipper |
| Detuned Pitch Lead | va saw, mod1 random-per-note → pitch 0.04 (sloppy), glide 30 ms |
| Laser Lead | wt, pitchEnv +24 st 0.08 s, warp sync 0.5 |

### 4.2 Plucks, bells, keys (~10)

`Glass Pluck` (wt digital table, env D 0.25 S 0, filter env +0.6 → Hall 2 s),
`Digicore Pluck` (va saw+square, A 1 ms D 0.18, filter LP env, OTT 0.5 — "layered plucky leads"),
`Music Box` (phys modal table 1 high, mallet hardness 0.9),
`Toy Bell` (fm alg 1 op ratio 1:7.1 fixed-ish),
`Celesta Chime` (phys modal 5 → Hall),
`Bubble` (va sine, pitchEnv −12 st 40 ms inverted curve — the SOPHIE "bubble"),
`Latex Perc` (phys modal table 13 with ratio overrides, pitchEnv +7 st 15 ms — metallic SOPHIE-style),
`Plucked Synth Harp` (phys pluck brightness 0.8 → Ensemble),
`Bit Keys` (va square → Bitcrush 6 bit 11 kHz),
`EP Shimmer` (fm alg 5 Rhodes-style op2 ratio 14 with fast decay for the tine "bark" → Tremolo stereo → Hall).

### 4.3 808s and basses (~9)

| Name | Recipe |
|---|---|
| Hyper 808 | core/808 Long Glide + layer va saw (−12 dB, LP 1 kHz) → Overdrive 24 dB → Clipper hard 6 dB → EQ HP 25 Hz. The saw-over-sine recipe |
| Gliding 808 | as above, glide 140 ms legato, pitchEnv +12 st 30 ms |
| Blown 808 | Hyper 808 → Distortion 18 → OTT 0.4 → Limiter |
| Sytrus FM Bass | fm alg 1, op1 ratio 1, op2 ratio 1 feedback 0.3, op2 decay 0.3 s; mod1 modEnv1 → op2 level |
| Complextro Grind | wt growl table, warp FM-from-osc2 0.6, Filter comb+ LFO sync 1/16 → Clipper → OTT 0.6 |
| Diva Mono Bass | va saw, ladder LP 600 Hz res 0.35, filter env 0.6, mono legato |
| Aggressive Y2K Bass | va square+saw, unison 3, drive → Overdrive → Filter LP follow 0.5 |
| Bass Guitar Finger | phys pluck, decay 2.5 s, damping 0.6, body none → Compressor → Saturation tape |
| Bass Guitar Pick | phys pluck, brightness 0.8, exciter hardness 0.9 → Overdrive 6 dB |

### 4.4 Guitars (~6)

| Name | Recipe |
|---|---|
| Emo Clean Arp | phys pluck, decay 3 s, brightness 0.65, pick position 0.2, body guitar 0.4 → Ensemble I → Delay 1/8 dotted → Hall 1.5 s |
| Power Chord Dist | phys pluck → Overdrive 30 dB tone 3 kHz tightness 150 → Convolution cab 4×12 → EQ (cut 400 Hz −3, boost 2.5 kHz +2). Power chords not full chords (muddy under distortion) |
| Palm Mute Chug | Power Chord Dist with `mute=1`; macro 1 Mute Amount |
| Shoegaze Wash | Power Chord Dist → Ensemble 6 voices → Hall 8 s shimmer 0.15 |
| Acoustic Strum | phys pluck, body guitar 0.8 + Phase 6 piano-roll strum tool |
| Lo-Fi Guitar | Emo Clean Arp → Tape wow 0.3 flutter 0.2 age 0.5 |

### 4.5 GM nostalgia set (~8)

Porter's *Worlds* palette. Each is the plain GM-style instrument rendered to a
22.05 kHz multisample (`render_multisample.py`), then played back with
`interpolation=0` for the console crunch:

`GM Square Lead`, `GM Calliope`, `GM Choir Aahs` (additive formant ah),
`GM Strings` (va saw ensemble), `GM Pan Flute` (phys tube, flute, breath 0.4),
`GM Celesta`, `GM Music Box`, `GM Synth Brass` (va saw + filter env).
Macro 1 Era (sample-rate 11–44 kHz via Bitcrush), 2 Crunch.

Phase 12 adds the `gm` instrument (§4.7.5 there), so the real thing is also
available: a `GM Bank` rack puts a `gm` channel (shipped bank, or the system
`gm.dls`) behind the same Era/Crunch macros. Use it to A/B against the rendered set
at the human gate. Whichever sounds closer to *Worlds* is the one that ships.

### 4.6 Pads and atmospheres (~5)

`Halo Pad` (wt formant table slow LFO → Ensemble → Hall 6 s), `Nurture Air Pad`
(va saw ±7 c, LP 1.8 kHz, A 1.2 s → Tape → Hall), `Computer Startup` (FM sweep
+ Resonator — Ninajirachi's "computer beginning to start up"), `Gear Grind`
(granular on Complextro Grind render, density 80), `Choir Chop Pad` (smp choir
chops → GrossBeat).

### 4.7 Vocal chains (~8, effect racks for recorded vocals or Phase 19 Voice)

| Name | Chain |
|---|---|
| Hard Tune | PitchCorrect retune 0 ms, scale from the project key → Compressor 4:1 → EQ air +3 dB @12 kHz → Clipper soft |
| Chipmunk | PitchShifter +7 st → FormantFilter shift −3 → Hard Tune |
| Nightcore Up | PitchShifter +4 st with formant follow; template note to raise tempo ×1.2 |
| Pitched Down Duet | PitchShifter −12 st, formant +4 → Hall (the "two voices" trick) |
| Vocal-to-Synth | Vocoder 32 bands carrier saw → Ensemble → Filter LP (Madeon's voice "almost like a synth") |
| Choir Stack | 4 sends: PitchShifter 0/+12/−12/+7, each Ensemble, summed → Hall 4 s |
| Glitch Vocal | GrossBeat StutterEighth → Bitcrush → Delay 1/32 |
| Phone Vocal | Filter BP 900 Hz–3 kHz → Clipper → Bitcrush 10 bit |

### 4.8 FX and drums (~6)

`Hyper Kit` (from core, retuned), `Crushed Snare` (core/Trap Snare → Clipper hard 12 dB → Bitcrush),
`Glitch Stutter` (GrossBeat + VolumeShaper 1/32), `Download Sweep` (Frequency
shifter sweep), `Error Blip` (fm short, pitchEnv), `Reverse Vocal Swell`
(granular reverse + Hall).

---

## 5. Templates

`hyperpop_160.adx` (160 BPM, Hyper 808, Hyper Saw, Hard Tune bus),
`worlds_90.adx` (90 BPM, Worlds chords, GM set), `madeon_house_122.adx`
(filtered chord stabs, choir chops), `digicore_145.adx` (guitar + plucks + 808).
These double as `docs/examples/hyperpop_demo.adx` material.

---

## 6. Tests

`pack_hyperpop_loads`, `pack_hyperpop_golden`, `pack_loudness_window`,
`pack_hyperpop_no_alloc`, `pack_hyperpop_cpu` (Hyper Saw 8-note chord, 8 unison
× 2 osc = 128 oscillators, < 20 % of a 256-frame block), `gm_set_rendered_deterministic`
(re-rendering the GM multisamples reproduces their hashes), `vocal_chain_latency`
(each vocal chain's PDC is reported and compensated, Phase 3), `pack_hyperpop_demo_renders`.

---

## 7. Definition of done

- [ ] §6 tests pass.
- [ ] Listening sessions (§3) held for every artist; §10 filled in with what matched. (judgement)
- [ ] `docs/examples/hyperpop_demo.adx` from this pack + core only.
- [ ] **Human gate:** the user plays each family (supersaws, 808s, guitars, GM set,
      vocal chains) against their own references and approves, recorded in their words.
      (judgement)
- [ ] FINAL_PLAN §10, STATE.md, launcher check.

---

## 8. Risks

| Risk | Mitigation |
|---|---|
| Physical guitar doesn't convince next to a real one | the guitar presets are judged against *hyperpop* mixes (heavily processed guitars), not solo guitar; failing that, VCSL has no guitar, so the fallback is rendered multisamples of the tuned model |
| PitchCorrect hard-tune artefacts differ from Auto-Tune's | match by ear in the listening session; the artefact *is* the sound, so tune `retune` and `humanize` until it matches |
| Artist-named presets imply endorsement | preset names describe sounds, never artists (`Sad Machine Lead` is a song-mood name, not "Porter Lead"); artists are named only in this plan and the pack's README as inspiration |

## 9. Sources

[Porter Robinson (Wikipedia)](https://en.wikipedia.org/wiki/Porter_Robinson) ·
[Equipboard — Porter Robinson](https://equipboard.com/porter-robinson) ·
[Madeon on FL Studio (MusicRadar)](https://www.musicradar.com/news/madeon-fl-studio-is-objectively-the-most-advanced-daw&lang=en) ·
[Good Faith review (34th Street)](https://www.34st.com/article/2019/12/madeon-good-faith-album-review-electronic-france-french-synth-vocals-music-electropop-edm) ·
[Ninajirachi (Mixmag)](https://mixmag.net/feature/ninajirachi-club-kid-edm-i-love-my-computer-hyperpop-album) ·
[Ninajirachi (MusicTech)](https://musictech.com/news/music/ninajirachi-still-doesnt-have-any-professional-music-gear/) ·
[Ninajirachi (Clash)](https://www.clashmusic.com/features/my-computer-is-an-endlessly-inspiring-prompt-ninajirachi-interviewed/) ·
[I Love My Computer review (Collegian)](https://collegian.com/?p=336742) ·
[Tiffany Day (Wikipedia)](https://en.wikipedia.org/wiki/Tiffany_Day_(musician)) ·
[Halo review (KMUW)](https://www.kmuw.org/music/2026-05-19/in-halo-tiffany-day-works-through-her-angst-with-the-help-of-synths-bass-and-glitches) ·
[Tiffany Day & glaive (FADER)](https://www.thefader.com/2026/10/13/tiffany-day-glaive-meant-to-be-video-bts-photos) ·
[digicore composition notes (Leeds Conservatoire)](https://show.leedsconservatoire.ac.uk/shr4c004m002-24101972-composition-production/) ·
[Hyperpop guides: eMastered](https://emastered.com/blog/how-to-make-hyperpop), [Soundtrap](https://blog.soundtrap.com/hyperpop-music/), [Native Instruments](https://blog.native-instruments.com/hyperpop/), [HyperPop Production Explained](https://www.musehub.com/course/g3-course-hyperpop-production-explained) ·
[Palm mute (Wikipedia)](https://en.wikipedia.org/wiki/Palm_mute) ·
[Karplus–Strong guitar with distortion (CCRMA)](https://ccrma.stanford.edu/~hugo/220a/project/)

---

## 10. Listening-session log

*(filled in during the phase: artist · reference · sound · what matched · what did not)*
