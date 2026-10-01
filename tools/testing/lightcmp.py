"""Lighting check: our Classic (4:3, N64 resolution) screenshots against the exact N64
emulation (BizHawk Angrylion), lined up (ours 4:3 -> 288x216 at (18,10) in the 320x240
N64 picture). Prints, per moment, how well the pictures match and our brightness as a
share of the original's in its lit parts.
  lightcmp.py EMU_PREFIX RUN t1 t2 ...   (EMU_PREFIX e.g. throne -> bizhawk/shots_intro/throne<t>.png)"""
import sys
import numpy as np
from PIL import Image
prefix, run, ts = sys.argv[1], sys.argv[2], sys.argv[3:]
def ours43(p):
    o = Image.open(p).convert('RGB'); w, h = o.size; o = o.crop((8, 31, w - 8, h - 8)); w, h = o.size
    cw = int(round(h * 4 / 3)); x0 = (w - cw) // 2
    return o.crop((x0, 0, x0 + cw, h))
X, Y, W, H = 18, 10, 288, 216
x0, y0, x1, y1 = 22, 14, 303, 223
for t in ts:
    E = np.asarray(Image.open(f'/mnt/c/ConkerRecompWin/bizhawk/shots_intro/{prefix}{t}.png').convert('RGB')).astype(float)
    O = np.asarray(ours43(f'/mnt/c/ConkerRecompWin/snaps/ab/{run}/t{t}.png').resize((W, H), Image.BILINEAR)).astype(float)
    e = E[y0:y1, x0:x1]; o = O[y0 - Y:y1 - Y, x0 - X:x1 - X]
    c = np.corrcoef(e.mean(2).ravel(), o.mean(2).ravel())[0, 1]
    lit = e.mean(2) > 40
    ratio = o[lit].mean(0) / np.maximum(e[lit].mean(0), 1)
    print(f'{t}: match {c:.2f}  brightness {ratio.mean() * 100:.0f}% (R {ratio[0] * 100:.0f} G {ratio[1] * 100:.0f} B {ratio[2] * 100:.0f})  whole {o.mean() / e.mean() * 100:.0f}%')
