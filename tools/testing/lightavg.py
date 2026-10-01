"""Averaged lighting check: like lightcmp.py, over groups of frames (torches flicker at
random, so single pictures can disagree by chance).
  lightavg.py EMU_PREFIX RUN base1,base2,... frames step"""
import sys
import numpy as np
from PIL import Image
prefix, run, bases, n, step = sys.argv[1], sys.argv[2], [int(b) for b in sys.argv[3].split(',')], int(sys.argv[4]), int(sys.argv[5])
def ours43(p):
    o = Image.open(p).convert('RGB'); w, h = o.size; o = o.crop((8, 31, w - 8, h - 8)); w, h = o.size
    cw = int(round(h * 4 / 3)); x0 = (w - cw) // 2
    return o.crop((x0, 0, x0 + cw, h))
X, Y, W, H = 18, 10, 288, 216
x0, y0, x1, y1 = 22, 14, 303, 223
for b in bases:
    es, os_ = [], []
    for k in range(n):
        t = b + k * step
        E = np.asarray(Image.open(f'/mnt/c/ConkerRecompWin/bizhawk/shots_intro/{prefix}{t}.png').convert('RGB')).astype(float)[y0:y1, x0:x1]
        O = np.asarray(ours43(f'/mnt/c/ConkerRecompWin/snaps/ab/{run}/t{t}.png').resize((W, H), Image.BILINEAR)).astype(float)[y0 - Y:y1 - Y, x0 - X:x1 - X]
        es.append(E.mean()); os_.append(O.mean())
    es, os_ = np.array(es), np.array(os_)
    print(f'{b}: ours/original averaged over {n} frames: {os_.mean() / es.mean() * 100:.0f}%   (single frames range {100 * (os_ / es).min():.0f}-{100 * (os_ / es).max():.0f}%; original varies {100 * es.min() / es.mean():.0f}-{100 * es.max() / es.mean():.0f}% of its mean)')
