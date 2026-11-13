# Phase 12's demonstration set

One sound per gap phase 12 closed (phase_12.md §1, §6). Open any rack or preset from the
browser, or render one with `python tools/packs/audition.py <file>`.

| Gap | File | What it shows |
|---|---|---|
| G1, G2 | `euphoric-kick.adxrack` | three layers (punch, tail, click) into a clip/OTT/limit chain with a Hall send ducked by the kick; two macros, Decay and Drive |
| G3 | `hoover.adxpreset` | a VA hoover: unison saws, a pitch dive at the note's start, vibrato that arrives late, legato glide |
| G4 | `pluck-guitar.adxpreset`, `marimba.adxpreset`, `bowed-cello.adxpreset` | the `physical` instrument's pluck (guitar body), modal (marimba table) and bow (cello body) |
| G5 | `ott-lead.adxrack` | a clipped supersaw through OTT into a shimmer Hall |
| G6 | `cowbell-808.adxpreset`, `cowbell-melody.adx` | DrumSynth's 808 cowbell, played as a melody |
| G7 | `violin-section-ks.adxpreset` | VSCO 2's keyswitched violin section, imported from its SFZ (needs `fetch_libraries.py fetch vsco2ce`) |
| G8 | (made locally) | the WannaCry transcription exported as a `.mid`, imported onto one 16-part `gm` channel (File > Import MIDI on one GM channel), and played once on the Windows `gm.dls` and once on GeneralUser GS from the library store (`fetch_libraries.py fetch generaluser`). The song is the user's own corpus and is never committed: `check_demo_set.py --wannacry` builds both projects under `build/phase12-demo/wannacry/` |

`python tools/packs/check_demo_set.py [--wannacry]` checks the set before anyone listens:
each sound loads, renders with no `adx glitch` findings and is listed in the browser's
Sounds section. Its report is `build/phase12-demo/report.json`.
