# Phase 19 — Pack: Phonk + Vocal (UTAU) · M

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `engine/preset/packs/phonk/` (Memphis, drift and Brazilian phonk; extends `stakillaz`) and `engine/preset/packs/vocal/` (Voice-instrument settings, vocal stacks and vocal chains); the default-voicebank preference |
| **FINAL_PLAN refs** | §5.3 (Voice), §5.9, §5.12, §7 Phase 19 |
| **Entry criteria** | [phase_18.md](phase_18.md) §7 complete |
| **Next** | none planned. The sound library's further growth is ordinary pack work under §5.12 |
| **Pack ids** | `phonk` · `REQUIRES=core, stakillaz` — `vocal` · `REQUIRES=core` |
| **Reference voicebank** | Kasane Teto, which is never in the repo (FINAL_PLAN §5.3) |

---

## 1. Objective

Two smaller packs that finish the library:

- **Phonk.** STAKILLAZ's world, widened to drift phonk's cowbell melodies,
  distorted gliding 808s, Memphis tape and vocal chops.
- **Vocal.** Make the Voice instrument (Phase 4 tranche D) as browsable as any
  synth: singing styles as presets, layered stacks, and chains for both Voice
  and recorded vocals. The presets hold *settings only*. A voicebank is never
  shipped, because its licence usually forbids redistribution.

---

## 2. Research summary

**Drift phonk:**

- The **cowbell is "the defining sound"**: the classic 808 cowbell, tuned
  across a scale, looping short 3–4-note motifs, distorted or saturated for
  grit.
- **808s are distorted, pitched down and "pushed until they're growling"**,
  usually as a clean sub plus a distorted mid layer, with portamento slides.
- Lo-fi texture (vinyl crackle, tape hiss, bitcrush) is used sparingly in drift
  phonk, more in classic phonk, along with sidechain pumping.
- Tempo estimates conflict (90–120 vs 140–160 BPM). Drift phonk is usually
  felt at ~130–160, Memphis at a ~65–80 half-time, so templates cover both.
- Kordhell-specific methods were not found.

**Memphis/classic phonk** (docs/STAKILLAZ.md): Memphis tape hiss, lo-fi,
minor-key cowbell melodies, chopped and repitched vocal samples, blown-out
distortion, pumping.

**Brazilian phonk ("funk")**: distorted 808 slides, chopped vocal chants,
tamborzão-style percussion. This is the user's genre territory but the web
research didn't cover it, so it is listening-session-driven (§3).

**Voice instrument parameters:**

| Parameter | Range | UTAU analogue |
|---|---|---|
| `gender` | −100..100 | the `g` flag |
| `breathiness` | 0..100 | `B` |
| `peakCompression` | 0..100 % | `P` |
| `tuning` | ±100 cents | — |
| `velocity` | 0..1 | consonant velocity |
| `vibrato.depth` / `vibrato.rate` / `vibrato.delay` | — | — |

Flag meanings depend on the resampler, and some flags "benefit one UTAU [and]
ruin another". So every Voice preset is tuned and accepted against **Teto on
the user's machine** and labelled as tuned for her.

---

## 3. Listening sessions

As Phase 14 §3: the user picks phonk references (classic Memphis, drift
phonk, Brazilian phonk, STAKILLAZ) and vocal references (hyperpop
hard-tuned vocals, vocaloid duets like Avanna's "Sad Machine", UTAU covers
they like).

---

## 4. Phonk catalogue (~24)

| Name | Recipe |
|---|---|
| Drift Cowbell | core/808 Cowbell keyTrack 1, decay 0.35 → Overdrive 16 dB tone 5 kHz → EQ +4 @ 2 kHz → Room. Played as a melody (C5 region) |
| Drift Cowbell Wide | Drift Cowbell ×2 layers ±8 c, panned ±30 → Hall 1.2 s |
| Memphis Cowbell | Drift Cowbell, softer drive, Tape age 0.5 hiss −40 dB, minor-key template |
| Cowbell Saturated Lead | Drift Cowbell → Clipper hard → Delay 1/8 dotted |
| Phonk 808 Growl | rack: core/Sub Sine (clean, LP 120 Hz) + core/808 Long Glide → Overdrive 28 dB → Clipper hard 8 dB → EQ HP 80 (the mid layer); glide 150 ms legato — "clean sub + distorted layer" |
| Phonk 808 Slide | Phonk 808 Growl, glide 250 ms |
| Brazilian 808 | Phonk 808 Growl, shorter, Clipper harder, pitchEnv +12 / 30 ms |
| Memphis Kick | core/808 Kick Long → Tape → Saturation |
| Phonk Snare | core/Trap Snare → Bitcrush 12 bit mix 0.3 → Tape |
| Phonk Hat Roll | core/Trap Hat + `trap_hats_rolls` |
| Phonk Kit | rack of the above, GM-mapped |
| Memphis Vocal Chop | rack chain for samples: PitchShifter −3 st → Bitcrush 11 bit mix 0.4 → Tape → Filter BP 300 Hz–4 kHz → Delay 1/4 |
| Chant Chop | Memphis Vocal Chop, tighter, Gate, for Brazilian chants |
| Dark Bell | fm bell (ratio 3.5) in minor, Tape → Hall |
| Phonk Piano Lo-Fi | classical/Upright → Tape cassette → Bitcrush 12 bit |
| Tamborzão Perc | core/808 Conga + Clave + Shaker rack, Brazilian template |
| Phonk Pump | core/Kick Pump 1/4, deeper |
| Memphis Tape Bus | Tape (wow 0.3, hiss −42 dB, age 0.6) → stakillaz/Memphis Tape |
| Blown Bus | stakillaz/Blown Master → Clipper soft |
| Drift Lead Screech | stakillaz/Screech Lead → Clipper → OTT |
| Phonk Riser | core/Noise Riser 8 → Tape |
| Phonk Impact | core/Impact → Distortion |
| Vinyl Bed | breakcore/Vinyl Crackle + Tape hiss |
| Yabujin Pad | ambient/Juno Pad → Bitcrush → Hall (yabujincore atmospheres) |

Templates: `drift_phonk_150.adx` (cowbell melody + growl 808), `memphis_70.adx`
(half-time, tape), `brazilian_phonk_130.adx`, `trashwave_150.adx` (STAKILLAZ-style,
from the stakillaz demo).

---

## 5. Vocal catalogue (~20)

### 5.1 Voice presets (settings only; the bank comes from the user's preference)

| Name | Settings |
|---|---|
| Natural | gender 0, breathiness 50, peakCompression 86, vibrato depth 25 c rate 5.5 Hz delay 0.3 s |
| Bright Pop | gender −15, breathiness 35, peakCompression 90, vibrato 15 c |
| Soft Whisper | breathiness 90, peakCompression 70, velocity 0.4 |
| Deep | gender +35, breathiness 45 |
| Child-like | gender −45, tuning +5 c |
| Robotic | vibrato 0, peakCompression 100, breathiness 20 (pairs with Hard Tune) |
| Belt | gender −5, breathiness 25, velocity 0.8, vibrato 40 c delayed 0.4 s |
| Ballad | breathiness 60, vibrato 30 c rate 5 Hz delay 0.5 s |

### 5.2 Stacks (racks of several Voice layers)

| Name | Recipe |
|---|---|
| Double Track | 2 × Natural, tuning ±6 c, offset 12 ms, pan ±25 |
| Choir of One | 4 layers, gender −20/−5/+10/+30, tuning ±8 c, offsets 0–25 ms → Ensemble → Hall 3 s |
| Octave Stack | lead + layer +12 st (gender −30) at −10 dB |
| Harmony Third | lead + layer through Phase 7's scale-degree transpose (+2 degrees) |

Voice renders are cached off the audio thread (Phase 4 §4.13). A 4-layer stack
is four cache entries, which is why stacks stay at ≤ 4 layers. That limit is
measured by `voice_stack_cache_budget`.

### 5.3 Vocal chains (for Voice *or* recorded audio)

`Hyperpop Hard Tune` (hyperpop/Hard Tune voiced for Voice: Clipper softer),
`Vocaloid Duet` (Phase-4 Voice + Formant shift −2 → Delay → Hall — the
"robot girl and human boy" duet idea), `Nightcore`, `Phonk Vocal`
(PitchShifter −5 st formant +3 → Tape → Bitcrush), `Radio`, `Airy Pop`
(Compressor → EQ air +4 dB → Hall plate), `Distorted Scream` (Clipper → Overdrive →
Filter BP), `Telephone`.

---

## 6. Deliverables beyond presets

- **Default voicebank preference** (`app/adx/preferences`): the folder of the
  user's UTAU bank. A Voice preset inserted with no bank uses it. With no
  preference set, the browser shows "needs a voicebank" and links to the
  setting. A preset never names a bank by path, so the pack is redistributable.
- `docs/vocal.md`: choosing and installing a bank, what each preset changes, and
  how the flags map to these parameters.

---

## 7. Tests

`pack_phonk_*`, `pack_vocal_*` (loads/golden/no_alloc/cpu/demo_renders). Vocal
goldens run with the **Teto reference bank locally** (`ctest -L voicebank`,
never in CI). CI renders them through the bank-less path and asserts the
diagnostic. Also:

| Test | Asserts |
|---|---|
| `cowbell_melody_tuning` | Drift Cowbell's two 808 partials track the key, ±5 cents, C4–C6 |
| `phonk_808_layers` | below 100 Hz the growl 808 is ≥ 90 % sub energy; 300 Hz–3 kHz carries the distortion |
| `voice_preset_no_bank` | inserting a Voice preset without a bank gives one diagnostic, no crash, silence flagged in the UI |
| `voice_stack_cache_budget` | Choir of One renders a 16-bar phrase within the Voice cache's memory budget |
| `stakillaz_pack_unchanged` | the original pack's hashes are untouched |

---

## 8. Definition of done

- [ ] §7 passes (voicebank-tagged tests locally with Teto).
- [ ] Listening log (§10). (judgement)
- [ ] `docs/examples/phonk_demo.adx` and `docs/examples/vocal_demo.adx` (the latter needs a bank to sound; it says so in a comment line).
- [ ] **Human gate:** the user approves the cowbells and 808s against their
      phonk references, and the Voice presets *with Teto*, in their words. (judgement)
- [ ] FINAL_PLAN §10 and STATE.md: the §5.12 sound library is complete. Launcher check.

## 9. Risks

| Risk | Mitigation |
|---|---|
| Voice presets tuned on Teto sound wrong on other banks | they are labelled "tuned for Teto"; macros expose gender/breathiness so other banks can be corrected quickly |
| Phonk's lo-fi elements make goldens noisy-looking | all noise is seeded (Phase 3 §4.10 condition 6), so hashes are stable |

## 10. Listening-session log

*(genre/artist · reference · element · parameters · verdict)*

## 11. Sources

[Melodics — producer's guide to phonk](https://melodics.com/blog/producers-guide-to-phonk-music) ·
[Native Instruments — phonk](https://blog.native-instruments.com/phonk-music/) ·
[Violet Recording — how to make phonk](https://violetrecording.com/how-to-make-phonk/) ·
[Output — phonk plugins](https://output.com/blog/best-plugins-for-phonk) ·
[Soundtrap — phonk](https://blog.soundtrap.com/?p=976) ·
[docs/STAKILLAZ.md](../docs/STAKILLAZ.md) ·
[UTAU (Wikipedia)](https://en.wikipedia.org/wiki/Utau) ·
[UTAU flags list (fan-made, bank-specific)](https://meadonroe.deviantart.com/art/List-of-UTAU-Flags-683232677) ·
[Porter Robinson — Avanna on "Sad Machine"](https://en.wikipedia.org/wiki/Porter_Robinson)
