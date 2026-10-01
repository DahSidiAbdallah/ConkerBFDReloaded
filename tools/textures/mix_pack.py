"""Make a texture pack's 2D pictures (HUD, menus, text) match the originals.
  mix_pack.py ORIGINALS_DIR PACK_DIR LAYOUT.json PLACES.txt OUT_DIR [--restyle hash,...] [--redraw hash,...]
LAYOUT.json (tile_layout.py) lists the pictures the game draws as several pieces: each is put
together whole, worked on, and cut back into its pieces, so the joins line up. PLACES.txt is the
dump's rect_places.txt (which pictures are drawn at the same place: a button's normal and glowing
look). For each picture (or piece on its own):
- The pack's colours are put back to the original's where its author changed them (e.g. red
  lettering on green buttons): a smooth colour mapping, fitted from the pack's picture shrunk to
  the original's size onto the original, is applied to the pack's own pixels (so its edges stay sharp).
- Shapes the pack added (e.g. dots after QUIT) are taken out whole.
- A pack picture that's only a blurred enlargement (the buttons' glowing looks) is remade from
  the sharp picture drawn at the same place: its shapes, in the glowing look's colours, over the
  original's glow enlarged smoothly.
- A picture redrawn in another shape (outline matching the original's by less than 60%) is left
  out, unless listed in --restyle (its shapes kept, recoloured part by part from the original:
  e.g. PAUSED, and the Rare logo, drawn a little differently so the colour mapping goes wrong) or --redraw (the original enlarged with clean edges: flat pictures, e.g. the Dolby logo).
Prints what it did."""
import collections
import json
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

SCALE_OWN = 4 # pictures made here: 4 times the original's size
MAX_SCALE = 8 # bigger pack pictures are made this size (8 times the original's: enough for 4K)


def load(path):
    return np.asarray(Image.open(path).convert('RGBA')).astype(np.float32) / 255


def save(a, path):
    Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGBA').save(path)


def resize(a, size, method=Image.LANCZOS):
    """Resizes an RGBA array (colours weighted by transparency, so see-through pixels don't bleed)."""
    pm = np.dstack([a[..., :3] * a[..., 3:], a[..., 3]])
    chans = [np.asarray(Image.fromarray(np.ascontiguousarray(pm[..., c], np.float32), 'F').resize(size, method)) for c in range(4)]
    out = np.dstack(chans)
    alpha = np.clip(out[..., 3], 0, 1)
    rgb = out[..., :3] / np.maximum(alpha[..., None], 1e-4)
    return np.dstack([np.clip(rgb, 0, 1), alpha])


def shrink(a, size):
    return resize(a, size, Image.BOX)


def luma(rgb):
    return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114


def features(rgb):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    return np.stack([np.ones_like(r), r, g, b, r * r, g * g, b * b, r * g, r * b, g * b], -1)


def fit_colours(src, dst, weight):
    """A smooth colour mapping (quadratic in RGB) taking src's colours to dst's, where weight > 0."""
    m = weight > 0.05
    if m.sum() < 12:
        return None
    x, y, w = features(src[m][:, :3]), dst[m][:, :3], weight[m][:, None]
    identity = np.zeros((10, 3)); identity[1, 0] = identity[2, 1] = identity[3, 2] = 1
    lam = 0.001 * w.sum()
    a = (x * w).T @ x + lam * np.eye(10)
    b = (x * w).T @ y + lam * identity
    return np.linalg.solve(a, b)


def apply_colours(m, a):
    return np.dstack([np.clip(features(a[..., :3]) @ m, 0, 1), a[..., 3]])


def colour_gap(a, b, weight):
    m = weight > 0.05
    return float((np.abs(a[..., :3] - b[..., :3]).mean(-1)[m] * weight[m]).sum() / max(1e-6, weight[m].sum())) * 255


def match_weight(o, s):
    """Where the original and the shrunk pack picture can be compared: both solid, away from edges."""
    w = np.minimum(o[..., 3], s[..., 3])
    edge = np.hypot(*np.gradient(luma(o[..., :3]) * o[..., 3]))
    return np.where(w > 0.5, w / (1 + edge * 8), 0)


def outline_match(o, s):
    oa, pa = o[..., 3] > 0.5, s[..., 3] > 0.5
    return (oa & pa).sum() / max(1, (oa | pa).sum())


def sharpness(a):
    y = luma(a[..., :3]) * a[..., 3]
    return float(np.abs(ndimage.laplace(y)).mean())


def remove_added_shapes(p, o):
    """Takes out shapes of p lying (almost) wholly where the original is transparent."""
    s = p.shape[0] / o.shape[0]
    grown = ndimage.binary_dilation(o[..., 3] > 0.4, iterations=1)
    grown = np.asarray(Image.fromarray(grown.astype(np.uint8) * 255).resize((p.shape[1], p.shape[0]), Image.NEAREST)) > 0
    labels, n = ndimage.label(p[..., 3] > 0.25)
    removed = np.zeros(labels.shape, bool)
    for i in range(1, n + 1):
        part = labels == i
        if (part & grown).sum() < 0.15 * part.sum() and part.sum() > s * s:
            removed |= part
    if not removed.any():
        return p, False
    # Its soft edge too, where the original has nothing.
    halo = ndimage.binary_dilation(removed, iterations=max(2, int(s))) & ~grown
    out = p.copy()
    out[..., 3] = np.where(removed | halo, 0, p[..., 3])
    return out, True


def clean_enlarge(o, scale=SCALE_OWN):
    """The original enlarged with clean edges (flat pictures: logos, lettering)."""
    size = (o.shape[1] * scale, o.shape[0] * scale)
    big = resize(o, size, Image.BICUBIC)
    a = big[..., 3]
    big[..., 3] = np.clip((a - 0.5) * 2.2 + 0.5, 0, 1)
    big[..., 3] = big[..., 3] * big[..., 3] * (3 - 2 * big[..., 3])
    return big


def colour_parts(a, mask):
    """Splits a picture's pixels into two colour parts (e.g. letters and their outline), the darker
    first: by colour where it's clear, and pixels without much colour (white shine, black shade) go
    with the part around them. Returns each pixel's share of each part (H, W, 2)."""
    rgb = a[..., :3]
    ch = np.stack([rgb[..., 2] - luma(rgb), rgb[..., 0] - luma(rgb)], -1) # colour, without brightness
    clear = mask & (np.hypot(ch[..., 0], ch[..., 1]) > 0.04)
    if clear.sum() < 8:
        clear = mask
    c = ch[clear]
    centres = np.array([c[c[:, 0].argmin()], c[c[:, 0].argmax()]])
    for _ in range(20):
        lab = np.argmin(((c[:, None] - centres[None]) ** 2).sum(-1), 1)
        centres = np.array([c[lab == k].mean(0) if (lab == k).any() else centres[k] for k in range(2)])
    d = np.stack([((ch - centres[k]) ** 2).sum(-1) for k in range(2)], -1)
    near = (d == d.min(-1, keepdims=True)).astype(np.float32) * clear[..., None]
    sigma = max(1.0, 0.02 * max(mask.shape))
    spread = np.stack([ndimage.gaussian_filter(near[..., k], sigma) for k in range(2)], -1)
    spread /= np.maximum(spread.sum(-1, keepdims=True), 1e-6)
    share = np.where(clear[..., None], near, spread)
    share = np.where(spread.sum(-1, keepdims=True) > 0, share, 0.5)
    bright = [luma(rgb)[mask & (share[..., k] > 0.5)].mean() if (mask & (share[..., k] > 0.5)).any() else 0 for k in range(2)]
    return share[..., np.argsort(bright)]


def restyle(p, o):
    """p's shapes in o's colours: each has two colour parts (colour_parts), matched darker to
    darker; within a part, p's brightness picks the original's colour of the same brightness rank."""
    om, pm = o[..., 3] > 0.5, p[..., 3] > 0.3
    os_, ps = colour_parts(o, om), colour_parts(p, pm)
    y = luma(p[..., :3])
    new = np.zeros_like(p[..., :3])
    for k in range(2):
        o_part = o[..., :3][om & (os_[..., k] > 0.5)]
        p_y = y[pm & (ps[..., k] > 0.5)]
        if len(o_part) == 0 or len(p_y) == 0:
            continue
        o_sorted = o_part[np.argsort(luma(o_part))]
        rank = np.searchsorted(np.sort(p_y), y) / len(p_y)
        idx = np.clip((rank * (len(o_sorted) - 1)).astype(int), 0, len(o_sorted) - 1)
        # Average neighbouring ranks for smooth ramps.
        lo, hi = np.clip(idx - 2, 0, None), np.clip(idx + 3, None, len(o_sorted))
        cs = np.cumsum(np.vstack([np.zeros(3), o_sorted]), 0)
        new += ps[..., k:k + 1] * (cs[hi] - cs[lo]) / (hi - lo)[..., None]
    return np.dstack([new, p[..., 3]])


def main():
    orig_dir, pack_dir, layout, places, out = sys.argv[1:6]
    opt = lambda name: set(sys.argv[sys.argv.index(name) + 1].split(',')) if name in sys.argv else set()
    restyles, redraws = opt('--restyle'), opt('--redraw')
    os.makedirs(out, exist_ok=True)
    pictures = json.load(open(layout))
    in_picture = {t[0] for p in pictures for t in p['tiles']}
    for n in sorted(os.listdir(orig_dir)):
        h = n[:-4]
        if n.endswith('.png') and h not in in_picture:
            im = Image.open(os.path.join(orig_dir, n))
            pictures.append({'size': list(im.size), 'tiles': [[h, 0, 0]]})
    # Where each piece is drawn, to find the pictures drawn at the same place.
    where = collections.defaultdict(set)
    for line in open(places):
        _, h, fb, _, x, y, _, _ = line.split()
        where[h].add((fb, int(x), int(y)))

    def matches(names, h):
        return any(h.startswith(x) for x in names)

    def assemble(p, d, scale=None):
        w, hh = p['size']
        first = os.path.join(d, p['tiles'][0][0] + '.png')
        if scale is None:
            if not all(os.path.exists(os.path.join(d, t[0] + '.png')) for t in p['tiles']):
                return None, None
            ow = Image.open(os.path.join(orig_dir, p['tiles'][0][0] + '.png')).width
            scale = min(MAX_SCALE, Image.open(first).width / ow)
        a = np.zeros((int(round(hh * scale)), int(round(w * scale)), 4), np.float32)
        for h, x, y in p['tiles']:
            im = load(os.path.join(d, h + '.png'))
            osz = Image.open(os.path.join(orig_dir, h + '.png')).size
            tw, th = int(round(osz[0] * scale)), int(round(osz[1] * scale))
            if im.shape[:2] != (th, tw):
                im = resize(im, (tw, th))
            a[int(round(y * scale)):int(round(y * scale)) + th, int(round(x * scale)):int(round(x * scale)) + tw] = im
        return a, scale

    origs = [assemble(p, orig_dir, 1.0)[0] for p in pictures]
    packs = [assemble(p, pack_dir) for p in pictures]
    blurry = []
    for i, p in enumerate(pictures):
        pk, s = packs[i]
        if pk is None:
            blurry.append(False); continue
        smooth = resize(origs[i], (pk.shape[1], pk.shape[0]))
        blurry.append(sharpness(pk) < 1.35 * sharpness(smooth))

    def sibling(i):
        """A picture drawn at the same place, of the same size, whose pack picture is sharp."""
        def corner(q):
            return min(q['tiles'], key=lambda t: (t[2], t[1]))[0]
        for j, q in enumerate(pictures):
            if j != i and q['size'] == pictures[i]['size'] and packs[j][0] is not None and not blurry[j] \
                    and len(q['tiles']) == len(pictures[i]['tiles']) and where[corner(q)] & where[corner(pictures[i])]:
                return j
        return None

    counts = collections.Counter()
    for i, p in enumerate(pictures):
        o = origs[i]
        pk, s = packs[i]
        name = p['tiles'][0][0][:6] + (f' (+{len(p["tiles"]) - 1} pieces)' if len(p['tiles']) > 1 else '')
        notes = []
        if matches(redraws, p['tiles'][0][0]):
            result, s = clean_enlarge(o), SCALE_OWN
            notes.append('redrawn with clean edges')
        elif pk is None:
            counts['not in the pack'] += 1
            continue
        else:
            small = shrink(pk, (o.shape[1], o.shape[0]))
            match = outline_match(o, small)
            if matches(restyles, p['tiles'][0][0]):
                result = restyle(pk, o)
                notes.append(f'recoloured part by part (outline match {match:.2f})')
            elif match < 0.6 and o.shape[0] * o.shape[1] >= 256:
                print('left out', name, f'(redrawn in another shape: outline match {match:.2f})')
                counts['left out'] += 1
                continue
            else:
                j = sibling(i) if blurry[i] else None
                if j is not None:
                    # The glowing look: the sharp look's shapes in these colours, over this glow enlarged.
                    sharp = packs[j][0]
                    if sharp.shape != pk.shape:
                        sharp = resize(sharp, (pk.shape[1], pk.shape[0]))
                    ssmall = shrink(sharp, (o.shape[1], o.shape[0]))
                    m = fit_colours(ssmall, o, match_weight(o, ssmall))
                    detail = apply_colours(m, sharp) if m is not None else sharp
                    base = resize(o, (pk.shape[1], pk.shape[0]))
                    a = detail[..., 3:]
                    result = np.dstack([detail[..., :3] * a + base[..., :3] * base[..., 3:] * (1 - a),
                                        a[..., 0] + base[..., 3] * (1 - a[..., 0])])
                    result[..., :3] /= np.maximum(result[..., 3:], 1e-4)
                    notes.append('glowing look remade from ' + pictures[j]['tiles'][0][0][:6])
                    pk = result
                result = pk
                small = shrink(result, (o.shape[1], o.shape[0]))
                wgt = match_weight(o, small)
                gap = colour_gap(small, o, wgt)
                if gap > 10:
                    m = fit_colours(small, o, wgt)
                    if m is not None:
                        mapped = apply_colours(m, result)
                        new_gap = colour_gap(shrink(mapped, (o.shape[1], o.shape[0])), o, wgt)
                        if new_gap < 0.75 * gap:
                            result = mapped
                            notes.append(f'colours put back ({gap:.0f} -> {new_gap:.0f})')
                result, trimmed = remove_added_shapes(result, o)
                if trimmed:
                    notes.append('added shapes taken out')
        for h, x, y in p['tiles']:
            osz = Image.open(os.path.join(orig_dir, h + '.png')).size
            x0, y0 = int(round(x * s)), int(round(y * s))
            save(result[y0:y0 + int(round(osz[1] * s)), x0:x0 + int(round(osz[0] * s))], os.path.join(out, h + '.png'))
        counts['made'] += 1
        if notes:
            print(name + ':', '; '.join(notes))
    print(dict(counts))


if __name__ == '__main__':
    main()
