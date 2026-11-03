# Driving adX from a test or an agent

adX can be launched the way you launch it and then worked from outside: clicked, typed
into, played, listened to (as numbers), and closed. This is how every software check
that used to need someone at the keyboard is run now (plans/phase_10.5.md). This page
covers launching, the control channel, the driver, the headless commands, and how a
scenario turns "it sounded fine" into an assertion.

The Python examples on this page run in the test suite (`tests/python/test_agent_docs.py`),
so they stay true.

## Launching

```python
from adx.testing.driver import Driver

with Driver.launch(project="tests/golden/single_tone.adx", via="gui") as app:
    app.wait_for("project.open")
    print(app.state()["project"]["channels"])
```

- `via="adX.cmd"` starts the app through the double-click launcher, so the launcher's
  own pathing and environment are covered. `via="gui"` runs `python -m adx gui`.
- `null_audio=True` (the default) renders on a timer instead of a device: what CI uses.
  `null_audio=False` plays on the default device. Point Windows' default output at
  VB-Cable if nothing should come out of the speakers.
- Each launch gets its own settings folder (`ADX_SETTINGS_DIR`), so a driven run never
  touches your layout or recent files.
- Leaving the `with` block quits the app, discarding unsaved work, and waits for the
  process to end.

## The control channel

It is off unless asked for: `adx gui --control`, or `ADX_CONTROL=1` in the environment
(which is how a launch through `adX.cmd` asks). With it on, the app listens on a named
pipe, `adx-control-<pid>-<random>`, that only your user can open, and it writes the
pipe's name and a token to the control file (`--control-file`, or `ADX_CONTROL_FILE`). A
connection that does not present the token is refused. There is no network socket.

The protocol is one JSON object per line. The handshake is
`{"hello": TOKEN, "protocol": 1}`; after it, each request is `{"id": N, "op": ..., ...}`
and each reply is `{"id": N, "ok": true, "result": ...}` or
`{"id": N, "ok": false, "code": ..., "error": ...}` (codes in `adx/control/protocol.py`).
Requests run on the app's main thread, between events, so they see what you would see.

| Request | Does |
|---|---|
| `tree`, `find` | widgets: name, path, class, geometry, visible, enabled, text, value |
| `click`, `double_click`, `drag`, `scroll`, `type`, `key`, `choose` | synthesized input at the widget's coordinates, through the same handlers a mouse or keyboard reaches |
| `context_menu`, `menu` | right-click for a context menu, then click one of its items |
| `drop` | a drag from one widget's drag data onto another, through Qt's drop events |
| `trigger`, `actions` | a menu action by id (`transport.play`, `file.save`, `view.mixer`), and the list of them |
| `state`, `wait_for` | the probes, and a predicate on one of them with a timeout |
| `screenshot` | a widget or the window to PNG; `screen: true` takes it off the screen, so a plugin's native editor shows |
| `capture_start`, `capture_stop` | the master output to a float WAV |
| `open`, `quit`, `answer` | open a project, quit (saving or not), press a dialog's button |
| `script` | code in the scripting API, for setup and readback that has no GUI path |

A request that opens a menu or a dialog is sent with `"async": true`: the app answers at
once and performs it on the next turn of its event loop, so the reply does not wait for
the menu to close. `context_menu` always works this way.

### Addressing widgets

A target is an `objectName`, or a path of names joined by `/` when a name repeats
(`mixer.strip:2/slot:0.button`). Panels name their widgets by what they are
(`plugins.scan`, `channel_rack.row:1`, `param:insert.2.slot.1.gain`); a widget left
unnamed gets one from its panel, its kind and its label (`mixer.button.Mute.0`), the
same on every launch. Item views take an `item` too: the texts of the row to click.

```python
with Driver.launch(via="gui") as app:
    buttons = app.find(cls="QPushButton", text="Scan")
    assert buttons and buttons[0]["name"] == "plugins.scan"
```

## Probes and waiting

`state()` is one dictionary: `transport` (playing, position), `project` (path, dirty,
revision, undo depth, history, a SHA-256 of the project text, channels, patterns,
notes), `status`, `panels_open`, `focus`, `modal`, `popup`, `plugins` (instances, the
known count, whether a scan runs, every notice so far), `midi`, `audio` (null, xruns,
worst callback), `analyzers`, and `control.errors` (failures of async requests).

`wait_for` names a path into it with a predicate (`truthy`, `falsy`, `equals`,
`not_equals`, `gt`, `gte`, `lte`, `contains`), checked in the app every 20 ms until the
timeout. A timeout fails with the last value, so a scenario never needs a sleep:

```python
with Driver.launch(project="tests/golden/single_tone.adx", via="gui") as app:
    app.trigger("transport.play")
    app.wait_for("transport.playing")
    app.wait_for("transport.position_seconds", predicate="gt", value=0.25, timeout=10)
    take = app.capture(1.0, "build/doc-take.wav")
    assert take["frames"] > 0
```

## From a shell

`adx drive` is the same thing one request per call, against an app left running:

```console
$ adx drive launch --via gui --project tests/golden/single_tone.adx
C:\Users\you\AppData\Local\Temp\adx-drive-abc123\control.json
$ adx drive trigger transport.play --control-file C:\...\control.json
$ adx drive wait_for transport.playing timeout=10 --control-file C:\...\control.json
$ adx drive quit save=false --control-file C:\...\control.json
```

The first argument after the op is its target (or its action, path, or keys); the rest
are `key=value` fields, with values read as JSON where they parse.

The other headless commands load no Qt at all, so they run anywhere the engine does:

```console
$ adx analyze key song.wav            # or chords, bpm; a .adx is rendered first
$ adx describe take.wav               # bands, loudness, peak, crest, centroid, f0, envelope
$ adx compare take.wav reference.wav  # exit 1 past a tolerance
$ adx glitch take.wav --reference render.wav   # exit 1 on any finding, with positions
$ adx plugins scan "C:\Program Files\Common Files\VST3"
$ adx plugins probe "C:\...\Dexed.clap"
$ adx midi ports
$ adx midi play loopMIDI riff.txt
```

`adx midi play` sends a timed script (one message a line: `0.5 note_on 1 60 100`) into
an output port. With loopMIDI's port opened as an input in adX, a script stands in for a
pad controller.

## "No glitches" as a number

A listener notices clicks, dropouts and stutter. A scenario checks for the same things:

- **xruns:** `state()["audio"]["xruns"]` before and after. Any increase fails.
- **the capture:** the master output, copied on the audio thread into a ring and drained
  to a WAV on a worker. Capturing allocates nothing on the audio thread.
- **the detector** (`adx.testing.glitch`, or `adx glitch`):
  - a run of exact zeros longer than 1 ms inside sound (a dropout);
  - a block repeated sample for sample (a stuck buffer);
  - an isolated first-difference spike more than 8 times the local level (a click);
  - with a reference render, the residual after alignment, which must stay under
    -90 dBFS outside the edit points a scenario names.

The detector is tested on itself first: each defect is planted in a clean render and
must be found where it was planted, and a clean render must give nothing.

```python
import numpy as np
from adx.testing.glitch import find_glitches

rate = 48000
t = np.arange(rate) / rate
clean = np.stack([0.3 * np.sin(2 * np.pi * 220 * t)] * 2, axis=1).astype(np.float32)
assert find_glitches(clean, rate) == []
broken = clean.copy()
broken[24000] += 0.5
assert [f.kind for f in find_glitches(broken, rate)] == ["click"]
```

## Scenarios

`tests/scenarios/` holds the end-to-end checks: the launcher driven, an external editor
saving while the app plays (P7-1's stand-in), building a track through the GUI (P6-1's),
real plugins (P9-2), a plugin crashing live (P9-3), and the agent's WannaCry against
the reference recording. Run them with:

```console
$ py -3.12 -m pytest tests/scenarios
$ py -3.12 -m pytest tests/scenarios -m "not wannacry and not realplugin"   # what CI runs per push
```

Any scenario that writes music uses the WannaCry corpus, which lives only on this
machine and is never committed (`tests/scenarios/wannacry.py`). Each one also runs on a
committed generated project, so CI exercises the same paths. A WannaCry case that skips
because a file is missing has not run.

The real-plugin scenarios host pinned open-source plugins (`tools/test_plugins.toml`),
fetched and hash-checked by `tools/fetch_test_plugins.py` into a cache outside the
repository. They are test fixtures only: never shipped, linked or committed.

## What this does not cover

- A plugin's own editor is a native window. Synthesized input does not reach inside it;
  it is checked by its window handle and a screenshot.
- The operating system's file dialogs. `open` and `quit(path=...)` do what the dialog
  would once it has a path.
- Listening. Whether something sounds good is a judgement, and it stays with you.
