# Phase 17 — Pack: Ambient · M

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `engine/preset/packs/ambient/` — steel pan and handpan, mallets and music boxes, soft pianos, analog pads and leads, Dorian-Concept keys, granular/spectral textures, space and tape chains |
| **FINAL_PLAN refs** | §5.9 (extends the C418 suite), §5.12, §7 Phase 17 |
| **Entry criteria** | [phase_16.md](phase_16.md) §7 complete (the orchestral parts of this pack reuse its presets) |
| **Next** | [phase_18.md](phase_18.md) |
| **Pack id** | `ambient` · `REQUIRES=core, classical` (and `c418`, which it extends rather than replaces) |
| **Artist references** | C418, Dorian Concept (others named by the user later join §3's listening sessions) |

---

## 1. Objective

Soft, warm, spacious instruments: the C418 palette done properly, now that adX
has physical models, real samples, tape and long reverbs, plus Dorian
Concept's wonky analogue-and-Wurlitzer side. The existing `c418` pack (seven
presets, Phase 4) stays as-is for its golden hashes. This pack adds the deeper
versions beside it.

---

## 2. Research summary

**C418 (Minecraft Volume Alpha/Beta).**

- *Production:* all of Volume Alpha and Beta were made in **Ableton Live**,
  after earlier work in Schism Tracker. Reviews describe soft synths and pads
  with frequent piano. "Aria Math" is "a song full of Pan Drums and old
  synthesisers rhythmically dancing to ping pong delays" (C418, quoted in
  docs/C418.md), and its steelpans are "gentle, muted".
- *Unconfirmed claims:* the Moog hardware (only the title "Moog City 2"
  suggests it), Kontakt and the Soniccouture Hang Drum (docs/C418.md cites
  c418wiki and Equipboard). Presets are named for sounds, not gear.
- *Devices from docs/C418.md:* mallet/pluck timbres; warm Juno/Prophet-style
  slow pads with gentle LP movement; space as an instrument (long reverbs,
  synced ping-pong); rising 16th arpeggios; imperfect, rolled-off stacks rather
  than brickwall digital brightness.

**Dorian Concept.**

- The **microKORG** was his main production and live instrument until 2014. He
  has a "love/hate" relationship with it.
- *Joined Ends* (Ninja Tune, 2014) dropped it for a **Wurlitzer electric piano
  and a handful of analogue synths**, with his own vocals. A **Moog Prodigy**
  (bought ~2011) began the move to vintage gear.
- He studied sound design and music production in Salzburg.
- In adX terms: Wurlitzer (P modal reed + light drive + tremolo), Prodigy-style
  mono lead/bass (2-osc, ladder, glide), microKORG-style vocoder and wonky lead
  (pitch-bend heavy, mod-wheel vibrato), and a "sloppy" feel from per-note
  random detune and timing.

**Steel pan:**

- Makers tune the lowest three partials to **fundamental, octave and twelfth**
  (or double octave).
- In bass pans the (1,0) mode sits on the octave and the (0,1) mode on the
  twelfth. Some twelfth partials decay slowly, which gives an audible
  "aftersound".
- The "steel" character comes from coupling between neighbouring note areas,
  which are laid out by harmonic relationship (Rossing & Hansen).
- In adX: modal table 8 (1 : 2 : 3), with the third mode's decay ~1.6× the
  others for the aftersound. A sympathetic coupling layer, at −24 dB, rings
  the octave and the fifth neighbours. The exciter is a soft mallet (rubber
  stick).

---

## 3. Listening sessions

As Phase 14 §3. The user chooses references from C418 (e.g. "Aria Math",
"Sweden", "Moog City 2", "Dreiton", "Subwoofer Lullaby") and Dorian Concept
(e.g. tracks from *Joined Ends* and *The Nature of Imitation*). Phase 10's
`TimbreAnalyzer` is especially useful here, because these sounds are sparse
and exposed.

---

## 4. Catalogue (~48)

### 4.1 Tuned percussion and plucks (~12)

| Name | Recipe |
|---|---|
| Steel Pan | P modal table 8, mallet hardness 0.35, mode 3 decay ×1.6, coupling layer −24 dB, env R 1.5 s → EQ cut 300 Hz −2 → core Room |
| Muted Pan Drum | Steel Pan, hardness 0.2, decayTilt 0.6, LP 3 kHz — the "gentle, muted" Aria Math pan |
| Aria Pan Echo | rack: Muted Pan Drum → c418/Ping-Pong Echo → c418/Long Hall |
| Handpan | P modal 9, hardness 0.25, 3-cent spread, Hall 3 s |
| Tenor Pan Bright | Steel Pan, hardness 0.6 |
| Kalimba (Sampled) | VCSL Kalimba Kenya/Tanzania [c418/Kalimba] |
| Mbira | VCSL Mbira dzaVadzimu [P modal 14] |
| Music Box | P modal 1 high register, hardness 0.95 → Tape age 0.3 |
| Celesta Soft | classical/Celesta → LP 6 kHz → Hall |
| Bowed Vibes | VCSL vibraphone, A 0.6 s, Tremolo off → Hall shimmer 0.1 |
| Glass Harmonica | P modal 15, bow exciter, slow A |
| Wine Glasses | VCSL Wine Glasses |

### 4.2 Keys (~10)

| Name | Recipe |
|---|---|
| Felt Piano | VCSL Upright (Knight), velocity → LP 1.5–5 kHz, noise layer (felt thump) −30 dB, release triggers → Tape age 0.2 → Hall 3 s |
| Sweden Piano | Salamander, soft dynamics only, Hall 4 s mix 0.3 — soft, centred, sparse |
| Giant Piano (Sampled) | VCSL Steinway, the c418/Giant Piano voicing as an EQ match |
| Haggstrom Keys | fm alg 5 bright EP, Delay 1/8 dotted |
| Wurlitzer | P modal 12 (reed), hammer hardness by velocity → Overdrive 4–14 dB by velocity → Tremolo 5 Hz depth 0.35 → Room. The *Joined Ends* instrument |
| Wurlitzer Dirty | Wurlitzer → Overdrive 18 dB → Tape |
| Rhodes Soft | classical/Rhodes, tine bark low, Tremolo stereo 4 Hz |
| Jazz Organ | classical/Hammond 888000000 → Rotary slow |
| Clav Funk | classical/Clavinet → Filter env (auto-wah) |
| Toy Piano | P modal 1 (free bar) short → Tape |

### 4.3 Synths (~12)

| Name | Recipe |
|---|---|
| Juno Pad | va saw + sub, unison 2 detune 6 c, ladder LP 1.6 kHz, lfo1 0.15 Hz → cutoff 0.2, A 1.5 s R 3 s → Ensemble Juno I → Hall 5 s |
| Prophet Pad | va saw + pulse (PWM lfo2 0.3 Hz), ladder LP 2.2 kHz res 0.2, filterEnv slow → Ensemble II |
| Dreiton Pad | the c418 Warm Pad voiced on VA + Tape |
| Moog City Lead | va saw + saw +7 c, ladder LP 1.2 kHz res 0.3, filterEnv 0.4, mono legato glide 60 ms → Delay 3/16 → Hall |
| Subwoofer Lullaby Bass | va sine + tri −12, LP 300 Hz, A 0.3 s S 1 R 1.5 |
| Prodigy Mono | va 2 saws, ladder res 0.45, filter env 0.6, voice.mode legato, glide 45 ms — Moog Prodigy-style lead/bass |
| Wonky Lead | Prodigy Mono + mod1 random-per-note → pitch 0.03, mod2 modwheel → lfo1 vibrato 0.1, pitch-bend range 12 — the microKORG wonk |
| Vocoder Choir | Vocoder carrier saw-noise, 24 bands, sibilance 0.4 (mic or Voice into the key) — microKORG vocoder |
| Arp Glass | wt digital, short pluck, channel arp Up 1/16 2 oct → Ping-Pong Echo — the cascading C418 arpeggio |
| Old Synth Pluck | va square, LP env, Tape wow 0.4 — "old synthesisers dancing" |
| Tape Strings | classical/Violin Section sus (soft) → Tape → Hall 6 s |
| Breath Flute Pad | classical/Flute non-vib, A 0.4 s → Hall shimmer 0.15 |

### 4.4 Textures (~8)

`Granular Cloud` (granular over a rendered Juno Pad, size 220 ms, density 25,
spray 0.3, pitchJitter 20 c), `Frozen Choir` (classical/Choir → SpectralFreeze),
`Shimmer Bed` (noise → Filter BP 900 Hz → Hall shimmer 0.6 decay 20 s),
`Rain Room` (noise → Filter LP/HP + VolumeShaper random → Hall),
`Tape Hiss Bed` (Tape hiss only), `Field Hum` (sine 50/60 Hz + harmonics, −40 dB),
`Glass Drone` (Resonator over noise, root and fifth), `Distant Brass Swell`
(classical/Horn sus soft → Filter LP 1.5 kHz → Hall 8 s).

### 4.5 Space and tape chains (~6)

`Tape Age` (Tape wow 0.25 flutter 0.15 sat 0.3 age 0.5 hiss −48 dB), `Cassette`
(stronger), `Shimmer Hall` (Hall 9 s shimmer 0.35 +12), `Infinite Hall` (Hall 45 s,
freeze macro), `Ping-Pong Hall` (c418 Ping-Pong Echo → Long Hall), `Lo-Fi Room`
(Room + Bitcrush 14 bit mix 0.2).

---

## 5. Templates

`aria_math_like_96.adx` (pan drums + ping-pong + old-synth arps; tempo and key
are the user's choice, and nothing is transcribed), `felt_piano_sketch_70.adx`,
`wurli_jazz_wonk_92.adx` (Wurlitzer, Prodigy, sloppy drums),
`granular_drone.adx`. The generator side is Phase 10's C418 suite, which these
presets now feed.

---

## 6. Tests

`pack_ambient_loads/golden/no_alloc/cpu/demo_renders` as every pack, plus:

| Test | Asserts |
|---|---|
| `steel_pan_partials` | the first three partials of Steel Pan at C5 are at 1 : 2 : 3 within 1 %, and the third outlasts the first by ≥ 1.4× (the aftersound) |
| `wurlitzer_velocity_drive` | the 2nd/3rd harmonic ratio rises with velocity (the bark) |
| `long_tail_no_denormals` | Infinite Hall for 120 s with input then silence: CPU stays flat, output decays cleanly |
| `c418_pack_unchanged` | the original `c418` pack's golden hashes are untouched |

---

## 7. Definition of done

- [ ] §6 passes.
- [ ] Listening sessions logged (§9). (judgement)
- [ ] `docs/C418.md` gains a short "Phase 17 additions" pointer section. It is reference data, so the pointer is the only edit.
- [ ] `docs/examples/ambient_demo.adx`.
- [ ] **Human gate:** the user approves the steel pan family, the pianos, the
      Wurlitzer and the pads against their references, in their words. (judgement)
- [ ] FINAL_PLAN §10, STATE.md, launcher check.

## 8. Risks

| Risk | Mitigation |
|---|---|
| Long reverbs and granular clouds eat CPU | `pack_ambient_cpu`; Hall's FDN is sized once; granular density is capped by a macro range |
| Unconfirmed gear claims leak into preset names | names describe sound only; claims stay attributed in docs/C418.md |

## 9. Listening-session log

*(artist · reference · sound · parameters · verdict)*

## 10. Sources

[C418 (Wikipedia)](https://en.wikipedia.org/wiki/C418) · [Epigram — C418](https://epigram.org.uk/c418-the-music-of-minecraft-and-more/) ·
[docs/C418.md](../docs/C418.md) ·
[Dorian Concept (Wikipedia)](https://en.wikipedia.org/wiki/Dorian_Concept) ·
[MusicTech — Dorian Concept and the microKORG](https://musictech.com/news/gear/dorian-concept-korg-microkorg/) ·
[Electronic Beats interview](https://electronicbeats.net/dorian-concept-i-realized-that-the-biggest-challenge-for-me-would-be-to-do-something-simple) ·
[Joined Ends notes (Ninja Tune via Affine)](https://affinerecords.com/?p=1835) ·
[Hansen & Rossing — the Caribbean steelpan](https://www.hangblog.org/the-caribbean-steelpan-and-some-offsprings) ·
[Bass steelpan modes](https://labs.sonicfield.org/library/vibrational-modes-and-sound-spectra-of-bass-steelpans) ·
[Steel pan layout (jaced)](https://jaced.com/2005/06/27/steel-drum-tuning-and-layout/) ·
[Zarek — vibration of steel band pans (IOA)](https://www.ioa.org.uk/system/files/publications/JHB%20ZAREK%20VIBRATION%20OF%20STEEL%20BAND%20PANS.pdf)
