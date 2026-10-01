"""Lighting of two of our runs over the same moments (tools/testing/abrun.sh), in the 4:3 middle
of the picture both show, split into left / middle / right thirds, averaged over groups of
frames (torches flicker at random, so single pictures disagree by chance).
  widecmp.py RUN_A RUN_B base1,base2,... frames step
Prints A's brightness as a percentage of B's."""
import sys
import numpy as np
from PIL import Image
a, b, bases, n, step = sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3].split(',')], int(sys.argv[4]), int(sys.argv[5])

def middle43(run, t):
    im = np.asarray(Image.open(f'/mnt/c/ConkerRecompWin/snaps/ab/{run}/t{t}.png').convert('RGB')).astype(float)
    h, w, _ = im.shape
    cw = int(round(h * 4 / 3)); x0 = (w - cw) // 2
    return im[:, x0:x0 + cw]

for base in bases:
    sums = {k: [0.0, 0.0] for k in ('left', 'middle', 'right', 'all')}
    for k in range(n):
        t = base + k * step
        A, B = middle43(a, t), middle43(b, t)
        third = A.shape[1] // 3
        parts = {'left': slice(0, third), 'middle': slice(third, 2 * third), 'right': slice(2 * third, None), 'all': slice(None)}
        for name, sl in parts.items():
            sums[name][0] += A[:, sl].mean(); sums[name][1] += B[:, sl].mean()
    print(f'{base}: ' + '  '.join(f'{name} {100 * s[0] / s[1]:.0f}%' for name, s in sums.items()))
