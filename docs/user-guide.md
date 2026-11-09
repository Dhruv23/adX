# adX user guide

From installing adX to a finished, exported track. The panels are covered in the order
you would reach for them; the commands and scripts on this page are run by the test
suite (`tests/python/test_docs_examples.py`), so they work as written.

## 1. Install and open

**From the installer.** Run `adX-<version>.msi`. It installs for you alone (no
administrator prompt), adds adX to the Start menu and opens `.adx` files with it.
Uninstalling removes adX and leaves your projects and your own presets where they are.

**From a checkout.** With Python 3.12 and Visual Studio's C++ tools:

```console
$ py -3.12 -m pip install -e .[dev]
```

then double-click `adX.cmd` in the repository's folder, or drop a `.adx` file on it.

The first time adX opens it shows a short quick start (Help > Quick start brings it
back). Edit > Preferences chooses the audio device, buffer size and MIDI inputs; the
status bar shows the latency you get.

## 2. A project is a text file

Everything you make is saved in one `.adx` file you can read, diff and edit in any
editor. adX watches the file: save it from VS Code and the change is in the app on the
next keystroke, as one undo step. The format is in [adx-format-v2.md](adx-format-v2.md).

The command line reads the same files without opening the app:

```console
$ adx fmt --upgrade docs/examples/c418_demo.adx -o build/guide/c418.adx
$ adx fmt --upgrade docs/examples/stakillaz_demo.adx -o build/guide/stakillaz.adx
$ adx info build/guide/c418.adx
$ adx validate build/guide/c418.adx
$ adx fmt --check build/guide/c418.adx
$ adx diff build/guide/c418.adx build/guide/stakillaz.adx
```

The examples are in the original (v1) format, which adX reads but never writes over:
`--upgrade` writes a v2 copy, with a report of what changed. `validate` exits 1 on an
error and `diff` exits 1 when the projects differ, so both work in a script or a
pre-commit hook.

## 3. Sounds: the channel rack

The **Channel rack** (F6) holds one row per instrument. `+ Channel` adds one with the
instrument chosen beside it; double-click a name to rename it; the instrument button
opens it in the **Instrument** panel, with every parameter as a knob. The **Browser**
(F8) lists presets; drag one onto a channel to load it, or onto a mixer slot for an
effect.

The rack's step grid writes a pattern's drums: click a step to set it, right-click to
clear it. `+ Pattern` makes a new pattern; the box beside it chooses which one the steps
edit.

## 4. Notes: the piano roll

The **Piano roll** (F7) edits one channel's notes in one pattern, chosen in its two
boxes. Draw (1) clicks a note in and drags its length; Select (3) boxes notes, and a
drag moves what is selected. The snap grid is in the second row of the
toolbar, along with Quantize, Humanize and the scale highlight. Notes drawn past the end
of the pattern lengthen it to the next bar.

Cut (0) trims a row: click a note and every note on that pitch from the start of that
bar on is deleted, and a note crossing the bar line is shortened to end on it. Slice (4)
splits and keeps both halves; Cut keeps nothing after the bar.

The scale box has 31 scales. Pick **Custom…** at the bottom to make your own: toggle
the steps above the root, name it, and save; it is listed above Custom… from then on.
**Lock**, next to the scale box, moves every note in the pattern onto the scale when you
turn it on and whenever the scale or root changes, as one undo step. With Lock off a
scale only colours the rows and snaps notes as you draw them.

The **Inspector** shows exact values for whatever is selected: type a velocity there and
it applies to every selected note.

The same notes can be written in a script (the **Console**, View > Console):

```python
p = adx.project()
with p.undo_group("a bass line"):
    bass = p.add_channel("Bass", instrument="va")
    part = p.add_pattern("Bassline", bars=4)
    part.notes(bass).add_mini("0 3 5 7 | scale=a_minor | fast 2")
    p.playlist.track(1).place(part, at="0:0:0")
```

or as mini-notation inside the file itself ([mininotation.md](mininotation.md)).

## 5. Arranging: the playlist

The **Playlist** (F5) is the song: drag a pattern from the browser onto a track to place
it, drag a placement to move it, drag its right edge to repeat it. Audio files dropped
on a track become audio clips, which can be stretched, warped and faded. Markers (Insert
> Marker) name sections, and the export can stop and start at them.

Space plays and pauses from the playhead; Ctrl+Space stops in place.

## 6. Mixing: the mixer

Each channel feeds an insert of the **Mixer** (F9): its fader, pan, mute, solo, eight
effect slots, and sends. Click an empty slot to add an effect; click a full one to open
it in the **Effect** panel. Right-click a knob anywhere for *Create automation*, which
puts a lane for it on the playlist.

Plugins (VST3 and CLAP) are scanned in the **Plugins** panel. Each runs in a separate
process, so a plugin that crashes leaves the rest of the song playing; Edit > Restart
crashed plugins brings it back with its last state. Mark a plugin *isolated* to give it a
process of its own.

## 7. Analysis and generation

The **Analysis** panel (View > Analysis) finds a song's key and chords, rebuilds a
timbre from a recording, and tidies a part into a cleaner form; every result it applies
is one undo step. **Generate** writes a whole track in a style (C418, STAKILLAZ) from a
seed, the same track every time for the same seed. The six analyzers (spectrum,
spectrogram, scope, vectorscope, loudness, particles) are under View and cost nothing
while closed.

```python
p = adx.project()
print(p.channels())
```

## 8. Export

File > Export (Ctrl+E) renders WAV (16, 24 or 32-bit float), FLAC, MP3 or Ogg, the
whole song, the loop or a range between markers, with stems by channel, insert or
pattern, and loudness normalization. The same export runs without the app:

```console
$ adx render build/guide/c418.adx -o build/guide/c418.wav --format wav24 --tail 2s
$ adx render build/guide/c418.adx -o build/guide/c418.flac --normalize -14LUFS
$ adx render build/guide/c418.adx -o build/guide/stems.wav --stems channels --stems-only
```

An export of the same project is the same bytes every time: renders are exact per
block, so a render on another machine matches yours.

## 9. Keys

The default keymap is in [keyboard.md](keyboard.md). To change a key, open
**Preferences > Shortcuts**: search for the action, click its shortcut field, press the
new key. A clash with another action in the same place is marked; Reset puts one key
back and Reset all puts back every key.
