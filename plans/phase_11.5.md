# Phase 11.5 — M-Vave FM-1: full hardware support and native DX7 voices · XL

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `engine/instruments/dx7/`, `engine/midi/dx7/`, `engine/midi/sysex/`, `engine/hardware/fm1/`, the FM-1 effects rack, the `dx7` editor panel, the Hardware panel, the firmware catalog client |
| **FINAL_PLAN refs** | §5.13, §7 Phase 11.5, §8 (msfa row, firmware-catalog row), §9 (hardware-half and judgement conventions) |
| **Entry criteria** | [phase_9.md](phase_9.md) §6 complete — **hard dependency**: `MidiInput`, `MidiOutput`, `MidiRouter`, `MidiLearn`, `MidiClock`, the device-profile convention and the plugin host (for the Dexed comparison) all come from there. [phase_11.md](phase_11.md) §6 — **soft dependency**: the FM-1's 27 keys as a launcher and its key LEDs as pad feedback reuse `PadController`/`PadFeedback`; everything else stands without Phase 11 |
| **Next** | none required. Independent of Phases 12–19 |
| **§5 coverage owned** | §5.13 **all** |

Rewritten 2026-11-08 at the user's request: "fully support everything the FM-1
supports from its hardware", with patches coming into adX **as editable native
synths** as the centrepiece. The first draft (2026-11-05) is superseded; §9
lists what it got wrong.

---

## 1. Objective

The M-Vave FM-1 is a 6-operator FM pocket synth. Its engine is a port of
Google's `music-synthesizer-for-android` ("msfa", the core Dexed also uses),
running fixed-point at 44 118 Hz in 64-sample blocks. It uses byte-exact DX7
lookup tables, and its presets are DX7 voices. Every patch made on it is a DX7
voice, so a faithful home for that voice in adX is a DX7-exact engine. A
best-fit onto the simpler `FmInstrument` would not do.

Four things the user gets from this phase:

1. **Their FM-1 sounds, inside adX, as native, editable, automatable synths.**
   A new built-in instrument, `dx7`, ports msfa (Apache-2.0) and exposes every
   DX7 voice parameter as a `ParamRef`. Patches arrive from a `.syx` file, from
   a live SysEx dump or the V16 Bank Export, and every one opens as that
   instrument. It sounds the way it did on the device, can be edited in an adX
   panel, automated, MIDI-learned, saved as an `.adxpreset`, rendered offline,
   and needs no hardware once imported.
2. **The device's whole MIDI surface, both ways.**
   - Into adX: notes from its keys, arp and sequencer, velocity where the
     firmware sends it, recorded as takes, with adX as the clock master so its
     sequencer and arp lock to the project.
   - Out of adX: notes on up to 8 parts, bend and bend range, program change,
     CC 1/2/4/7/11/64, every effects CC by name, DX7 single-parameter edits so
     the adX editor drives the device's sound live, clock and start/stop, and
     the V16 key LEDs.
3. **The device as an external instrument and an audio interface.** Its USB
   audio (UAC1, 24-bit stereo) comes back into a channel with latency
   compensation, so a part played on the hardware sits in the mix like a
   plugin.
4. **Its firmware market, carefully.** A hash-verified catalog of
   `thefirmwarehub.com`'s open firmwares, and an updater that speaks the
   device's own documented OTA protocol over USB-MIDI SysEx. It detects the ROM
   recovery mode and has a hard confirmation gate before every write.

Three things make this cheaper than it looks:

- **msfa is small, permissive and proven.** It is Apache-2.0, about 3 kLOC of
  C++, with no dependencies. It already powers Dexed, which Phase 10.5 hosts
  as a test fixture, so a working reference to compare against is already in
  the repo's toolchain.
- **DX7 SysEx is forty years old and fully documented.** The voice and bank
  formats are fixed and every field is known.
- **MIDI I/O, clock out, learn, external audio input and pad feedback exist
  already.** Phase 9 built them, and Phase 11 built pad feedback. The new
  pieces are SysEx transport, the DX7 engine and codec, the FM-1 device
  profile, and the updater.

The risk worth naming is unchanged from the first draft. The device has a
single flash bank, and its only fallback is the mask-ROM `WL82 UBOOT1.00`
mass-storage mode. A failed write is recovered with community tools, not
guaranteed. §4.11 treats that as a central constraint.

---

## 2. What the hardware supports — the reference this phase is built to

Gathered 2026-11-07/08 from M-VAVE's manuals and firmware notes (V14, V15,
V16), the device's reverse-engineering write-ups, the FM1 Open SDK notes, and
the community tools (fm1-editor.com, dx7-to-fm1.dev, fm1-emu, sloopDX, Skiff).
Items marked **unverified** are what the emulated tests assume and what the
hardware half of §6 confirms. Each is probed at runtime, never hard-wired.

### 2.1 Synth engine

| Fact | Detail |
|---|---|
| Engine | msfa port (Dexed core), fixed-point, 44 118 Hz, 64-sample blocks, byte-exact DX7 tables (exp, sin, frequency, EG rate/level, LFO, pitch-EG) |
| Voice format | DX7 155-byte voice (VCED); banks as 32 x 128-byte packed VMEM |
| Polyphony | 12 voices (V14/V15); 20 shared across 8 parts (V16) |
| Multitimbral | V16: 8 parts on channels 1–8, each with level, pan, bend range, glide, ±100 cent tuning |
| Play modes | poly / mono, glide (portamento), bend range 1–24 |
| Effects | V16: true-stereo chain — filter (LP/HP/BP), reverb (room/hall/plate), delay, distortion, chorus, phaser. Older firmware: a smaller chain on the same CC page |
| Presets on device | banks of 32; V16 Bank Export sends a bank as DX7 `.syx` (sound data only — **effects are not in the dump**) |

### 2.2 MIDI it transmits (stock firmware)

- Notes it plays — keys, arp, sequencer — on channel 1, over USB and BLE.
- Velocity from the keys since V15. Whether velocity is **transmitted** is
  **unverified** (it may send a fixed 100). adX detects this from the first
  notes and labels the take; it never assumes.
- **Nothing else**: no CCs, no pitch bend, no program change, no clock out, no
  MIDI thru. The knobs and encoders cannot be MIDI-learned from the device.
  Skiff (a community firmware) adds clock out over USB, and that is all.

### 2.3 MIDI it receives

| On | Messages |
|---|---|
| Its MIDI channel ("Midi Chn"; V16: parts 1–8) | note on/off with velocity, pitch bend, program change (bank slot), CC 1 mod wheel, CC 2 breath, CC 4 foot, CC 64 sustain; V16 adds CC 7 volume and CC 11 expression |
| The effect channel (default 2) | FX CCs — **CC 0–23 on V14/V15, CC 70–93 on V16**; V16 also lights the 27 key LEDs with notes 0–26 (on/off only, silent) |
| Any channel, Sync on | MIDI clock, start, stop (arp and sequencer follow); MTC and SPP are ignored |
| SysEx | DX7 single voice `F0 43 0n 00 01 1B …`, 32-voice bulk `F0 43 0n 09 20 00 …` (4096 + checksum), DX7 single-parameter change `F0 43 1n gg pp vv F7` (as Dexed sends); the vendor protocol under manufacturer ID `00 32` |

Whether the device answers a DX7 **dump request** (`F0 43 2n 00 F7` /
`F0 43 2n 09 F7`) is **unverified**. If it doesn't, pulling patches off the
device means the user presses Bank Export, and the librarian waits for it
(§4.4).

### 2.4 Vendor SysEx (manufacturer `00 32`)

- Identity: query `F0 00 32 45 00 00 00 40 7F F7`. The device answers only
  parameter ID `0x40`, with a model string such as `FM-1_015` or `FM-1_014`.
  That gives the firmware generation.
- Enter-upgrade: `F0 22 24 35 7F F7`.
- OTA, pulled by the device. After enter-upgrade the device re-enumerates as
  `4D4A:4155` "ota-FM-1", then **requests** blocks with `F0 00 32 41 41 …`
  read requests. Data goes back pack7-encoded (an LSB-first 7-bit bitstream)
  with checksum `~(flashtype + Σdata + Σaddr + Σlen)`. A request for address
  `0xF0000000` marks completion. There is no signature check.
- Recovery: mask ROM `WL82 UBOOT1.00`, USB mass storage, VID `0x4C4A`,
  PID `0x8057`.

### 2.5 Transports and audio

| Port | What |
|---|---|
| USB-C | composite USB-MIDI ("FM-1 Midi") + UAC1 audio ("FM-1 Audio"), VID:PID `4c4a:c755`, driver-free. Audio: 24-bit, 2-channel, adaptive; hosts report 44.1 or 48 kHz. V16 adds "USB Audio Loopback". **Unverified**: whether the computer-to-device (playback) direction is exposed on stock firmware |
| 3.5 mm TRS | **MIDI in only** (Type A) — not an output |
| Bluetooth LE | MIDI both ways. Reported unreliable by some users; best effort |
| Headphone/line out | analogue; outside adX |

### 2.6 Firmware ecosystem

- Official firmware: V14, V15, and V16 (2026-10-21).
- FM1 Open: M-VAVE's open firmware, SDK and browser workbench.
- Community firmwares on `thefirmwarehub.com`:
  - sloopDX: DX7 bulk, 256 voices.
  - Skiff: clock out.
  - Melodee, Dinghy, ECHOFIELD and FMini-1.
  - FM-1+VA: closed, with its own patch format.
- Tools: fm1-editor.com, dx7-to-fm1.dev, and fm1-emu (joakimeriksson). fm1-emu
  runs firmware images unchanged, in the browser or natively, so it can serve
  as an oracle.

---

## 3. Deliverables — exact file manifest

```
cmake/AdxDependencies.cmake       msfa: FetchContent_Declare of upstream
                                   google/music-synthesizer-for-android at a
                                   pinned commit, with its line
                                   "# adx-license: msfa | msfa | Apache-2.0 |
                                   dx7 instrument"; tools/gen_third_party.py
                                   picks it up for THIRD-PARTY and the installer.
                                   The upstream tree is also the oracle that
                                   dx7_engine_matches_msfa compiles against.
engine/instruments/dx7/msfa/       the ported core (dx7note, env, fm_core,
                                   fm_op_kernel, lfo, pitchenv, exp2, sin,
                                   freqlut, controllers), Apache headers kept,
                                   changes marked, made to follow the RT rules
                                   (§4.1). Never Dexed's GPL additions.

engine/instruments/dx7/
  Dx7Instrument.h/.cpp             the `dx7` channel node: voice pool, mono/poly,
                                   glide, bend range, tuning, pan, part level;
                                   renders msfa blocks at the engine rate, or at
                                   44 118 Hz resampled ("FM-1 rate", §4.2)
  Dx7Params.h/.cpp                 every DX7 voice parameter + FM-1 part
                                   parameters as ParamRefs (§4.3), with the
                                   DX7's own ranges and display names
  Dx7Voice.h                       the unpacked 155-byte voice as a struct; the
                                   instrument's canonical state
engine/midi/dx7/
  Dx7Codec.h/.cpp                  VCED single voice and VMEM 32-voice bank:
                                   pack/unpack, checksum, the packed-bit fields,
                                   lenient import (out-of-range clamped, counted)
  Dx7ParamChange.h/.cpp            DX7 single-parameter change messages, both
                                   directions (gg/pp/vv addressing)
  Dx7ToFm.h/.cpp                   optional *approximate* conversion onto the
                                   existing FmInstrument, with a report of what
                                   it dropped (§4.5)
engine/midi/sysex/
  SysexQueue.h                     RT-safe hand-off of SysEx from the MIDI
                                   callback to the main thread (§4.4)
  SysexAssembler.h/.cpp            joins fragmented SysEx (RtMidi, BLE) into
                                   whole messages, bounded size, timeout
engine/midi/MidiInput.cpp          keeps SysEx (today dropped at line 55) and
                                   routes it to SysexQueue, never to channels
engine/midi/MidiOutput.h/.cpp      sendSysex(), paced (§4.4)
engine/hardware/fm1/
  Fm1Identity.h/.cpp               USB names / VID:PID; vendor identity query and
                                   reply parse; firmware generation; capability
                                   probe results (§4.8)
  Fm1Profile.h/.cpp                parses `.adxdevice`; per-generation CC map;
                                   part channels; LED notes; the effect channel
  profiles/
    fm1_v14.adxdevice              one file per firmware generation whose CC
    fm1_v15.adxdevice              layout is known; Open and community firmwares
    fm1_v16.adxdevice              get their own file when their maps are
    fm1_open.adxdevice             published, else fall back to raw CC
  Fm1Device.h/.cpp                 the connected device: parts, FX state adX
                                   owns, program per part, send-on-connect
                                   snapshot, live-edit follow (§4.6)
  Fm1Bootloader.h/.cpp             detects ota-FM-1 (4D4A:4155) and UBOOT1.00
                                   mass storage (4C4A:8057)
  update/
    OtaProtocol.h/.cpp             enter-upgrade, serve device read requests,
                                   pack7, checksum, completion (§4.11)
    FirmwarePackage.h              one catalog entry: name, source, URL,
                                   version, sha256, licence, Hub risk notes
    FirmwareCatalog.h/.cpp         fetch + cache + SHA-256 check
    Flasher.h/.cpp                 verify -> confirm -> write -> confirm-boot
engine/effects/fm1/
  Fm1Effects.h/.cpp                the FM-1 effects chain as one native effect
                                   (filter LP/HP/BP, reverb room/hall/plate,
                                   delay, distortion, chorus, phaser) with the
                                   device's parameter ranges, built from the
                                   existing native DSP blocks (§4.7)

app/adx/panels/instrument/dx7/
  editor.py                        the dx7 editor: algorithm diagram, operator
                                   strip x6 (ratio/fixed/detune/level/velocity/
                                   AMS), 4-rate/4-level EG curves, keyboard
                                   scaling curve, pitch EG, LFO, globals, name;
                                   "Send to FM-1 while editing" toggle
app/adx/panels/hardware/
  fm1_panel.py                     status: connected / ota / UBOOT / not found,
                                   firmware, transports, probe results; tabs
  librarian_tab.py                 banks and voices: import .syx, receive dump,
                                   wait-for-Bank-Export, audition, rename, open
                                   as dx7 channel, send voice/bank, export .syx
  parts_tab.py                     V16 parts 1–8: channel, program, level, pan,
                                   bend range, glide, tuning; external-
                                   instrument wiring; audio return + latency
  fx_tab.py                        the device's effects by name, adX-owned state,
                                   "Match in adX" (load Fm1Effects with the same
                                   settings)
  firmware_tab.py                  catalog, risk notes, install, back to
                                   official, confirmation gate, recovery guide
app/adx/pads/profiles/fm1_keys.adxpad   the 27 keys as a launcher, LEDs on V16

tools/
  virtual_fm1.py                   emulated FM-1 over loopMIDI (§4.12)
  fetch_fm1_firmware_catalog.py    refreshes the cached, hashed manifest
docs/
  fm1.md                           setup on all transports, import, the editor,
                                   parts, FX, clock, LEDs, firmware + recovery

tests/cpp/dx7/                     test_codec, test_param_change, test_engine_
                                   vs_msfa, test_params_cover_voice, test_dx7_
                                   no_alloc, test_fm1_rate, test_to_fm_report
tests/cpp/midi/                    test_sysex_assembler, test_sysex_queue,
                                   test_midi_input_keeps_sysex
tests/cpp/hardware/fm1/            test_identity, test_profiles, test_device_
                                   snapshot, test_ota_against_mock, test_
                                   flasher_gates, test_bootloader_detect
tests/python/                      test_dx7_editor.py, test_fm1_librarian.py,
                                   test_fm1_parts.py, test_fm1_fx.py,
                                   test_firmware_catalog.py
tests/scenarios/                   fm1_import_bank, fm1_live_edit, fm1_capture_
                                   sequencer, fm1_external_part,
                                   fm1_firmware_install_emulated
tests/fixtures/dx7/                synthesized voices and banks only, covering
                                   every field's extremes (§5); never a copied
                                   commercial or factory library
bench/bench_dx7.py                 voices per core at 48 kHz / 64 frames
```

---

## 4. Design

### 4.1 Porting msfa to the engine's rules

msfa is ported, not wrapped. It is upstream Google code (Apache-2.0). The
pinned upstream commit is declared in `cmake/AdxDependencies.cmake` with its
`adx-license` line, so `tools/gen_third_party.py` carries its notice into
THIRD-PARTY and the installer. The ported copy lives in
`engine/instruments/dx7/msfa/`, with its Apache headers kept and each change
marked. Dexed's GPL-3 additions are never copied: its controllers, its
"engine types" and its UI. Dexed is used only as an external comparison plugin
(§5), which it already is.

The port follows the RT rules for `engine/instruments`:

- No allocation on the audio thread. Voices are `rt::OwnedArray`, sized at
  `prepare`.
- No banned headers.
- The lookup tables are built once at load and are const after.
- Fixed-point arithmetic is kept exactly. The bit-exactness test (§5) holds
  the port to upstream's output.

The DX7 tables are the ones the FM-1 also uses (exp2, sin, freqlut, the EG
rate and level tables, LFO and pitch-EG). That is what makes "sounds like the
device" a testable claim, not a hope.

### 4.2 `dx7`, the instrument

- **Rate.** msfa renders 64-sample blocks.
  - In **engine-rate** mode (the default), the block is fed the host rate, as
    Dexed does.
  - In **FM-1-rate** mode, the instrument renders at 44 118 Hz and resamples
    to the engine rate with the existing polyphase resampler. Aliasing and
    envelope timing then match the hardware's.
  - The choice is a channel parameter, saved with the project.
- **Voices.**
  - Polyphony is per channel. The default is 16; 20 matches V16's shared pool,
    and 12 matches V15.
  - Voice stealing works like `FmInstrument`'s.
  - Mono mode is last-note priority. Legato retrigger follows DX7 mono
    behaviour.
  - Glide has the DX7's two modes, retain and follow, plus a time setting.
- **Controllers.** Mod wheel, breath, foot and aftertouch route to pitch, amp
  and EG bias with the DX7 function-parameter ranges and assigns. Sustain is
  CC 64, with a DX7-faithful release when held. Bend range runs 0–12 (DX7) or
  up to 24 (FM-1).
- **Offline render** is identical to live, and deterministic per placement.
  `rt` voice phases start at zero, as on the device, not seeded.
- **Cost.** `bench_dx7.py` must show at least 64 voices on one core at 48 kHz
  with 64-frame blocks before the phase closes. msfa is cheap, and this is a
  check, not a stretch.

### 4.3 Parameters — every field of the voice, editable

The instrument's state is the `Dx7Voice` struct, and every field is a
`ParamRef`. That makes each field automatable, MIDI-learnable, undoable and
saved in the project. A `.syx` voice and a dx7 channel are interconvertible
with no loss, by construction.

| Group | Parameters (DX7 ranges) |
|---|---|
| Per operator x6 | EG rate 1–4 (0–99), EG level 1–4 (0–99), output level (0–99), oscillator mode (ratio/fixed), coarse (0–31), fine (0–99), detune (−7…+7), key velocity sensitivity (0–7), amp mod sensitivity (0–3), keyboard level scaling: break point (A-1…C8), left/right depth (0–99), left/right curve (−LIN, −EXP, +EXP, +LIN), keyboard rate scaling (0–7), operator on/off |
| Pitch EG | rate 1–4, level 1–4 (0–99) |
| Voice | algorithm (1–32), feedback (0–7), oscillator key sync (on/off), transpose (C1–C5, ±2 oct), name (10 chars) |
| LFO | speed (0–99), delay (0–99), pitch mod depth (0–99), amp mod depth (0–99), key sync (on/off), wave (triangle, saw down, saw up, square, sine, sample & hold), pitch mod sensitivity (0–7) |
| Function (DX7 global) | poly/mono, pitch bend range, bend step, portamento mode/glissando/time, mod wheel / foot / breath / aftertouch range and assign (pitch, amp, EG bias) |
| FM-1 part (V16) | part level, pan, bend range (1–24), glide, tuning (±100 cents) |
| adX extras | FM-1-rate mode, polyphony, output gain |

Two rules make this editable rather than just storable:

- **Parameter changes are voice-safe.** A change mid-note behaves as on a DX7:
  EG and level changes take effect on the running voice, and frequency changes
  on the next sample. That matches what the device does when it receives a
  parameter change, so "edit in adX while the device plays the same patch"
  sounds the same on both.
- **Presets are ordinary.** Save writes an `.adxpreset` with the voice fields,
  and Export writes `.syx`. The preset browser lists a dx7 preset with the
  other native ones, and its category comes from the bank name where there is
  one.

### 4.4 SysEx transport — the missing plumbing

`MidiInput` drops SysEx today (`engine/midi/MidiInput.cpp:55`). This phase
changes that:

- RtMidi's SysEx filter is turned off for inputs that ask for it. These are
  the FM-1's inputs, and any input with "Receive SysEx" set in Preferences >
  MIDI.
- **On the MIDI callback thread**, bytes go to `SysexAssembler`. It holds a
  fixed buffer (8 KiB, enough for a DX7 bank of 4104 bytes and V16 OTA frames)
  and joins fragments, because BLE and some drivers split messages. When a
  message completes, it is pushed into `SysexQueue`. That is a fixed-slot
  single-producer/single-consumer ring of size-prefixed messages, with no
  allocation; a full ring drops the message and counts the drop.
- **SysEx never reaches a channel or the audio graph.** The main thread drains
  the queue at frame rate and dispatches by header:
  - DX7 voice, bank or parameter change goes to the librarian and to the
    following editor.
  - `00 32` goes to `Fm1Identity` or the updater.
  - Anything else goes to a "SysEx monitor" in the MIDI page.
- `MidiOutput::sendSysex` paces large messages: a bank goes out in 256-byte
  slices about 10 ms apart on BLE and TRS, and whole over USB. This avoids
  overrunning the device's buffer.

**Getting patches off the device**, in order of preference:

1. **Dump request.** The librarian sends `F0 43 2n 09 F7` and waits 2 s.
   Whether the device answers is unverified (§2.3); the result is cached per
   firmware.
2. **Bank Export (V16).** The librarian shows "Press Bank Export on the FM-1"
   and waits for the dump.
3. **A `.syx` file** from fm1-editor.com, dx7-to-fm1.dev, sloopDX banks (eight
   VMEM banks in a row are read as 256 voices) or any DX7 library.

`Dx7Codec` import is lenient:

- Out-of-range values are clamped and counted, never rejected.
- A bad checksum is reported, and the bank can still be opened after an
  explicit "open anyway".
- Packed and unpacked forms are both read.
- Voice names keep their bytes. Non-ASCII bytes are shown as `?` but written
  back unchanged.

### 4.5 `FmInstrument`, and why it is not the target

The existing `FmInstrument` covers only part of a DX7 voice:

- It has 6 operators, the 32 DX7 algorithms, per-operator ratio/fixed, detune,
  level, ADSR and velocity, and global feedback, level and glide.
- It lacks the 4-rate/4-level EG, the LFO, the pitch EG, keyboard level and
  rate scaling, transpose, amp and pitch mod sensitivity, and osc key sync.

The first draft's "lossless onto `FmInstrument`" and "LFO maps to existing
ParamRefs" were therefore wrong. `Dx7ToFm` stays as an optional one-way
"Convert to FM (approximate)" action on a dx7 channel. It fits each 4-stage EG
to an ADSR and drops what has no target. It shows a report of every dropped
or approximated field, and the dx7 channel is never replaced silently.
`FmInstrument` itself is not changed.

### 4.6 The device as an external instrument

`Fm1Device` represents a connected FM-1 and is driven from the Hardware panel
and the channel rack.

- **Parts.** On V16 there are 8 parts on channels 1–8, each a channel-rack
  external-instrument channel sending to the FM-1's output port on its part
  channel. Before V16 there is one part on the device's channel. Per part,
  adX sends:
  - program change, at project start and whenever a pattern changes it;
  - bend range;
  - CC 1, 2, 4, 7, 11 and 64 from automation lanes or played input;
  - level, pan, glide and tuning, by the CCs or SysEx the profile names for
    them. Where no message is known, they are set on the device and recorded
    in the project as a note.
- **Audio return.** The part's audio comes back on the FM-1's USB audio input
  device:
  - Opened through the existing input path (Phase 9 §4.9) and routed to a
    mixer insert.
  - Latency compensation is measured by "Ping": a note out, then its onset in
    the return, reusing the recording-offset measurement. The result is
    stored per device and sample rate.
  - Monitoring follows the existing input-monitoring rules.
- **FX state is owned by adX.** The device never reports its effect settings,
  and effects are not in dumps (§2.1, §2.2). So:
  - The project holds the FM-1 FX state.
  - It is sent as a full CC snapshot on connect, on project load and on
    transport start (configurable).
  - It is automatable by name from the `.adxdevice` map.
  - Changes made on the device's own knobs cannot be seen. `docs/fm1.md` and
    the fx tab say so plainly.
- **Live edit ("Send to FM-1 while editing").** Each `dx7` parameter change in
  the editor is sent as a DX7 single-parameter change. Rate is limited to
  50 messages/s per parameter, and the last value is always sent. The device
  therefore plays the patch as it is being edited. On choosing a different
  voice, the whole voice is sent as a single-voice dump.
- **Clock.** "FM-1 follows adX" sends MIDI clock and start/stop to the FM-1
  port, through the existing `MidiClock` with its latency offset. The device's
  arp and sequencer then lock to the project tempo when Sync is on. adX never
  expects clock *from* stock firmware. With Skiff, adX can follow the device
  through the existing `MidiSync` input.
- **Capturing what it plays.** Notes on channel 1, from the keys, the 64-step
  sequencer and the arp, record into a pattern as an ordinary MIDI take.
  - **External arp**: on V16 the device arpeggiates notes adX sends it. A part
    can be marked "arp source" and its output recorded back. The loop is
    detected and the echo is not double-recorded.
  - **Velocity detection**: if 16 consecutive notes arrive with one identical
    velocity, the take is flagged "device sent fixed velocity" (§2.2).
- **Program change from adX** selects a device bank slot. The librarian knows
  which voices adX last sent to which slot, so a program number has a name.

### 4.7 The FM-1's effects, natively

`Fm1Effects` is one native effect that reproduces the device's chain, in
order:

- a filter (LP/HP/BP, cutoff and resonance);
- distortion;
- chorus;
- phaser;
- delay (time, feedback, mix, tempo sync);
- reverb (room, hall and plate, with size, damping and mix).

Its parameter names and ranges follow the `.adxdevice` CC map one to one. Two
things follow from that:

- An imported dx7 voice plus "Match in adX" gets the same sound in an
  all-software project. The FX tab copies the adX-owned FX state into an
  `Fm1Effects` instance on the dx7 channel's insert.
- A project can move between the hardware and software versions with one
  click either way.

It is built from the existing native blocks (the `Reverb`, `Delay`, `Drive`,
`Modulation` and filter DSP), not new algorithms. Matching it is a listening
judgement, not a bit-exactness claim, because M-VAVE's FX code is not public.
Where FM1 Open's source reveals the algorithms, the closer version follows.

### 4.8 Identity and capability probing

On connect, a port named `FM-1 Midi`, or on Windows a device with VID:PID
`4c4a:c755`, is matched to an FM-1. adX then:

1. sends the vendor identity query and parses the model string into a firmware
   generation (14, 15, 16, or unknown). With no reply, it falls back to
   asking the user, with V15 preselected;
2. loads the matching `.adxdevice`;
3. probes once per firmware, and caches the results:
   - whether the device answers a dump request;
   - whether it echoes a single-voice dump into its edit buffer (heard by a
     test note through the audio return, if open);
   - whether the USB playback direction exists;
   - whether velocity is transmitted (§4.6);
4. shows the results on the panel. Nothing in the app depends on a probe
   having passed when it can degrade instead.

A community firmware that changes the model string, or does not answer, is
"unknown". adX falls back to the generic DX7 behaviour, the raw CC map, and
the features each probe confirms.

### 4.9 Device profiles: `.adxdevice`

This is a sibling of Phase 11's `.adxpad`. It has one file per firmware
generation, and `engine/hardware/fm1/profiles` ships them.

```
[DEVICE_PROFILE]
NAME="M-Vave FM-1 (V16)"
MATCH_DEVICE="FM-1 Midi"
MATCH_MODEL="FM-1_016"
PARTS=8                       ; channels 1..8
EFFECT_CHANNEL=2
BEND_RANGE_MAX=24
LED_NOTES=0-26                ; on the effect channel, on/off
SENDS_VELOCITY=probe
[PART_CC]
MOD=1  BREATH=2  FOOT=4  VOLUME=7  EXPRESSION=11  SUSTAIN=64
[FX_CC]                        ; V16 page; V14/V15 files use 0..23
FILTER_TYPE=70  FILTER_CUTOFF=71  FILTER_RESO=72 ...  REVERB_MIX=93
```

Every name in `[FX_CC]` becomes an output `ParamRef` on the device. It
appears in automation, in MIDI learn (an input controller can drive the
device's reverb by name) and in `Fm1Effects`.

The exact CC-to-name table for each generation is transcribed from M-VAVE's
published CC guide into the profile files. `test_profiles` checks each file
against a hand-entered copy of the guide's table, so a typo fails a test.

### 4.10 Keys and LEDs as a Phase 11 controller

`fm1_keys.adxpad` maps the 27 keys (channel 1 notes) to a 9 x 3 launcher grid:

- one row per scene;
- the top-octave keys as scene launch;
- the last key as stop-all.

On V16, `PadFeedback` drives the key LEDs in on/off mode through notes 0–26 on
the effect channel:

- a playing clip is solid;
- a queued clip blinks at the pad-feedback rate;
- an empty cell is off.

Older firmware has no LED control, and the profile says `FEEDBACK=none`.
Using the keys as pads is a mode the user turns on. By default the keys play
notes.

### 4.11 Firmware: catalog, updater, recovery

**Catalog.** `FirmwareCatalog` fetches `thefirmwarehub.com`'s FM-1 list and
caches it outside git. Each open-source entry carries a URL, version,
licence, the Hub's risk notes and a SHA-256. Closed-source firmwares are
link cards only. Official M-VAVE packages are entries when M-VAVE publishes
the file; otherwise they are a link to M-VAVE's updater. Nothing is vendored.

**Updater.** `OtaProtocol` implements the device-pull protocol from §2.4. It
is the documented protocol M-VAVE's own updater and the Hub's installer use,
and nothing new. The steps:

1. **Verify.** The package bytes must match the catalog SHA-256 and parse as
   an FM-1 image (size, header). If not, the package is never offered.
2. **Confirm.** A dialog names the firmware, its source and its licence. It
   says plainly that the device has no verified recovery path, and that a
   failed write needs the UBOOT recovery tools. The user types the firmware
   name to proceed. A bare OK is not enough.
3. **Enter upgrade.** adX sends `F0 22 24 35 7F F7` and waits up to 10 s for
   `ota-FM-1` (4D4A:4155) to enumerate.
4. **Serve.** adX answers each read request with the addressed slice,
   pack7-encoded, with the checksum. It refuses requests outside the image,
   and progress follows the highest address served. The device sets the pace;
   adX never pushes.
5. **Finish.** The `0xF0000000` request marks completion. adX waits for the
   device to re-enumerate as `FM-1 Midi`, re-runs the identity query and
   confirms the version changed.
6. **Interrupted.** If the device vanishes mid-transfer, adX stops, never
   retries on its own, and opens the recovery guide.

**Recovery.** `Fm1Bootloader` detects UBOOT1.00 (VID 0x4C4A, PID 0x8057, mass
storage) and an `ota-FM-1` left with no transfer running. It shows exactly
which state the device is in, and the steps for the community recovery tools
(fm1_rescue.py, MvaveFM1Unbricker, M-Flasher). adX does not write in UBOOT
mode, because that path cannot be tested safely here.

**Backup** means adX keeps the exact package last flashed successfully. There
is no known way to read the current firmware off the device, so the panel
doesn't offer one.

### 4.12 `tools/virtual_fm1.py` — the emulated device

This is a headless loopMIDI device, like `tools/virtual_pad.py`. Its model
generation is selectable (14, 15, 16 or unknown). It:

- answers the identity query;
- answers dump requests (behind a switch, so both probe outcomes are tested);
- sends a Bank Export of a fixture bank on command;
- accepts and applies single-voice dumps, banks and parameter changes,
  keeping the device's state so tests can read it back;
- records FX CCs, program changes, bend and LED notes;
- plays a scripted sequencer pattern as channel 1 notes, with fixed or real
  velocity, and follows clock;
- runs the OTA device side: re-enumerates under the ota name (a second
  loopMIDI port), issues read requests and checks checksums. It can simulate a
  disconnect at a chosen address and a bad checksum.

Where loopMIDI can't emulate something (USB re-enumeration, audio), the
emulator swaps the port and the test says what it does not cover.

**Optional oracle.** If fm1-emu's licence permits use as a fetch-only test
tool, `fm1_engine_matches_emulator` renders a fixture voice through fm1-emu
running an official firmware image (fetched, hashed, never vendored) and
compares it with `dx7` in FM-1-rate mode. If the licence does not permit it,
the test is not built and the hardware capture (§6) carries this check alone.

---

## 5. Tests

| Test | Asserts |
|---|---|
| `dx7_codec_roundtrip` | every field of every fixture voice and bank survives unpack → pack byte-identically; packed bit fields at their extremes; checksum computed and verified |
| `dx7_codec_lenient` | out-of-range bytes are clamped and counted; a bad checksum is reported and openable; eight concatenated banks read as 256 voices (sloopDX) |
| `dx7_param_change_roundtrip` | every (group, parameter) address encodes and decodes to the same `ParamRef` and value |
| `dx7_params_cover_voice` | each of the 155 voice bytes maps to exactly one ParamRef and back; nothing in the voice is unreachable from the editor |
| `dx7_engine_matches_msfa` | for every fixture voice, chords and single notes at three velocities, sustain on/off, LFO on/off, all 32 algorithms: the port's output is **bit-identical** to unmodified upstream msfa compiled into the test, block by block |
| `dx7_matches_dexed` | the same voices through Dexed 1.1.0 (the Phase 10.5 fixture, hosted) at 48 kHz: envelope correlation > 0.98 and spectral centroid within 2 % per note (Dexed's own additions mean bit-identity is not expected) |
| `dx7_fm1_rate` | FM-1-rate mode renders at 44 118 Hz internally and resamples; note timing within 1 sample of engine-rate mode |
| `dx7_no_alloc` | 64 voices, parameter automation and patch changes on the audio thread allocate nothing (the RT allocation hook) |
| `dx7_offline_equals_live` | an offline render equals a realtime render of the same project |
| `dx7_to_fm_report` | conversion lists every dropped field; a voice using none of them converts with zero report entries |
| `sysex_assembler` | fragments joined; over-size dropped and counted; an interrupted message (new status byte) discarded |
| `sysex_queue_no_alloc` | the MIDI-callback side never allocates; full ring drops and counts |
| `midi_input_keeps_sysex` | SysEx reaches the main-thread queue and never a channel's events |
| `fm1_identity` | the model strings for V14, V15 and V16 resolve; an unknown or absent reply falls back as specified |
| `fm1_profiles` | each `.adxdevice` loads and matches the hand-entered CC guide table for its generation |
| `fm1_device_snapshot` | connect sends the full FX snapshot and the per-part program and bend range, in a fixed order |
| `fm1_live_edit_rate_limit` | a 1 kHz knob sweep sends at most 50 msgs/s/param and always ends on the final value |
| `ota_against_mock` | the served bytes, pack7 and checksum match the emulator's requests byte for byte; out-of-range requests are refused |
| `flasher_gates` | bad hash: never offered; no typed confirmation: no enter-upgrade sent; mid-transfer disconnect: stop, no retry, recovery guide shown |
| `bootloader_detect` | an emulated UBOOT and a stranded ota device each route to guidance, never to `Flasher` |
| `test_dx7_editor.py` | every editor control moves its ParamRef and back; the EG and scaling curves redraw from the values; the name field is limited to 10 chars |
| `test_fm1_librarian.py` | import, browse, rename, audition, open as channel, export: bytes identical to the source |
| `test_fm1_parts.py` | 8 parts route to channels 1–8; program and bend range sent; ping measures a known offset in a loopback |
| `test_fm1_fx.py` | each named FX parameter sends its CC on the profile's channel; Match in adX sets `Fm1Effects` to the same values |
| `test_firmware_catalog.py` | fetch, cache, hash check; offline uses the cache; closed entries are not installable |
| `scenario fm1_import_bank` | through the real app (`adX.cmd`): emulator sends Bank Export → librarian lists 32 voices → one opens as a dx7 channel → it plays → edited → saved → reopened identical → exported `.syx` equals the edited voice |
| `scenario fm1_live_edit` | editor changes arrive at the emulator as parameter changes; its state equals the adX voice at the end |
| `scenario fm1_capture_sequencer` | adX sends clock; the emulator's sequencer pattern records as a take aligned to the bar within 1 ms; fixed velocity flagged |
| `scenario fm1_external_part` | a part's notes, program, CCs and FX reach the emulator; the audio return is a loopback stand-in with a known delay that ping measures |
| `scenario fm1_firmware_install_emulated` | catalog → verify → typed confirm → enter-upgrade → served → done → identity re-read; and the interrupted variant ends at the recovery guide; **does not cover real flash, real USB re-enumeration or a real interrupted write** |

Fixtures: `tests/fixtures/dx7/` contains generated voices only:

- one per algorithm;
- every EG, scaling curve and LFO wave at its extremes;
- random-but-valid voices from a fixed seed;
- a bank with a deliberately bad checksum.

No factory ROM or commercial bank is committed.

---

## 6. Definition of done

- [ ] `dx7` is a built-in instrument: `dx7_engine_matches_msfa` (bit-identical to
      upstream for all 32 algorithms and every fixture), `dx7_params_cover_voice`,
      `dx7_no_alloc` and `dx7_offline_equals_live` pass; `bench_dx7.py` shows
      ≥ 64 voices on one core.
- [ ] Every DX7 voice parameter is editable in the dx7 editor, automatable and
      MIDI-learnable (`test_dx7_editor.py`), and saves and reloads in the project
      and as an `.adxpreset`.
- [ ] SysEx reaches the main thread through the RT-safe queue and never the audio
      graph (`midi_input_keeps_sysex`, `sysex_queue_no_alloc`).
- [ ] Patches made on the FM-1 come into adX as editable dx7 channels. **Emulated
      half** (makes it `[~]`): `scenario fm1_import_bank` from a Bank Export and
      from a `.syx`. **Hardware half:** a bank made on the real device, pulled by
      dump request or Bank Export, every voice opened and played, deferred to the
      final review. (hardware half)
- [ ] An imported voice sounds like the device. **Emulated half:** `dx7_matches_dexed`,
      and `fm1_engine_matches_emulator` if fm1-emu's licence permits. **Hardware
      half:** five voices captured from the real FM-1 over USB audio against the
      dx7 render in FM-1-rate mode, envelope correlation > 0.95, then a listening
      verdict recorded in STATE.md. (hardware half) (judgement)
- [ ] Editing in adX drives the device live and voices go back to it
      (`scenario fm1_live_edit`, export roundtrip). **Hardware half:** the same on
      the real device. (hardware half)
- [ ] The FM-1 plays as an 8-part external instrument with program, bend range,
      part CCs, named FX and audio return with measured latency
      (`scenario fm1_external_part`). **Hardware half:** a two-part pattern on the
      real V16 device, the audio return aligned within 1 ms after ping. (hardware half)
- [ ] The FM-1's sequencer and arp lock to adX clock and record as takes
      (`scenario fm1_capture_sequencer`). **Hardware half:** a real 64-step pattern
      recorded, bar-aligned, velocity behaviour noted. (hardware half)
- [ ] `Fm1Effects` reproduces the device FX by name and Match in adX copies the
      state (`test_fm1_fx.py`). **Hardware half:** each effect type compared by ear
      against the device. (hardware half) (judgement)
- [ ] The keys work as a Phase 11 launcher and the V16 key LEDs show clip state
      (emulated through `virtual_fm1.py`). **Hardware half:** on the real device.
      (hardware half)
- [ ] The FM-1 enumerates as USB MIDI and USB audio (input; playback if probed) in
      Preferences, and Bluetooth MIDI connects or its failure is documented.
      (hardware half) — no software stand-in exists for a real composite device.
- [ ] The firmware catalog loads and caches, and every write is hash-verified and
      typed-confirmed (`flasher_gates`). The full install and the interrupted
      install complete against the emulator (`scenario fm1_firmware_install_emulated`).
      **Hardware half — the highest-stakes box:** one real flash of a known
      community firmware and back to official, at the final review, with §4.11's
      recovery limits stated first; the go-ahead and the result are the device
      owner's own call, recorded in their words. (hardware half) (judgement)
- [ ] `docs/fm1.md` written and the msfa notice in the generated THIRD-PARTY file; FINAL_PLAN §8 and §10
      rows current.

---

## 7. Order of work

Each step ships tested before the next starts. Nothing touches flash until
step 9.

1. **SysEx plumbing**: `SysexAssembler`, `SysexQueue`, `MidiInput` keeping
   SysEx, `MidiOutput::sendSysex`, and the SysEx monitor.
2. **msfa port + `dx7` instrument**: bit-identity against upstream first, then
   the RT rules, then the bench.
3. **`Dx7Codec` + `Dx7Params`**: the voice is the instrument's state, and
   every byte is a ParamRef.
4. **The dx7 editor panel**, `.adxpreset` and `.syx` save/load, and the Dexed
   comparison.
5. **`virtual_fm1.py`** (MIDI side), `Fm1Identity`, `.adxdevice`, and the
   librarian tab, with import from all three paths.
6. **`Fm1Device`**: parts, FX snapshot, live edit, clock, capture, ping and
   audio return. Then the parts and fx tabs.
7. **`Fm1Effects`** and Match in adX.
8. **`fm1_keys.adxpad`** and LED feedback.
9. **Firmware**: catalog (read-only), then the OTA emulator side, `OtaProtocol`,
   `Flasher`, `Fm1Bootloader` and the firmware tab, all against the emulator.
10. **`docs/fm1.md`**, the regenerated THIRD-PARTY notices, and the ledger.

---

## 8. Out of scope (and why)

| Thing | Status |
|---|---|
| Reading the device's FX settings or knob moves | **impossible on stock firmware** — it transmits no CCs and dumps carry no FX; adX owns FX state (§4.6) |
| MIDI-learning adX parameters from the FM-1's knobs | same reason; its keys (notes) can drive anything, its knobs cannot |
| TRS MIDI output from the device | the jack is input only |
| FM-1+VA patches and other closed community formats | not planned — no public format; DX7-format banks from any firmware (sloopDX included) are supported |
| FM1 Open workbench projects | stretch — only if the format is documented; otherwise later |
| A from-scratch UBOOT recovery writer | **not planned** — guidance to the proven community tools instead (§4.11) |
| Vendoring firmware, msfa from Dexed, or any factory/commercial patch bank | **never** — msfa from upstream, firmware fetch-only, fixtures synthesized |
| Bit-exact emulation of M-VAVE's FX | not possible without their source; a native match judged by ear (§4.7) |
| Other M-Vave devices | not planned; `Fm1Identity` is narrow by design |

---

## 9. Corrections to the 2026-11-05 draft

- "Lossless onto `FmInstrument`" and "LFO, pitch EG → existing ParamRefs":
  wrong. `FmInstrument` has no LFO, no pitch EG and no 4-stage EG. The
  faithful target is the new `dx7` instrument (§4.5).
- "Named effects CCs are MIDI-learnable from the device": the device sends no
  CCs. Named CCs are adX → device (§4.9).
- "3.5 mm TRS MIDI" listed as a bidirectional transport: it is input only.
- "Universal Device Inquiry" for identity: the device answers the vendor
  query under `00 32` (§2.4).
- "The JieLi update handshake written against fm1_rescue.py": the OTA path is
  the device-pull protocol in §2.4. fm1_rescue.py is a recovery tool for UBOOT
  mode, and adX defers to it, not reimplements it.
- "Nothing in this phase changes DSP": this phase adds a DX7 engine. The
  sound-alike claim is now tested (§5, §6).
- Missing entirely: SysEx input (adX dropped it), V16 multitimbral parts, the
  sequencer/arp capture with clock out, key LEDs, Bank Export, live
  parameter-change editing, the effects chain, USB audio return as an external
  instrument.

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| A real flash is interrupted | typed confirmation stating the limit; device-paced transfer; stop-and-guide on any loss; no write in UBOOT mode (§4.11) |
| A §2 fact marked unverified is false on a given firmware (dump request, velocity out, USB playback, parameter-change echo) | every one is probed at runtime and degraded around (§4.8); emulator covers both outcomes |
| msfa port drifts from upstream while being made RT-safe | `dx7_engine_matches_msfa` is bit-exact and runs in CI |
| The device's buffer overruns on a bank dump over BLE or TRS | paced sending (§4.4); bank sends verified by re-request where dump request works |
| Hub catalog or OTA protocol changes (V17+) | cached catalog; hash check regardless of source; protocol version tied to the identity reply, unknown models refused for flashing |
| CC map changes in a new firmware | a new `.adxdevice` file, not code (§4.9) |
| Bluetooth MIDI unreliable on Windows | best effort, documented; USB is the supported path |
| fm1-emu's licence forbids use as an oracle | test not built; Dexed comparison + hardware capture remain |
