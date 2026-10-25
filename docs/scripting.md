# Scripting

adX has a Python API over the same commands the GUI uses. In the app it is the
**Console** panel (View > Console): a Python prompt with `adx` already imported and
attached to the open project. A script can also run outside the app, on a project of its
own (§7).

```python
p = adx.project()
with p.undo_group("build a riff"):
    lead = p.add_channel("Lead", instrument="va")
    verse = p.add_pattern("Verse", bars=8)
    verse.notes(lead).add_mini("0 3 5 7 | scale=a_minor | fast 2")
    p.playlist.track(1).place(verse, at="0:0:0")
    p.mixer.insert(1).add_effect("Reverb", mix=0.3)
```

**Every Python block in this document is executed by the test suite**, in order, on a
fresh project (`tests/python/test_docs_examples.py`), and so is every script in
`app/adx/scripting/examples/` (`tests/python/test_scripting.py`). Documentation that
stops working fails CI.

---

## 1. The rules

1. **Everything a script does is a command** — the same commands a panel issues. There
   is no other way in, so a script, a knob and a line of text can never disagree
   (FINAL_PLAN §4).
2. **Everything a script does can be undone.** Wrap a block in `undo_group` and one Undo
   takes all of it back.
3. **Scripts run on the main thread.** Nothing a script does touches the audio thread;
   `on_bar` callbacks run from a timer on the main thread (§6).
4. **In the app, the panels follow.** A script's edit refreshes every panel and is heard
   on the next block, exactly as if it had been made by hand.

Positions and durations are written the way the file writes them — `"bar:beat:tick"`,
from `"0:0:0"` — and a duration in the same spelling: `"2:0:0"` is two bars. Where a
number of ticks is easier, a note's position and length may be given as an integer
(3840 ticks to a quarter note).

## 2. The project

| | |
|---|---|
| `adx.project()` | the open project |
| `p.text()` | its canonical `.adx` text |
| `with p.undo_group(label):` | everything inside is one undo step |
| `p.undo()`, `p.redo()` | one step |
| `p.channels()`, `p.patterns()` | names |
| `p.channel(name)`, `p.pattern(name)` | an existing one (`ScriptError` if not) |
| `p.add_channel(name, instrument="additive", preset=None, output=0)` | a new channel |
| `p.add_pattern(name, bars=4, length=None)` | a new pattern, `bars` long or `length` long |
| `p.set_tempo(bpm, at="0:0:0")` | a tempo change |
| `p.add_marker(name, at="0:0:0")` | a marker |
| `p.position(text)`, `p.duration(text)` | a position or duration in ticks |

```python
p = adx.project()
with p.undo_group("set up"):
    p.set_tempo(128)
    bass = p.add_channel("Bass", instrument="va")
    p.add_marker("Drop", at="8:0:0")
assert "0:0:0 128" in p.text()
assert p.position("1:0:0") == 4 * 3840
```

## 3. Channels

| | |
|---|---|
| `ch.set(volume=, pan=, pitch=, mute=, solo=, polyphony=)` | strip values, one undo step |
| `ch.param(name, value)` | an instrument parameter: `ch.param("filter.cutoff", 1200)` |
| `ch.instrument(type)` | change the instrument |
| `ch.route(insert)` | send the channel to a mixer insert |
| `ch.info()` | everything the inspector shows |

```python
bass = p.channel("Bass")
bass.set(volume=0.7, pan=-0.2)
bass.param("filter.cutoff", 900)
assert abs(bass.info()["volume"] - 0.7) < 1e-6
```

Changing a sounding channel's instrument releases its voices — they ring out — while
new notes play on the new one.

## 4. Patterns and notes

| | |
|---|---|
| `pat.notes(channel)` | one channel's notes in the pattern |
| `notes.add(pitch, at, length="0:1:0", velocity=100)` | one note |
| `notes.add_mini(text)` | mini-notation compiled into ordinary notes (returns how many) |
| `notes.clear()`, `notes.count()` | |
| `pat.mini(channel, text)` | a **live** `MINI` block (empty text removes it) |
| `pat.length` | in ticks |

`add_mini` and `mini` are different things. `add_mini` writes notes, once — the piano
roll can edit them afterwards, and the pattern's cycles count from its own start.
`mini` writes a `MINI` block that stays mini-notation: the engine compiles it as it plays,
one cycle per bar of the **song**, and editing the text changes what plays. The grammar
is [mininotation.md](mininotation.md).

```python
groove = p.add_pattern("Groove", bars=2)
assert groove.notes(bass).add_mini("c2 ~ c2 g1") == 6
groove.notes(bass).add(48, at="1:3:0", length="0:0:1920", velocity=90)
assert groove.notes(bass).count() == 7
groove.mini(bass, "")  # no live block for the bass
```

Reading notes: a clip crosses into Python as one numpy array, never as an object per
note (FINAL_PLAN §2.2) — `p.native.notes(pattern, channel)`, with fields `id`, `start`,
`length`, `pitch`, `velocity`, `muted` and the rest.

```python
rows = p.native.notes("Groove", "Bass")
assert sorted(int(x) for x in rows["pitch"])[:2] == [31, 31]  # g1
```

## 5. Playlist and mixer

| | |
|---|---|
| `p.playlist.track(n)` | track `n` (numbered as the file numbers them), created if missing |
| `track.place(pattern, at="0:0:0", length=None)` | a placement |
| `p.mixer.insert(n)` | an existing insert |
| `p.mixer.add_insert(name, to=0)` | a new insert, routed to `to` (0: the master) |
| `insert.add_effect(type, mix=1.0, **params)` | an effect at the end of the chain; returns its slot id |
| `insert.set(gain=, pan=, width=, mute=, solo=, invert=)` | strip values |

```python
p.playlist.track(2).place("Groove", at="0:0:0")
p.playlist.track(2).place("Groove", at="2:0:0")
bus = p.mixer.add_insert("Low end")
bass.route(bus.id)
slot = bus.add_effect("Compressor", threshold=-20.0, ratio=3.0)
assert f"SLOT {slot} Compressor" in p.text()
```

## 6. Playing, and bar callbacks

| | |
|---|---|
| `adx.transport.play()`, `.stop()`, `.seek(at)`, `.position()` | the transport |
| `adx.on_bar(callback)` | `callback(bar)` each time playback enters a bar (0-based) |
| `adx.off_bar(callback)` | stop calling it |

`on_bar` is for live coding: change a pattern every few bars, say. Callbacks run on the
main thread, from the 60 Hz frame clock in the app (a timer of their own outside it) —
never from the audio callback — so they may do anything a script may do.

```python
def every_fourth(bar):
    if bar % 4 == 3:
        p.pattern("Groove").notes(bass).add(55, at="0:0:0")

adx.on_bar(every_fourth)
adx.off_bar(every_fourth)
```

## 7. Outside the app

`adx.new(text=None)` gives the module a project of its own (one master insert, unless
`text` is `.adx` text), and `adx.open(path)` opens a file. A script run with
`python -c` or from a test imports the module as `from adx.scripting import api as adx`;
the console and the example runner provide it as `adx`. Save with
`p.native.save(path)`.

```python
q = adx.new()
assert q.channels() == []
assert "INSERT 1 name=\"Master\"" in q.text()
```

## 8. Examples

`app/adx/scripting/examples/` holds scripts that are documentation and tests at once:
`build_a_riff.py`, `live_drums.py`, `mixer_bus.py`, `transpose.py`. Each runs against
an open project, does its work in one undo group, and asserts what it documents. To run
one in the console: `exec(open(path).read())`.
