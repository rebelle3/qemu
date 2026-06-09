#!/usr/bin/env python3
"""Generate a few short PCM WAV tracks for the iPod sample filesystem."""
import math, os, struct, sys, wave

OUT = sys.argv[1] if len(sys.argv) > 1 else "music"
os.makedirs(OUT, exist_ok=True)
RATE = 44100

def tone(path, freq, secs, title):
    with wave.open(path, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(RATE)
        frames = bytearray()
        for n in range(int(RATE * secs)):
            env = min(1.0, n / (RATE * 0.05), (RATE * secs - n) / (RATE * 0.05))
            s = int(12000 * env * math.sin(2 * math.pi * freq * n / RATE))
            frames += struct.pack("<hh", s, s)
        w.writeframes(frames)
    print("wrote", path)

tracks = [("01 - Sine A440.wav", 440), ("02 - Sine C512.wav", 512),
          ("03 - Sine E660.wav", 660)]
for name, f in tracks:
    tone(os.path.join(OUT, name), f, 6, name)
