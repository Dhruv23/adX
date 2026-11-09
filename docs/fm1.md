# The M-Vave FM-1

adX knows the M-Vave FM-1 by name. Its patches come in as editable `dx7` channels, adX can
edit the sound on the device while you work, the device plays parts of a project, and its
firmware can be updated, carefully. Open **View > Hardware**.

Some of what follows has only been tested against adX's emulated FM-1
(`tools/virtual_fm1.py`), not a real one. Those parts are marked *(unverified on a device)*.
The final review checks them on hardware.

## Connecting

Use **USB-C**, with a data cable. The FM-1 shows up as a MIDI port named `FM-1 Midi` and an
audio device named `FM-1 Audio`, with no driver to install. adX finds the port within a
second, opens it, and asks the device which firmware it runs. The Hardware panel then shows
the model (`FM-1_015`, `FM-1_016`, ...) and loads the matching profile. If the device does
not answer, adX assumes V15. If you know better, choose the profile in the panel.

The 3.5 mm TRS jack is MIDI *in* only: the FM-1 can be played through it but sends nothing
back. **Bluetooth MIDI** works for notes when Windows pairs it, but some users report it is
unreliable. Firmware updates and patch transfers need USB.

## Patches as dx7 channels

The FM-1's engine is a port of msfa, the same engine Dexed is built on. adX ships the same
engine as the `dx7` instrument. Its output is tested bit-identical to unmodified upstream
msfa for every algorithm and for voices with every field at its extremes. So a patch made on
the FM-1 sounds the same in adX.

The **Librarian** tab gets voices three ways:

1. **Receive from FM-1** asks the device for its bank. Many firmwares do not answer that
   request. If nothing comes back within two seconds, the panel says *Press Bank Export on
   the FM-1* (V16), and adX listens for the bank.
2. **Import .syx…** reads any DX7 file: single voices, 32-voice banks, and sloopDX's
   256-voice files (eight banks in a row). A file with a bad checksum asks before it
   opens. Values out of range are clamped and counted, never refused.
3. **A file made by fm1-editor.com or dx7-to-fm1.dev** is just a `.syx`, so it imports
   the same way.

Then:

- **Open as dx7 channel** (or double-click a voice) adds a channel playing it, as one undo
  step.
- **Audition** plays it once.
- **Rename** changes its 10-character name.
- **Send voice** and **Send bank** put voices back on the device.
- **Export .syx…** writes the bank.

A dx7 channel holds every byte of the voice as a parameter. Every byte can be automated,
MIDI-learned, saved with the project and changed in the text. The instrument editor has an
overview with the algorithm, the chosen operator's envelope and keyboard scaling, the pitch
envelope and the name, beside the full grid of parameters. It can:

- export the voice as `.syx`;
- save it as an `.adxpreset`;
- convert it to the FM instrument (approximate). The FM instrument has no LFO, pitch
  envelope or keyboard scaling, so the conversion lists everything it dropped or
  approximated, and the dx7 channel is kept.

**FM-1 rate.** The `fm1.rate` parameter renders at the device's 44 118 Hz and resamples to
the project rate, so the aliasing and envelope timing are the device's. Off, the engine runs
at the project rate as Dexed does.

## Editing on the device

Tick **Send to FM-1 while editing** in a dx7 channel's editor. Every change you make is then
sent to the FM-1 as a DX7 parameter change, at most 50 a second per parameter, always ending
on the final value. The device plays the patch as you edit it. Turning the setting on sends
the whole voice first.

## Parts, effects and clock

The **Parts** tab lists the device's parts: eight on channels 1-8 on V16, one before that.
Each has a program, a bend range and a level. **Send everything** sends all of them and
every effect setting at once. adX also does this when the device connects.

**Add part channels** adds a channel per part (`FM-1 part 1` … `FM-1 part 8`). Each uses
the `midiout` instrument: its notes play on the FM-1, on that part's channel, while adX
makes no sound for it. Write and arrange them like any other channel. If the FM-1
reconnects on a different port, adX re-points them. **Ping** times the device's audio
coming back, which needs **FM-1 Audio** as the input in Preferences > Audio, so the return
can be lined up with the rest of the mix *(unverified on a device)*.

**FM-1 follows adX** sends MIDI clock, start and stop to the device, so its arpeggiator and
sequencer lock to the project when the device's Sync is on. To record what it plays, arm
**● MIDI** on the record toolbar.

The **Effects** tab sets the device's effects by name (filter, distortion, chorus, phaser,
delay, reverb). The FM-1 never reports its own effect settings and does not include them in
its dumps, so adX keeps them, and a knob turned on the device is not seen in adX. **Match
in adX** puts the native `Fm1Effects` chain, with the same settings, on the selected
channel's mixer insert, so a project can move between the device and software in one click.
How close the native chain sounds to the device is a listening judgement *(unverified on a
device)*.

The CC number behind each effect name is in the profile files in
`app/adx/hardware/fm1/profiles/`. They follow adX's reading of M-VAVE's CC page *(unverified
on a device)*. If one is wrong, fix it in that file.

**Keys as pads.** Tick **Use keys as pads** and the 27 keys launch session clips: 8 tracks
by 3 scenes, with the top three keys launching scenes. On V16 the key LEDs show which clips
are playing.

## Firmware

The **Firmware** tab loads a catalog (a JSON manifest; see
`app/adx/hardware/fm1/firmware.py`) and caches it. Each entry shows its source, licence and
risk notes. Closed-source firmwares are links only.

An install:

1. Downloads the package and checks its SHA-256 against the catalog and that it looks like
   an FM-1 image. A package that fails either check is never offered.
2. Asks you to **type the firmware's name**. Clicking OK alone does nothing.
3. Puts the device in update mode and answers its block requests. The device sets the pace.
4. Waits for the device to restart and checks that it reports the new version.

If the device disappears or stops asking for data, adX stops, does not retry, and shows
the recovery steps.

> **Writing to a real FM-1 is off for now.** adX's update protocol is written from the
> community's notes and works against the emulated device. Until it has been checked on a
> real FM-1 (the final review), the install refuses to start on hardware and sends nothing.

**Recovery.** The FM-1 has one flash bank. If its firmware is damaged it starts in its
ROM's recovery mode (`UBOOT1.00`, a USB drive). adX detects that state and never writes in
it. Use a community tool made for that mode, such as fm1_rescue.py, MvaveFM1Unbricker or
M-Flasher, with the official image, and follow that tool's instructions. There is no
verified recovery path. adX keeps a copy of the last package it installed successfully.

## Testing without the device

```
py -3.12 tools/virtual_fm1.py --model FM-1_016 --bank-export-after 5
```

This makes a port named `FM-1 Midi` (it needs teVirtualMIDI, which loopMIDI installs). The
emulated device:

- answers the identity query;
- sends a Bank Export on command;
- keeps the voices and parameter changes it is sent;
- plays a step pattern on clock;
- serves the update protocol, including a disconnect or a failed checksum partway.

`tests/scenarios/test_fm1.py` drives the app against it.
