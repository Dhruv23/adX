"""Transpose a pattern: read its notes, write them back a fifth higher.

Notes cross into Python as one numpy array, never one object per note (FINAL_PLAN
2.2) - so a script reads a clip with ``p.native.notes(pattern, channel)`` and works on
the array.
"""

from adx.scripting import api as adx

p = adx.project()

with p.undo_group("transpose up a fifth"):
    lead = p.add_channel("Lead")
    riff = p.add_pattern("Riff", bars=1)
    notes = riff.notes(lead)
    for step, pitch in enumerate((60, 62, 64, 67)):
        notes.add(pitch, at=f"0:{step}:0", length="0:1:0")
    before = p.native.notes("Riff", "Lead")
    notes.clear()
    for row in before:
        notes.add(int(row["pitch"]) + 7, at=int(row["start"]), length=int(row["length"]))

after = sorted(int(n) for n in p.native.notes("Riff", "Lead")["pitch"])
assert after == [67, 69, 71, 74]
