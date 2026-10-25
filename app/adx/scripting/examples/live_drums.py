"""Live drums: a MINI block rather than notes.

``pattern.mini(channel, text)`` writes a ``MINI`` block into the project. It stays
mini-notation - the engine compiles it as it plays, a cycle per bar of the song - so
editing the text in the editor panel or in another editor changes what plays at once.
"""

from adx.scripting import api as adx

p = adx.project()

with p.undo_group("live drums"):
    kit = p.add_channel("Kit", instrument="drumsynth")
    beat = p.add_pattern("Beat", bars=4)
    beat.mini(kit, "bd*2 [~ sn] hh(3,8), <~ cp>")
    p.playlist.track(1).place(beat, at="0:0:0")
    p.playlist.track(1).place(beat, at="4:0:0")

assert "MINI Kit\n  bd*2 [~ sn] hh(3,8), <~ cp>" in p.text()
assert beat.notes(kit).count() == 0  # nothing compiled into the pattern: it is live
