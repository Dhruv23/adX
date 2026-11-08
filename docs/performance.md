# Performing a set

This is how to set up a pad controller and play a set live in adX: launching clips
from the session, firing scenes, nudging and tapping the tempo, and recording the
performance back into the arrangement.

Every step below is followed by `tests/scenarios/test_performance_walkthrough.py`
against the emulated controller (`tools/virtual_pad.py`), so what each step says you
should see is checked on every run.

## 1. Open the session

**View → Session** opens the session dock beside the playlist. Scenes run down the
grid and tracks run across it. Each track plays one channel, and each cell can hold one
clip.

1. Click **+ Track** and choose a channel. A column appears under its name.
2. Click **+ Scene** for each row you want. The scene buttons appear on the right as
   `▶ Scene 1`, `▶ Scene 2` and so on.
3. Right-click a cell and choose **Put pattern → *name***. The cell now shows the
   pattern's name. The clip plays that pattern's notes for the track's channel only.

A cell's right-click menu also sets:

- **Launch quantize**: where a launch lands. `global` uses the toolbar's setting;
  `instant` starts on the next sample; the others are 1/4, 1/2, a bar, or 2, 4 or 8
  bars.
- **Follow action**: what happens when a pass ends: `next`, `previous`, `first`,
  `random`, `repeat`, `stop`, or `none`, which just loops.
- **Loop or Play once**: whether the clip loops.
- **Legato**: a relaunch carries on in phase instead of restarting.

The whole session is saved in the project as a `[SESSION]` section, so you can also
write a set in a text editor:

```
[SESSION]
QUANTIZE=bar
SCENE "Intro"
SCENE "Drop" tempo=128
TRACK Lead linked=1
  1 Verse
  2 Hook follow=next repeats=2
TRACK Bass linked=2
  1 Bassline gain=0.8
```

Every edit in the dock is one undo step.

## 2. Launch from the screen

- Click a cell to launch its clip. The cell turns **amber** while it waits for its
  boundary, then **green** while it plays. A ring at its right fills as the clip moves
  through its loop.
- Click an empty cell, or a track's **■**, to stop that track on the playing clip's
  boundary. The cell turns **orange-red** until then.
- Click a scene's **▶** to launch every track's cell in that row on the same sample.
  A track whose cell in that row is empty stops.
- **■ Stop all** stops every clip on its boundary.

A stopped clip is released, not cut. Its notes ring out through their release, and
reverbs and delays on its channel keep their tails.

## 3. Connect a pad controller

1. Plug the controller in and turn on its input under **Preferences → MIDI**.
2. If adX ships a profile for it (Launchpad Mini MK3, APC mini), the input becomes a
   pad controller on its own. The status bar says `<port>: pads (<profile>)`, and the
   pads light: dim for a stopped clip, dark for an empty cell.
3. Otherwise click **Pads…** in the session toolbar. Choose the input, pick a profile,
   and click **Use as pads**.

The grid's columns are tracks and its rows are scenes, from the top left. Pads launch
cells, scene buttons fire rows, and track buttons stop columns. Pressing harder plays
louder when the profile is velocity sensitive. A pad's light follows its cell: stopped,
waiting (amber, blinking), playing (green), stopping (red, blinking). The lights are
sent from a thread of their own at 30 Hz, so they keep up while a dialog is open.

### Learn mode, for a controller without a profile

1. In **Pads…**, choose the input and set the grid's size, for example 8 x 8.
2. Click **Learn…**. The dialog asks for each pad in turn (`Press pad 1, 1`, then
   `Press pad 2, 1` and so on), row by row from the top left.
3. Then it asks for each scene button, for stop all, and for tap tempo. Click **Skip**
   for any your controller doesn't have. Skipping the first scene button skips them
   all.
4. When it's done, the dialog says `Learned: saved as <name>.adxpad` and the
   controller is in use. The profile is saved in your settings folder under `pads/`,
   and you can edit it there. Its format is described in
   `engine/midi/PadController.h`.

## 4. Tempo, live

- **Tap** on the transport bar, or the profile's tap button, sets the tempo every clip
  runs at from your last four taps. A tap far off the others is ignored.
- Hold **◀◀** or **▶▶** to slow everything down or speed it up by 4 %. When you let
  go, it returns exactly to where it was.
- A scene with a tempo (`SCENE "Drop" tempo=128`) takes that tempo over on the global
  quantize's boundary when you fire it.

The rate everything runs at is shown beside the nudge buttons, for example `x1.040`.

## 5. Record the performance

1. Click **● Perform** on the transport bar before you start.
2. Play the set.
3. Click **● Perform** again to stop. The status bar says
   `Performance recorded: N clip(s), M tempo change(s)`.

The recording is an ordinary arrangement. Each pass of each clip becomes a pattern
placement on the session track's linked playlist track, and each nudge, tap or scene
tempo becomes a tempo change. A pattern that also holds other channels' notes is
written as a copy holding only the track's channel. The whole recording is one undo
step, and it plays back like what you heard.

## 6. Before a show

- Open the set and launch every clip once.
- Watch the session counters: if a launch is ever refused because every clip slot is
  in use (128 clips sounding or releasing at once), the status bar says so.
- **Preferences → Audio**: a 256-frame buffer at 48 kHz is the tested setup.
