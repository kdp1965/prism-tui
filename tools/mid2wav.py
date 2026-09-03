#!/usr/bin/env python3
"""Render a MIDI file to a 31250Hz mono WAV with a small software synth,
ready for mp3_to_dsm.py --format adpcm.

Channel timbres: even channels get a decaying piano-ish additive tone,
odd channels a detuned sustained pad.  Full polyphony and note durations
are honoured - this is the high-fidelity path, vs the 3-voice live
reduction of mid2pdemo.py.

Needs mido + numpy.  Usage: mid2wav.py <file.mid> <out.wav>
"""
import sys
import mido
import numpy as np
import wave

if len(sys.argv) != 3:
    sys.exit(__doc__)

FS = 31250
m = mido.MidiFile(sys.argv[1])
evs, active, t = [], {}, 0.0
for msg in m:
    t += msg.time
    if msg.type == 'note_on' and msg.velocity > 0:
        active[(msg.channel, msg.note)] = (t, msg.velocity)
    elif msg.type == 'note_off' or (msg.type == 'note_on' and msg.velocity == 0):
        k = (msg.channel, msg.note)
        if k in active:
            t0, v = active.pop(k)
            evs.append((t0, t, msg.note, v, msg.channel))
if not evs:
    sys.exit("no notes found")

dur = max(e[1] for e in evs) + 2.0
mix = np.zeros(int(dur * FS))

for t0, t1, note, vel, ch in evs:
    f = 440.0 * 2 ** ((note - 69) / 12)
    length = t1 - t0
    rel = 0.35 if ch % 2 else 0.25
    ns = int((length + rel) * FS)
    tt = np.arange(ns) / FS
    x = np.zeros(ns)
    if ch % 2 == 0:                     # piano-ish
        for h in range(1, 9):
            if f * h > FS * 0.45:
                break
            x += np.sin(2 * np.pi * f * h * tt) / (h ** 1.5)
        env = np.exp(-tt / 0.9) * np.minimum(tt / 0.004, 1.0)
        env[tt > length] *= np.exp(-(tt[tt > length] - length) / 0.08)
        amp = 1.0
    else:                               # sustained pad
        for d in (-0.15, 0.15):
            fd = f * 2 ** (d / 100)
            for h in (1, 2, 3):
                x += np.sin(2 * np.pi * fd * h * tt) / (h ** 2) * 0.5
        env = np.minimum(tt / 0.15, 1.0)
        env[tt > length] *= np.exp(-(tt[tt > length] - length) / rel)
        amp = 0.5
    i0 = int(t0 * FS)
    end = min(i0 + ns, len(mix))
    mix[i0:end] += (x * env * (vel / 127.0) * amp)[:end - i0]

mix /= np.abs(mix).max() * 1.05
with wave.open(sys.argv[2], "w") as fo:
    fo.setnchannels(1)
    fo.setsampwidth(2)
    fo.setframerate(FS)
    fo.writeframes((mix * 32767).astype("<i2").tobytes())
print(f"rendered {dur:.1f}s ({len(evs)} notes) -> {sys.argv[2]}")
