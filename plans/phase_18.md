# Phase 18 — Pack: Breakcore / Jungle / DnB · M

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `engine/preset/packs/breakcore/` — original breakbeats and their slicer kits, the break-import workflow, reese/neuro/sub basses, jungle textures, breakcore bus chains, odd-meter templates |
| **FINAL_PLAN refs** | §5.3 (slicer), §5.5 (stretch, onsets), §5.12, §7 Phase 18 |
| **Entry criteria** | [phase_17.md](phase_17.md) §7 complete |
| **Next** | [phase_19.md](phase_19.md) |
| **Pack id** | `breakcore` · `REQUIRES=core` (classical optional, for the orchestral-break hybrids) |
| **Artist references** | Venetian Snares, Machine Girl, Sewerslvt; jungle/DnB generally |

---

## 1. Objective

Make the breakbeat itself a first-class, playable instrument. The pack offers:

- **original** breaks, performed in adX on the acoustic kits and rendered, then
  sliced into Slicer presets;
- a one-click workflow for the user's own breaks (onset slicing, BPM-aware);
- the basses and textures of jungle, DnB and breakcore around them.

---

## 2. Research summary

- **Breakcore drums** are sliced breaks resampled over and over ("he often
  slices drum breaks and resamples them repeatedly in Renoise"), with the Amen
  break as a common starting point. Venetian Snares' signature is "meticulously
  complex drums … odd time signatures, in particular 7/4". The drums
  "shatter and skitter", in the drill-and-bass lineage (Squarepusher, Aphex
  Twin). Late-2000s Venetian Snares used a home-built modular drum synth with a
  Machinedrum as sequencer. *Rossz Csillag Alatt Született* combined breaks
  with orchestral samples.
- **Distortion** shows up as samples that become "more and more distorted",
  and as processing a channel and then slicing and transposing the result.
  Machine Girl's methods weren't found; the listening sessions cover them.
- **Reese:** two saws slightly detuned (about ±25 cents) whose phase
  cancellation moves. The beating speeds up as the pitch rises (a reese that
  doesn't change across the keyboard is just a stacked saw). Then a sine sub,
  a resonant lowpass, distortion, and mono legato with portamento. Unison with
  two voices is the same effect.

**The Amen break is not shipped.** It is a copyrighted recording, and the pack
must be redistributable. The pack ships **original performances** in the same
idioms, and the import workflow (§4.2) makes the user's own breaks one drag
away. Clearing those is the user's responsibility, as with any sample they add.

---

## 3. Listening sessions

As Phase 14 §3, for Venetian Snares, Machine Girl and Sewerslvt (user-chosen
references). Breaks are analysed for swing, ghost-note density and slice
patterns, and that analysis feeds §4.1's performances and §5's templates.

---

## 4. Catalogue (~46)

### 4.1 Original breaks (~10 break recordings, each → a Slicer preset + its source WAV)

Each break is an `.adx` performance in `packs/breakcore/breaks/src/`: the
core/Acoustic Studio Kit plus classical's room IRs, played with ghost notes,
humanize, swing and open/closed hat choke. It is rendered by `adx render` to
`lib://rendered/breakcore/` (deterministic, golden-hashed) and then sliced:

| Break | Feel | Tempo (source) |
|---|---|---|
| Think Funky | four-bar funk break, ghost-snare heavy, swung 16ths | 112 |
| Hot Pants Style | tight, dry, snare on 2 and 4 with flams | 105 |
| Ride Shuffle | ride-led jazz break | 140 |
| Gospel Crash | wide, roomy, crash accents (Amen-like *idiom*, original notes) | 136 |
| Tight Funk | 1-bar, very dry, for chopping | 100 |
| Rock Room | big room toms and kick | 95 |
| Brush Shuffle | brushes (VCSL snare swirls where available) | 90 |
| Machine Break | core/808 + 909 voices played as a break | 120 |
| Rope Snare Break | VCSL rope-tension snare, march-ish | 110 |
| Lo-Fi Vinyl Break | Think Funky → Tape → Bitcrush 12 bit → LP 9 kHz | 112 |

Slicer preset per break: one slice per onset (Phase 8 `OnsetDetector`), keys
from C3, `choke=1`, tempo-following on. Macros: 1 Pitch, 2 Decay, 3 Crush, 4 Room.

### 4.2 Break workflow (rack + tool)

- `Break Import` (`tools/packs/import_break.py`, also a browser drop target):
  WAV → BPM detect (Phase 8) → onset slices → Slicer preset with the pack's
  break chain and macros, written to the user preset folder. Optionally
  `--render-variants` renders pitched (−12…+12) and reversed variants as extra
  zones (the "resample repeatedly" loop).
- `Break Chopper` template: Slicer + a mini-notation pattern library of chops,
  e.g. `[0 1 [2 2] 3, ~ ~ 4? ~]`, rushes `0*8`, reverses, and
  `<[0 3] [5 7]>` alternations. Phase 7's mini-notation is the natural tool, and
  seeded probability keeps renders deterministic.

### 4.3 Basses (~10)

| Name | Recipe |
|---|---|
| Reese Classic | va saw + saw, osc2 fine +25 c / osc1 −25 c (as fine on both), sub 0.6, ladder LP 900 Hz res 0.3, mono legato glide 80 ms → Overdrive 8 dB |
| Reese Wide | Reese Classic, unison 2 each, spread 0.8 → Phaser 0.15 Hz → StereoImager lowWidth 0 (mono below 150 Hz) |
| Reese Dark | LP 400 Hz, Saturation tube |
| Neuro Bass | rack: L1 wt growl, warp FM-from-osc2 by modEnv1; L2 Reese Classic; L3 sub sine; chain Filter comb+ (LFO sync 1/16) → Clipper hard 12 dB → Multiband OTT 0.5 → EQ notch → Clipper soft. Macro 1 Growl (warp), 2 Comb Rate, 3 Drive |
| Neuro Stab | Neuro Bass, short env |
| Jungle Sub | va sine, pitchEnv +5 st / 40 ms, glide 120 ms legato — the rolling sub |
| Ragga Sub Slide | Jungle Sub, glide 300 ms |
| Hoover DnB | hardcore/Hoover (Juno A) −12 st, LP 1.5 kHz |
| 808 Roller | core/808 Distorted, 1/16 roll template |
| Wobble Reese | Reese Classic, lfo1 sync 1/8 → cutoff 0.6 |

### 4.4 Drums and FX (~12)

`Jungle Snare` (core/Acoustic Snare tuned +4 st, short), `Ghost Hat` (Acoustic Hat, low velocity layer only),
`DnB Kick` (ds char 1, tune +3, decay 0.18, click 0.8), `Rimshot Snap`, `Ride Bell`,
`Time-Stretch Smear` (granular over a break render, size 60 ms, scan 0.05 — the
grainy stretched-break sound), `Tape Stop Break` (GrossBeat), `Reverse Break Swell`,
`Dub Siren` (va square, lfo1 square 6 Hz → pitch 0.3, Delay 3/16 fb 0.8 → Hall),
`Rave Stab` (hardcore/Rave Organ Stab), `Amen-Free Crash` (core/Crash with long decay), `Vinyl Crackle` (noise impulses → Filter BP).

### 4.5 Pads and textures (~6)

`Jungle Pad` (classical strings soft → Filter LP 2 kHz → Hall 5 s),
`Atmospheric Breakcore Pad` (ambient/Juno Pad → Tape → Hall shimmer — the Sewerslvt
side), `Pitched Vocal Chop` (sampler chop rack + PitchShifter ±12, user vocals),
`Orchestral Hit` (classical brass + strings stac stack + Hall), `Choir Shard`
(classical/Choir → GrossBeat stutter), `Glass Shatter` (ambient/Glass Harmonica → Clipper → Hall).

### 4.6 Chains (~6)

`Break Bus` (Compressor 6:1 fast → Saturation tape → Clipper soft — fattened breaks),
`Breakcore Destroy` (Clipper hard 18 dB → Bitcrush 8 bit mix 0.4 → GrossBeat stutter → Limiter),
`Jungle Room` (Room 0.8 s, short predelay), `Rolling Sidechain` (VolumeShaper 1/8),
`Neuro Bus` (OTT 0.4 → Clipper → EQ), `DnB Master` (Compressor 2:1 → Clipper soft → Limiter).

---

## 5. Templates

`jungle_165.adx`, `dnb_174.adx`, `neuro_174.adx`, `breakcore_7_4_180.adx`
(7/4 via Phase 2's tempo/time-signature map), `atmospheric_breakcore_170.adx`,
`orchestral_breakcore.adx` (breaks + classical strings — the *Rossz Csillag* idea).

---

## 6. Tests

`pack_breakcore_loads/golden/no_alloc/cpu/demo_renders`, plus:

| Test | Asserts |
|---|---|
| `breaks_render_deterministic` | each break `.adx` renders to its committed hash in all three configurations |
| `break_slices_on_onsets` | every slice starts within 2 ms of a detected onset; no slice shorter than 20 ms |
| `slicer_tempo_follow` | a break at 112 BPM played in a 174 BPM project stays aligned over 8 bars |
| `import_break_roundtrip` | importing a rendered break reproduces the shipped Slicer preset's slice points |
| `reese_beating_tracks_pitch` | the measured beat rate at C3 < at C4 < at C5 (the "proper reese" test) |
| `odd_meter_template` | the 7/4 template's bar lines fall at 7 beats |
| `no_third_party_breaks` | `licence_report.py` finds only adX-rendered or CC0 audio in the pack |

---

## 7. Definition of done

- [ ] §6 passes; listening log (§9) filled. (judgement)
- [ ] `docs/examples/breakcore_demo.adx`.
- [ ] **Human gate:** the user approves the breaks (do they *chop* well?), the
      reese/neuro basses and the chains, in their words. (judgement)
- [ ] FINAL_PLAN §10, STATE.md, launcher check.

## 8. Risks

| Risk | Mitigation |
|---|---|
| Original breaks lack the magic of classic recordings | humanize, room IRs and the user's gate; the import workflow means the user's own breaks are always one drop away |
| Slicer CPU with heavy chops at 180+ BPM | Slicer voices are one-shots; `pack_breakcore_cpu` covers a 1/32 chop pattern |

## 9. Listening-session log

*(artist · reference · element · parameters · verdict)*

## 10. Sources

[Breakcore (Wikipedia)](https://en.wikipedia.org/wiki/Breakcore) · [Venetian Snares (Wikipedia)](https://en.wikipedia.org/wiki/Venetian_Snares) ·
[Meathole review (Sputnikmusic)](https://www.sputnikmusic.com/album/10721/Venetian-Snares-Meathole) ·
[Making a breakcore track (UAL blog)](https://25031938.myblog.arts.ac.uk/?p=41) ·
[Venetian Snares (Exclaim)](https://www.pressreader.com/canada/exclaim/20160401/281625304413023) ·
[PopMatters — Cavalcade of Glee](https://www.popmatters.com/venetian-snares-cavalcade-of-glee-and-dadaist-happy-hardcore-pom-poms-2495700412.html) ·
[Reese: SoundBridge](https://www.soundbridge.io/design-a-reese-bass-from-scratch), [Future Proof](https://futureproofmusicschool.com/blog/reese-bass-sound-design-everything-you-need-to-know), [Avid](https://www.avid.com/resource-center/how-to-make-a-reese-bass), [RouteNote](https://create.routenote.com/blog/how-to-make-a-reese-bass/), [dnbforum](https://dnbforum.com/threads/how-is-the-reese-effect-done.185969/)
