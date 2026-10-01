"""Work out which 2D textures are pieces of one picture (the game draws big pictures, like the
menu buttons, as 32x32 pieces side by side), from where they're drawn.
  tile_layout.py DUMP_DIR ORIGINALS_DIR... OUT.json [--memory SCAN_DIR]...
DUMP_DIR/rect_places.txt (RT64_DUMP_TEXTURES) lists each texture's places: frame, picture drawn
into, rectangle (quarter pixels). Pieces drawn edge to edge at their own size in a frame are
neighbours. A piece can have several possible neighbours on a side (a button's normal and glowing
pieces are drawn at the same place): they're paired up the best continuing pairs first. Writes the
pictures: [{"size": [w, h], "tiles": [[hash, x, y], ...]}], each piece in one only (pieces
without neighbours are left out).
--memory: pieces found in memory (scan_rdram.py) but never seen drawn: the game keeps a picture's
pieces one after another, left to right (16 bytes apart), so those are right-hand neighbours
if their edges continue."""
import collections
import json
import os
import sys

import numpy as np
from PIL import Image

SIDES = {'R': (1, 0), 'L': (-1, 0), 'D': (0, 1), 'U': (0, -1)}
OPPOSITE = {'R': 'L', 'L': 'R', 'D': 'U', 'U': 'D'}


def main():
    argv = sys.argv[1:]
    memory = []
    while '--memory' in argv:
        memory.append(argv[argv.index('--memory') + 1])
        del argv[argv.index('--memory'):argv.index('--memory') + 2]
    dump, orig_dirs, out = argv[0], argv[1:-1], argv[-1]
    index, paths = {}, {}
    for d in orig_dirs:
        for h, v in json.load(open(os.path.join(d, 'index.json'))).items():
            index[h] = v
            paths[h] = os.path.join(d, h + '.png')
    frames = collections.defaultdict(set)
    drawn = set()
    for line in open(os.path.join(dump, 'rect_places.txt')):
        frame, h, fb, _, ulx, uly, lrx, lry = line.split()
        drawn.add(h)
        if h not in index:
            continue
        w, hh = index[h]['width'], index[h]['height']
        if w < 16 or hh < 16 or (int(lrx) - int(ulx)) != w * 4 or (int(lry) - int(uly)) != hh * 4:
            continue # small pieces (letters) and pieces drawn scaled
        frames[(frame, fb)].add((h, int(ulx) // 4, int(uly) // 4, w, hh))
    neighbours = collections.defaultdict(set) # (hash, side) -> hashes
    for rects in frames.values():
        for a in rects:
            for b in rects:
                if a[1] + a[3] == b[1] and a[2] == b[2] and a[4] == b[4]:
                    neighbours[(a[0], 'R')].add(b[0]); neighbours[(b[0], 'L')].add(a[0])
                if a[2] + a[4] == b[2] and a[1] == b[1] and a[3] == b[3]:
                    neighbours[(a[0], 'D')].add(b[0]); neighbours[(b[0], 'U')].add(a[0])
    memory_pairs = [] # (a, b): b right after a in memory
    spots = []
    for folder in memory:
        for n in os.listdir(folder):
            if n.endswith('.rice.json'):
                j = json.load(open(os.path.join(folder, n)))
                h = n.split('.')[0]
                if h in index and 'scanName' in j:
                    size = os.path.getsize(os.path.join(folder, n[:-len('.json')] + '.rdram'))
                    spots.append((folder + '/' + j['scanName'].split('_')[0], j['texture']['address'], size, h))
    spots.sort()
    for (m1, a1, s1, h1), (m2, a2, s2, h2) in zip(spots, spots[1:]):
        # (16 bytes apart in memory; in the ROM's files right after one another, or the same.)
        if m1 == m2 and a2 - (a1 + s1) in (0, 16) and h1 not in drawn and h2 not in drawn \
                and (index[h1]['width'], index[h1]['height']) == (index[h2]['width'], index[h2]['height']):
            memory_pairs.append((h1, h2))
    images = {}

    def image(h):
        if h not in images:
            im = np.asarray(Image.open(paths[h]).convert('RGBA')).astype(np.float32)
            images[h] = np.dstack([im[..., :3] * im[..., 3:] / 255, im[..., 3]]) # premultiplied
        return images[h]

    def edge_gap(a, side, b):
        """How badly b continues a on that side (the mean difference across the join)."""
        ia, ib = image(a), image(b)
        ea, eb = {'R': (ia[:, -1], ib[:, 0]), 'L': (ia[:, 0], ib[:, -1]),
                  'D': (ia[-1], ib[0]), 'U': (ia[0], ib[-1])}[side]
        return float(np.abs(ea - eb).mean()) if ea.shape == eb.shape else 1e9

    # Pair the pieces up, the best continuing pairs first, each piece with one on each side.
    pairs = {}
    candidates = sorted((edge_gap(a, side, b), a, side, b) for (a, side), bs in neighbours.items()
                        if side in ('R', 'D') for b in bs)
    for _, a, side, b in candidates:
        if (a, side) not in pairs and (b, OPPOSITE[side]) not in pairs:
            pairs[(a, side)] = b
            pairs[(b, OPPOSITE[side])] = a
    for a, b in memory_pairs:
        if (a, 'R') not in pairs and (b, 'L') not in pairs and edge_gap(a, 'R', b) < 25:
            pairs[(a, 'R')] = b
            pairs[(b, 'L')] = a

    def best(a, side):
        return pairs.get((a, side))

    def picture(start):
        placed, queue = {start: (0, 0)}, [start]
        while queue:
            a = queue.pop(0)
            for side, (dx, dy) in SIDES.items():
                b = best(a, side)
                if b is None or b in placed:
                    continue
                x, y = placed[a]
                w, h = index[a]['width'], index[a]['height']
                bw, bh = index[b]['width'], index[b]['height']
                pos = (x + (w if dx > 0 else -bw if dx < 0 else 0), y + (h if dy > 0 else -bh if dy < 0 else 0))
                if pos in placed.values():
                    continue
                placed[b] = pos
                queue.append(b)
        return placed

    pictures, used = [], set()
    for h in sorted({k[0] for k in pairs}):
        if h in used:
            continue
        placed = picture(h)
        if len(placed) < 2:
            continue
        x0 = min(p[0] for p in placed.values()); y0 = min(p[1] for p in placed.values())
        tiles = sorted([t, p[0] - x0, p[1] - y0] for t, p in placed.items() if t not in used)
        if len(tiles) < 2:
            continue
        used.update(t[0] for t in tiles)
        w = max(t[1] + index[t[0]]['width'] for t in tiles)
        hh = max(t[2] + index[t[0]]['height'] for t in tiles)
        pictures.append({'size': [w, hh], 'tiles': tiles})
    json.dump(pictures, open(out, 'w'), indent=1)
    print(len(pictures), 'pictures of', len(used), 'pieces')


if __name__ == '__main__':
    main()
