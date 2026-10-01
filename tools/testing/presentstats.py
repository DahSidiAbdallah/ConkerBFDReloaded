"""Frame pacing from RT64's present log (fpstest.sh): the gameplay stretch (15 s to 3 s
before the end), frames shown per second, how evenly they're spaced, and how many were
in-between (interpolated) frames."""
import sys
import numpy as np
rows = np.loadtxt(sys.argv[1], dtype=np.int64, ndmin=2)
t = rows[:, 0] / 1e6
sel = (t >= t[-1] - 15) & (t <= t[-1] - 3)
t, place = t[sel], rows[sel, 1]
if len(t) < 10:
    print('too few frames logged'); sys.exit()
dt = np.diff(t) * 1000
fps = (len(t) - 1) / (t[-1] - t[0])
med = np.median(dt)
print(f'frames shown per second: {fps:.1f}')
print(f'time between frames: median {med:.1f} ms, 95% under {np.percentile(dt, 95):.1f} ms, longest {dt.max():.1f} ms')
print(f'hitches (a gap over 1.5x the usual): {(dt > 1.5 * med).sum()} in {t[-1] - t[0]:.0f} s')
big = np.where(dt > 1.5 * med)[0]
if len(big):
    print('hitches at (s into the stretch): ' + ', '.join(f'{t[k] - t[0]:.1f}s ({dt[k]:.0f} ms)' for k in big))
print(f'in-between (interpolated) frames: {(place > 0).mean() * 100:.0f}%')
