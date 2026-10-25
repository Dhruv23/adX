"""Build a riff: a channel, a pattern written in mini-notation, placed and given reverb.

The example from phase_7.md 4.7, as a script. ``add_mini`` compiles the pattern into
ordinary notes - degrees 0 3 5 7 of A minor, twice a bar - so the piano roll can edit
them afterwards; one bar of it is eight notes, eight bars sixty-four.
"""

from adx.scripting import api as adx

p = adx.project()

with p.undo_group("build a riff"):
    lead = p.add_channel("Lead", instrument="va")
    verse = p.add_pattern("Verse", bars=8)
    added = verse.notes(lead).add_mini("0 3 5 7 | scale=a_minor | fast 2")
    p.playlist.track(1).place(verse, at="0:0:0")
    p.mixer.insert(1).add_effect("Reverb", mix=0.3, room=0.6)

assert added == 64
assert verse.notes(lead).count() == 64
assert "Reverb mix=0.3" in p.text()
assert "PATTERN Verse 0:0:0" in p.text()
