# Phase 15 — Pack: Hardcore · L

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `engine/preset/packs/hardcore/` — kicks of every hard-dance style, the Kick Lab racks, leads, screeches, hoovers, pianos, basslines, FX, bus and master chains |
| **FINAL_PLAN refs** | §5.9 (extends the STAKILLAZ suite), §5.12, §7 Phase 15 |
| **Entry criteria** | [phase_14.md](phase_14.md) §7 complete (the euphoric leads reuse its supersaw work) |
| **Next** | [phase_16.md](phase_16.md) |
| **Pack id** | `hardcore` · `REQUIRES=core` (and `stakillaz` for the trashwave crossover presets) |
| **Styles** | hardstyle (euphoric, raw, early/"tok"), hard dance, happy hardcore / UK hardcore, hardtekk, jumpstyle, frenchcore, gabber/uptempo, internet "white girl" hardstyle remixes |
| **Artist references** | STAKILLAZ, Romance Planet, prodArvee, ANGUISH, Jiyagi, REMZCORE, S3RL, PAYWALL, Tatsunoshin |

---

## 1. Objective

In hardstyle, "the kick is the whole instrument". This pack puts most of its
effort there: a kick for every style, built the way producers build them. That
means layered synthesis, alternating EQ and distortion stages, and a resample
loop tuned per note, all automated by Phase 12's `kick_lab.py`. Around the kicks
it supplies the leads and textures each style needs: euphoric supersaws, raw
screeches, hoovers, happy-hardcore pianos, tekk and jumpstyle basslines,
frenchcore and gabber leads.

---

## 2. Research summary

### 2.1 Kick anatomy (consensus across the sources in §10)

| Layer | Band | Built from |
|---|---|---|
| Sub/body | 30–80 Hz | sine at E1–G1 (≈ 41–49 Hz) |
| Punch | 80–200 Hz | the same sine's pitch sweep, or a separate punch layer; a 909 is the classic seed |
| Click/"tok" | 1–8 kHz | noise burst 1–5 ms HP 1–2 kHz, *or* a 2–5 kHz sine/triangle with a fast downward sweep, 5–10 ms (sources differ: 3–6 ms vs 5–10 ms) |
| Tail | the pitched body after the punch, 200–400 ms | the distorted pitch envelope playing out longer, tuned to the track's root per note |
| Top | — | the harmonics the distortion stages create |

**Pitch envelope:** the sine starts at about 150–300 Hz and drops to the
fundamental in 20–50 ms. 10–20 ms is tight and techno-like, 30–50 ms is
"classic hardstyle punch", and 50–100 ms is boomy. A techno source instead
starts 2–3 octaves above the root and drops in 20–40 ms. The numbers conflict,
so they are starting points and the human gate decides.

**Distortion chain:** alternate EQ and distortion, "each EQ shapes how the
next distortion acts". Before each stage, low-cut at 20–40 Hz and boost
500–1000 Hz (+8 to +9 dB at ~700 Hz in one drill). Another tutorial cuts
~50 and ~170 Hz and boosts ~383 Hz. Notch the resonances each stage creates. Go
aggressive first, then gentler. Hard clipping (odd harmonics) is rawstyle's
tool. Then **render, reprocess and transpose per note, possibly more than
once**.

**Tail tricks:** cut the tail, reverse it onto its own track, high-pass and
widen it, and process the attack separately (bitcrush and distort it), then sum
the parts on a bus with glue compression and more distortion.

**Styles:**

- *Euphoric:* cleaner, tonal kick, more sub, smoother tail, around 150 BPM.
- *Raw:* grittier, mid-heavy (harmonics over fundamental), longer, 155–160 BPM.
- *Early/"tok":* bouncier, minimal, built around the **reverse bass**. That is
  the reshaped kick tail, or a saw with a fast plucky envelope, boosted at
  500–1000 Hz, low-cut, clipped plus phase-distorted, slightly wide, placed only
  on off-beats.
- *Hardcore/uptempo:* extreme distortion and a very short tail.

**Leads:**

- *Euphoric supersaw:* 7–8 unison voices at 30–40 % detune, distortion and
  compression, an octave-up layer for air, and a cut below ~120 Hz. One recipe
  is two unison saws plus an LFO at 1/8 on fine-tune (~35 %), a short ENV on
  pitch, and a high-passed white-noise layer. That is three layers, each
  compressed and distorted, kept monophonic, into sidechained 100 %-wet
  reverb/delay sends.
- *Rawstyle screech:* the same detuned saws with aggressive pitch and filter
  automation and heavy distortion and waveshaping. One Serum recipe uses an
  LFO at **~20 Hz, unsynced, into coarse pitch**. Formant or comb filtering
  supplies the vocal "scream".

**Hoover (Alpha Juno "What the"):** three PWM saws an octave apart, a fast LFO
on pulse width, a **downward pitch envelope at note start**, resonance, and
**heavy 4-voice chorus**. Several people credit the chorus with most of the
body. The PWM-saw mechanism is disputed (saw × inverted pulse vs two
counter-detuned saws), so Phase 12 generates both as wavetables and the gate
picks one. The Juno's multi-stage envelope matters, and Phase 12's
pitchEnv + modEnv approximate it.

**Happy / UK hardcore:** 160–190 BPM (prompts cluster at 170–175). Piano riffs
(a poor piano "can make your track sound cheap"), synthetic stabs, hoovers
layered with a sine for weight, a punchy distorted kick.

**Hardtekk / tekno:** 150–185 BPM, defined by basslines more than vocals,
with less clipping on the kick than mainstyle hardcore. A UK approach uses a
higher, punchier DnB-style kick to leave room for more sub in a Serum bass,
distorted with an amp. The bass ducks to the kick.

**Jumpstyle:** 140–150 BPM, a 909-style four-on-the-floor kick, distorted
basslines and square-wave synths, "clarity and bounce over speed".

**Frenchcore:** REMZCORE started in the Brutal Jesters duo and has worked solo
since 2016, influenced by early hardcore and gabber. Frenchcore kicks are sold
as separate **punch, tail and resonance** samples, processed with waveshaper,
EQ and multiband compression (Septum's tutorial). Distorted and ring-modulated
FX kicks are a staple.

**Artists** (documented facts only; listening sessions fill the rest, §3):

- *STAKILLAZ*: trashwave/phonk/jumpstyle/hardstyle/yabujincore (docs/STAKILLAZ.md).
- *Romance Planet*: Miami artist mixing hyperpop, hardstyle, trance and
  pop-punk; "FALL FROM THE SKY PT. 2" is with STAKILLAZ.
- *Tatsunoshin*: Osaka; hardstyle/hardcore fusion, UK hardcore kicks,
  melodic and vocal-driven; released "Dead Love" on OneSeventy.
- *S3RL*: Australian happy hardcore. Nothing specific to their production was
  found.
- *REMZCORE*: French frenchcore/gabber.
- *prodArvee, ANGUISH, Jiyagi, PAYWALL*: **the web turned up nothing usable**.
  They are covered only through listening sessions.

"White girl hardstyle" is a playlist and TikTok category of hardstyle remixes
of pop and viral songs ("gymcore"). In practice it means a euphoric-leaning
kick, supersaws and pitched-up pop vocals at ~150–160 BPM, so it is served by
the euphoric and vocal presets rather than a separate engine.

---

## 3. Listening sessions

As Phase 14 §3. The user names 2–3 reference tracks for each of the nine
artists. For every kick style, the reference kick is isolated at a drop
(loop a bar where it plays alone, or as nearly alone as the track allows), then:

- it is analysed (spectrum over time, the fundamental's pitch trajectory with
  Phase 10's tools, tail length, the click's band);
- a rack is built from §4.1, and Kick Lab renders it across the reference's key;
- the two are A/B'd at matched LUFS;
- the parameters are logged in §11.

Reference audio stays on the user's machine.

---

## 4. Catalogue (~72)

### 4.1 Kick racks (~18)

Every kick is a `.adxrack` with up to four layers (**click**, **punch**,
**tail**, **reverse tail**) into a staged chain. Each is shipped twice: once as
the live rack (fully editable, CPU-heavy) and once as its **Kick Lab sampler
preset**. The sampler version is rendered per semitone from E1 to E2 through
`kick_lab.py`, and its tail is tuned per key (`<name> (Lab)`).

Shared skeleton, from §2.1:

```
layers:  click  ds model 0 click 1, decay 0.01   OR  va sine 3 kHz pitchEnv -24 st 6 ms, HP 1.5 kHz
         punch  ds model 0 char 1, pitchDrop 30, pitchTime 0.035, decay 0.12
         tail   va sine (or tri/saw per style), keyTrack, pitchEnv +A st / T s, env decay D
chain:   EQ1 (HP 30, +9 dB @ 700 Hz, cut 170)  -> Clipper (stage 1, aggressive)
         EQ2 (notch stage-1 resonance, +6 @ 500-1k) -> Overdrive or Clipper (stage 2)
         EQ3 (notch, tilt)  -> Multiband OTT (light)  -> Clipper soft (glue)  -> Limiter
macros:  1 Tail Length · 2 Pitch Punch (pitchEnv amount) · 3 Punch Time · 4 Crunch (stage drives)
         5 Click · 6 Tail Tone (EQ2 boost freq) · 7 Reverse Tail · 8 Sub
```

| Name | Style | Distinguishing recipe |
|---|---|---|
| Euphoric Kick | euphoric | tail sine, pitchEnv +31 st (≈ 250 Hz from F1) / 0.04 s, decay 0.28; stage 1 Clipper soft 10 dB, stage 2 Overdrive 12 dB; sub macro high |
| Euphoric Kick Long | euphoric | decay 0.42, ROOT_HINT F1 |
| Raw Kick | raw | tail sine→tri blend, pitchEnv +36 st / 0.045 s, decay 0.4; stage 1 Clipper **hard** 18 dB, stage 2 Clipper hard 14 dB, EQ +9 @ 700; mid-heavy |
| Raw Kick Long | raw | decay 0.55, Resonator 1 voice (root+12) mix 0.15 for the "ring" |
| Zaag Kick | raw | tail **saw**, LP 2 kHz, Clipper hard → Shaper (stepped curve) — the saw-tailed "zaag" kick |
| Gated Kick | raw | Raw Kick → Gate fixed (hold = 1/8 at tempo, range −80) — chopped tail |
| Tok Kick | early | punch-heavy, tail decay 0.12, click layer bright, EQ +6 @ 1.2 kHz |
| Reverse Bass | early | layer: tail render reversed (Phase 8) or va saw A 1 ms D 0.18 S 0; EQ HP 80 (above kick sub), +8 @ 700; Clipper hard + Shaper ("phase distortion"); StereoImager width 1.3; pattern = off-beats only |
| Pitched Kick Melody | hardstyle | Euphoric Kick (Lab) — keyed melodic kick for kick-melody drops |
| UK Hardcore Kick | UK/happy | punchy, decay 0.2, pitchEnv +28 st / 0.025 s, Overdrive 18 dB, tail short — 170–175 BPM |
| Happy Hardcore Kick | happy | as UK, brighter click, Clipper soft |
| Uptempo Kick | uptempo | decay 0.15, Clipper hard 24 dB ×2, very short tail, Distortion 20 |
| Gabber Kick | gabber | core/909 Kick → Overdrive 36 dB → EQ (+10 @ 200, cut 1.5 k) → Overdrive 24 dB → Limiter — the "909 into distortion" |
| Frenchcore Kick | frenchcore | three layers named as sold: **punch** (ds), **tail** (va sine keyTrack, decay 0.18) and **resonance** (Resonator root, fb 0.9); Shaper + OTT; 190–200 BPM |
| Frenchcore FX Kick | frenchcore | Frenchcore Kick → RingMod 180 Hz mix 0.4 → Clipper |
| Hardtekk Kick | tekk | DnB-style higher punch (ds char 1, tune +5, decay 0.15), **little clipping** — room for the tekk bass |
| Jumpstyle Kick | jumpstyle | core/909 Kick, Overdrive 10 dB, decay 0.3 |
| Hard Dance Kick | hard dance | between Euphoric and UK: decay 0.25, Overdrive 14 dB |

Each style ships a matching **pattern** (`patterns/kick_<style>.adx`) with the
right placement: off-beat reverse bass, frenchcore offbeat patterns,
uptempo rolls, and so on.

### 4.2 Leads and screeches (~20)

| Name | Recipe |
|---|---|
| Euphoric Lead | rack, 3 layers × va: osc1+osc2 saw, unison 8, detune 34 c; L2 +12 st at −9 dB (air); L3 noise HP 3 kHz with pitchEnv on the filter; mod1 lfo1 sync 1/8 → osc fine-tune 0.35 (the "wobble"); pitchEnv +2 st / 25 ms; per-layer Compressor + Clipper soft; mono legato; chain EQ HP 120 → OTT 0.3 → sends Hall 3 s + Delay 1/4 dotted, both ducked from the kick (VolumeShaper or Ducker keyed) |
| Euphoric Pluck | Euphoric Lead with env D 0.2 S 0, filter env +0.5 |
| Euphoric Chords | Euphoric Lead poly (4), unison 6 — "reuse the melody layers, same rhythm" |
| Euphoric Pad | Euphoric Lead, A 0.8 s, LP 3 kHz, Hall 6 s |
| Rawstyle Screech | va saws unison 6 → mod1 lfo1 **20 Hz unsynced** → osc coarse pitch 0.15 (macro Vibrato Depth), Filter formant morph LFO, Resonator 2 voices → Clipper hard 16 dB → Shaper → OTT 0.5. Mono |
| FM Screech | fm alg 1, op2 ratio 1.5, feedback 0.6, mod1 lfo1 20 Hz → pitch; mod2 modEnv1 → op2 level → Clipper → FormantFilter (vowelA a, vowelB e, LFO 6 Hz) |
| Air Screech | Rawstyle Screech → Filter HP 900 Hz, Hall 2 s |
| Pitch-Dive Screech | Rawstyle Screech + pitchEnv −12 st / 0.3 s (the dive), glide 80 ms |
| Laser | va saw, pitchEnv +36 st / 0.1 s, Delay 1/16 → Hall |
| Hoover (Juno A) | wt `juno-pwm-saw` (saw × pulse), 3 layers −12/0/+12 st; mod1 lfo1 6.5 Hz → warp PWM 0.6; pitchEnv **−12 → 0 st** over 0.18 s (the dive, mono legato); resonance 0.4 → **Ensemble 4 voices, Juno I+II** → Hall. Macro 1 Dive, 2 PWM Rate, 3 Chorus |
| Hoover (Juno B) | as A with `juno-detune-saw` (two counter-detuned saws). **The gate keeps whichever the user prefers; both may ship** |
| Hoover Heavy | Hoover A + sine layer −12 st ("layer with a sine if the hoover lacks weight") |
| Mentasm Stab | Hoover A, pitchEnv +5 st 40 ms, short env — the "Second Phase" rave stab |
| Rave Organ Stab | additive drawbar organ (harmonics 1,2,3,4,6,8) A 1 ms D 0.25 → Rotary fast → Hall short |
| Happy Supersaw Stab | Euphoric Lead poly, D 0.15 S 0.2, brighter |
| Chip Lead (S3RL-ish) | va pulse 25 %, vibrato lfo1 sync 1/16 delay 0.25 s, Delay 1/8 |
| Jumpstyle Square | va square, unison 2, Overdrive 8 dB, LP 3 kHz |
| Gabber Alarm | va square + saw, pitchEnv, Distortion 24 → EQ |
| Frenchcore Lead | va saw unison 6 → VolumeShaper 1/16 gate → Clipper hard → Hall |
| Trashwave Screech | stakillaz/Screech Lead → this pack's chain (Clipper, OTT) |

### 4.3 Basslines (~8)

`Tekk Bass` (va saw + sine sub, Filter LP 700 Hz follow, Overdrive amp-style 18 dB,
off-beat pattern, ducked 0.9 from kick), `Tekk Bass Rolling` (1/16 pattern),
`Jumpstyle Bass` (va square −12 st, Overdrive, off-beat), `Hard Dance Offbeat`
(va saw, filter env pluck, Clipper), `Frenchcore Bass` (tail-derived, Shaper),
`Hardstyle Sub` (core/Sub Sine, high-passed off the kick), `Kick Bass` (Kick Lab
tail-only render, keyed — the kick-as-bassline), `Reese Rumble` (for raw intros).

### 4.4 Keys, vocals, FX (~14)

| Name | Recipe |
|---|---|
| Happy Hardcore Piano | smp Salamander/VCSL Steinway, velocity bright → EQ +4 @ 3 kHz → Compressor 4:1 → Hall 1.2 s. Bright and *not cheap* — the research's warning |
| Piano Stab | Happy Hardcore Piano, release 0.1 |
| FM Rave Piano | fm alg 5 (M1-ish piano), op2 ratio 1, op4 ratio 14 short — the 90s rave piano |
| Nightcore Vocal Chain | hyperpop/Nightcore Up, tuned for 170 BPM |
| Gym Vocal Chain | PitchShifter +2..+4 st → Hard Tune → Compressor → Hall — "white girl hardstyle" pop vocal |
| Hardstyle Vocal Chain | Compressor → Saturation → Hall 2 s ducked → Delay 1/4 |
| Kick Crash | Crash + Impact (core) |
| Riser Hardstyle | core/Noise Riser 8 + Pitch Riser layered |
| Screech Riser | Rawstyle Screech pitchEnv +24 st over 4 bars |
| Snare Roll 150 | core/Snare Roll Builder at 150 |
| Reverse Kick FX | Euphoric Kick reversed + Hall |
| Atmos Choir | additive formant "ah" → Ensemble → Hall 8 s (breakdowns) |
| Countdown Blip | fm short blip |
| Drop Silence Cut | VolumeShaper gate template (the "brief silence before the drop") |

### 4.5 Bus and master chains (~6)

`Hardstyle Master` (glue Compressor 2:1 → Clipper soft −0.5 → Limiter −0.1 dBTP —
"glue, clipper, limiter"), `Kick Bus` (Clipper soft → Limiter), `Ducked Reverb Return`,
`Ducked Delay Return`, `Lead Bus` (OTT 0.25 → Clipper soft → EQ HP 120), `Uptempo Master` (harder clip).

---

## 5. Templates

`euphoric_150.adx`, `raw_155.adx`, `early_tok_145.adx` (reverse bass), `uk_hardcore_172.adx`,
`happy_hardcore_175.adx` (piano + hoover), `hardtekk_165.adx`, `jumpstyle_145.adx`,
`frenchcore_198.adx`, `gabber_180.adx`, `gym_hardstyle_155.adx` (pop-vocal remix
skeleton). Each is intro → build → drop → breakdown → second drop, with the
kick tail re-pitched on the second drop if it changes key.

---

## 6. Tests

| Test | Asserts |
|---|---|
| `pack_hardcore_loads/golden/no_alloc` | as every pack |
| `kick_lab_per_key_tuning` | each Lab zone's tail fundamental is within ±5 cents of its key, E1–E2 |
| `kick_lab_matches_live_rack` | the Lab render of a key matches the live rack at that key within −60 dB residual (proves Lab is the same sound, not a different one) |
| `kick_layers_band_split` | click energy is above 1 kHz and sub energy 30–80 Hz, measured |
| `kick_pitch_envelope` | measured start pitch and drop time within 5 % of the preset's values |
| `reverse_bass_offbeat_template` | the early template's reverse bass notes are all on off-beats |
| `hoover_dive` | Hoover pitch starts −12 st and reaches 0 within the configured time |
| `screech_vibrato_rate` | measured pitch modulation ≈ 20 Hz |
| `ducked_returns` | reverb return ducks ≥ 10 dB on each kick |
| `pack_hardcore_cpu` | the live Euphoric Lead (3 layers × 8 unison × 2 osc) + Raw Kick rack < 30 % of a 256-frame block |
| `pack_hardcore_demo_renders` | as every pack |

---

## 7. Definition of done

- [ ] §6 passes.
- [ ] Listening sessions held for all nine artists; §11 log filled. (judgement)
- [ ] Every kick style has a Lab render committed to the rendered store with its hash.
- [ ] `docs/examples/hardcore_demo.adx` (one drop per style is fine).
- [ ] **Human gate:** the user A/Bs every kick style against their references
      (matched loudness), and picks the hoover variant. Leads and screeches are
      approved in their words, per style. (judgement)
- [ ] `docs/STAKILLAZ.md` cross-referenced (the trashwave crossover presets).
- [ ] FINAL_PLAN §10, STATE.md, launcher check.

---

## 8. Out of scope

Third-party kick samples (Phase 12's library store holds CC0 libraries only;
the user may add their own kicks to the user preset folder) · VST kick synths
(Kick 2/3 via Phase 9 hosting works but is not part of a native pack).

## 9. Risks

| Risk | Mitigation |
|---|---|
| Kicks are judged on tiny differences | Kick Lab makes iteration fast (one script run per tweak); the gate is per style with references at matched loudness |
| CPU of live layered leads | Lab-style renders exist for kicks; leads can be frozen (Phase 8) |
| Little is documented for four of the named artists | handled honestly: listening sessions, and presets named by sound, not by artist |

## 10. Sources

Kicks: [Hardwave Studios — kick synthesis](https://hardwavestudios.com/learn/sound-design/hardstyle-kick-synthesis) ·
[Music Production Wiki — how to make hardstyle](https://musicproductionwiki.com/articles/how-to-make-hardstyle.html) ·
[EDMProd — how to make hardstyle](https://www.edmprod.com/how-to-make-hardstyle/) ·
[Native Instruments — hardstyle](https://www.native-instruments.com/blogs/blog/hardstyle-music) ·
[The Ghost Production — hard styles tools](https://theghostproduction.com/hard-styles-tools-kicks-bass-dj-sets/) ·
[Myloops — hard techno kicks](https://www.myloops.net/creating-hard-techno-kicks) ·
[Septum frenchcore kick tutorial](https://septum.gumroad.com/l/septumtutorial) ·
[Mr. Bassmeister frenchcore kick pack listing](https://annasarchive.it.com/?p=266535)

Leads and hoover: [ADSR — hardstyle screech in Serum](https://www.adsrsounds.com/serum-tutorials/hardstyle-screech-sound-design-with-serum/) ·
[KVR — formant/comb screech discussion](https://www.kvraudio.com/forum/viewtopic.php?p=7319839) ·
[SoundBridge — hardstyle lead](https://www.soundbridge.io/hardstyle-lead) ·
[Syntorial — supersaw](https://www.syntorial.com/tutorials/crafting-the-supersaw-edm-dance-synth-lead-a-step-by-step-guide/) ·
[Steinberg — hardstyle leads](https://www.steinberg.net/tutorials/build-hardstyle-leads) ·
[Music Production Wiki — hoover](https://musicproductionwiki.com/articles/how-to-recreate-the-hoover-sound) ·
[Elektronauts — Alpha Juno hoover](https://elektronauts.com/t/syntakt-patch-requests-alpha-juno-2-hoover/198696) ·
[SuperCollider list — Juno PWM saw](https://listarc.cal.bham.ac.uk/lists/sc-users-2009/msg53777.html) ·
[Renoise forum — "What the" patch](https://forum.renoise.com/t/roland-alpha-juno-style-what-the-patch/70579) ·
[Gearspace — V-Synth "What the"](https://gearspace.com/threads/v-synth-what-the-patch.571568/)

Genres and artists: [MusicRadar — 24 happy hardcore tips](https://www.musicradar.com/tuition/tech/24-happy-hardcore-tips-33701) ·
[MusicRadar — Mandidextrous hardtek kick/bass](https://musicradar.com/news/the-breakdown-mandidextrous-2) ·
[Soundtrap — jumpstyle](https://blog.soundtrap.com/what-is-jumpstyle-music/) ·
[Splice — hardstyle/rawstyle/hardcore](https://splice.com/sounds/packs/sample-tools-by-cr2/hardstyle-rawstyle-hardcore/story) ·
[Remzcore (Bandsintown)](https://www.bandsintown.com/ja/a/1932313) ·
[Tatsunoshin (Viberate)](https://www.viberate.com/artist/tatsunoshin/) ·
[Tatsunoshin (Festival Insider)](https://festivalinsider.com/articles/this-week-in-hard-dance-tatsunoshin-hysta-relianze-code-black-and) ·
[Romance Planet (event listing)](https://washington.org/event/romance-planet) ·
["White Girl Hardstyle" playlist](https://open.spotify.com/playlist/75mxTzMgoP14bSSkdzYXKU) ·
[docs/STAKILLAZ.md](../docs/STAKILLAZ.md)

---

## 11. Listening-session log

*(artist · reference · element · parameters extracted · match verdict)*
