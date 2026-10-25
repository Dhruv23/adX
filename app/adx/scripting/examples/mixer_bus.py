"""A bus: two channels into one new insert, with a chain on it, into the master.

Inserts are numbered the way the file numbers them; ``add_insert`` returns the new one,
already routed to the master.
"""

from adx.scripting import api as adx

p = adx.project()

with p.undo_group("drum bus"):
    bus = p.mixer.add_insert("Drums")
    for name in ("Kick", "Snare"):
        channel = p.add_channel(name, instrument="drumsynth")
        channel.route(bus.id)
        channel.set(volume=0.8)
    bus.add_effect("Compressor", threshold=-18.0, ratio=4.0)
    bus.add_effect("Saturation", mix=0.5)
    bus.set(gain=0.9)

text = p.text()
assert f'INSERT {bus.id} name="Drums" gain=0.9' in text
assert f"ROUTE insert.{bus.id} -> insert.1" in text
assert text.count(f"OUTPUT=insert.{bus.id}") == 2
