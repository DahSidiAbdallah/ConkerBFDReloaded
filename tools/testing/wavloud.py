"""Loudness (RMS) of abrun.sh sound recordings over stretches of game time.
  wavloud.py NAME ... [--at from-to,...]"""
import sys, wave
import numpy as np
root = '/mnt/c/ConkerRecompWin/snaps/ab'
args = [a for a in sys.argv[1:] if not a.startswith('--')]
spans = [(46, 54, 'bar menu'), (60, 70, 'gameplay')]
print('%-18s' % '', ''.join('%14s' % s[2] for s in spans))
for name in args:
    w = wave.open(f'{root}/{name}/sound.wav')
    rate = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768
    x = x.reshape(-1, 2)
    row = []
    for a, b, _ in spans:
        seg = x[int(a * rate):int(b * rate)]
        row.append(np.sqrt((seg ** 2).mean()) if len(seg) else float('nan'))
    print('%-18s' % name, ''.join('%14.4f' % v for v in row))
