#!/usr/bin/env python3
"""Convert a MIDI file to a synthp pdemo_ev table (3-voice reduction).

Note-ons from channel 0 are quantized to the 80ms demo grid, up to three
per strike (bass + the two highest); durations come from onset spacing
and the preset's ring supplies the sustain.  Long silences emit PD_DAMP.

The top note of a strike is marked PD_V ("vocal"/melody, treble clef)
when its real MIDI duration is VOCAL_MIN_S or longer: the engine then
protects that voice from accompaniment stealing so the melody holds its
full length even through busy passages.

Paste the output into synth.c and add it to pdemo_songs[].

Needs mido (the faceswap conda python has it).
Usage: mid2pdemo.py <file.mid> <table_name> [channel]
"""
import sys
import mido

if len(sys.argv) < 3:
    sys.exit(__doc__)
path, name = sys.argv[1], sys.argv[2]
chan = int(sys.argv[3]) if len(sys.argv) > 3 else 0

m = mido.MidiFile(path)
evs, active, t = [], {}, 0.0
for msg in m:
    t += msg.time
    if msg.type == 'note_on' and msg.velocity > 0 and msg.channel == chan:
        active[msg.note] = t
    elif (msg.type == 'note_off' or
          (msg.type == 'note_on' and msg.velocity == 0)) \
            and msg.channel == chan and msg.note in active:
        evs.append((active.pop(msg.note), t, msg.note))
evs.sort()
if not evs:
    sys.exit(f"no notes on channel {chan}")

STEP = 0.08
VOCAL_MIN_S = 0.5                       # melody notes this long get PD_V
groups = {}
for t0, t1, note in evs:
    n = note - 12                       # MIDI 24 = our C1
    while n < 12: n += 12
    while n > 83: n -= 12
    g = groups.setdefault(round(t0 / STEP), {})
    g[n] = max(g.get(n, 0.0), t1 - t0)  # keep the longest duration

steps = sorted(groups)
names = ["nC", "nCs", "nD", "nDs", "nE", "nF", "nFs", "nG", "nGs",
         "nA", "nAs", "nB"]
def nm(n): return f"{names[n % 12]}({n // 12})"

vocal_count = 0
print(f"// '{name}' - converted from {path} (channel {chan}) by mid2pdemo.py")
print(f"// PD_V marks melody notes held >= {VOCAL_MIN_S}s in the MIDI")
print(f"static const struct pdemo_ev pdemo_{name}[] = {{")
for i, st in enumerate(steps):
    ns = sorted(groups[st])
    if len(ns) > 3:
        ns = [ns[0]] + ns[-2:]
    gap = (steps[i + 1] - st) if i + 1 < len(steps) else 24
    top = ns[-1]
    parts = []
    for n in ns:
        if n == top and groups[st][n] >= VOCAL_MIN_S:
            parts.append(f"PD_V|{nm(n)}")
            vocal_count += 1
        else:
            parts.append(nm(n))
    chord = ", ".join(parts) + ", 0" * (3 - len(ns))
    if gap > 25:
        print(f"    {{ {{{chord}}}, 20 }},")
        print(f"    {{ {{PD_DAMP, 0, 0}}, {min(gap - 20, 255)} }},")
    else:
        print(f"    {{ {{{chord}}}, {min(gap, 255)} }},")
print("    { {PD_DAMP, 0, 0}, 12 },")
print("    { {0, 0, 0}, 0 },")
print("};")
print(f"// {len(steps)} strikes ({vocal_count} vocal), "
      f"~{steps[-1] * STEP:.0f}s", file=sys.stderr)
