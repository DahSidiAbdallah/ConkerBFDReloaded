"""HD versions of the speech-bubble font's accented capitals (É È Ê À Â Ç Ù Û Î Ï Ô), which the
English game never shows (so the HD Icons pack's sources never had them) but translations use.
The font draws each as a smaller copy of the letter with the accent above (the cedilla below), in
an 8-pixel-wide picture 13 or 14 tall. Each HD one is the pack's own HD letter, shrunk to where
the small letter sits in the original, and the accent drawn smooth where the original's is, in the
HD letters' grey and stroke weight.
  make_accents.py ORIGINALS MIXED [OUT]   (ORIGINALS: the game's pictures; MIXED: the pack's HD
  ones, where the new ones go unless OUT is given)"""
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

SCALE = 8      # the pack's letters are 8 times the game's
SUPER = 4      # accents drawn this much bigger, then shrunk (smooth edges)

def alpha(img):
    return np.array(img.convert('RGBA'))[:, :, 3].astype(float)


def bbox(a, thresh=40):
    ys, xs = np.nonzero(a > thresh)
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


def split_rows(a, accent_below):
    """Rows of the accent and of the letter in the game's small picture: the letter is the biggest
    block of rows; the accent the rows on the other side of the faintest row between them."""
    rows = a.sum(axis=1)
    lit = rows > 40
    ys = np.nonzero(lit)[0]
    top, bottom = ys.min(), ys.max() + 1
    # the faintest row inside, on the accent's side
    inner = range(top + 1, bottom - 1)
    if accent_below:
        cut = min((r for r in inner if r > (top + bottom) // 2), key=lambda r: rows[r])
        return (cut + 1, bottom), (top, cut)
    cut = min((r for r in inner if r < (top + bottom) // 2 + 1), key=lambda r: rows[r])
    return (top, cut), (cut + 1, bottom)


def draw_accent(kind, box, width, stroke, colour):
    """The accent in an RGBA picture `width` x box height (HD), inside box (x0, y0, x1, y1, HD)."""
    x0, y0, x1, y1 = [v * SUPER for v in box]
    big = Image.new('L', (width * SUPER, y1 + SUPER * SCALE), 0)
    d = ImageDraw.Draw(big)
    s = stroke * SUPER
    def line(p, q):
        d.line([p, q], fill=255, width=s)
        for c in (p, q):
            d.ellipse([c[0] - s / 2, c[1] - s / 2, c[0] + s / 2, c[1] + s / 2], fill=255)
    m = s / 2
    if kind == 'acute':
        line((x0 + m, y1 - m), (x1 - m, y0 + m))
    elif kind == 'grave':
        line((x0 + m, y0 + m), (x1 - m, y1 - m))
    elif kind == 'circumflex':
        cx = (x0 + x1) / 2
        line((x0 + m, y1 - m), (cx, y0 + m))
        line((cx, y0 + m), (x1 - m, y1 - m))
    elif kind == 'diaeresis':
        r = s * 0.62
        cy = (y0 + y1) / 2
        for cx in (x0 + (x1 - x0) * 0.25, x1 - (x1 - x0) * 0.25):
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    elif kind == 'cedilla':
        # a short stem down from the letter, then a hook curling right and back under it
        w, h = x1 - x0, y1 - y0
        cx = (x0 + x1) / 2
        points = [(cx, y0 + m), (cx + w * 0.04, y0 + h * 0.32), (cx + w * 0.22, y0 + h * 0.45),
                  (cx + w * 0.26, y0 + h * 0.65), (cx + w * 0.12, y1 - m * 1.2), (cx - w * 0.18, y1 - m)]
        for p, q in zip(points, points[1:]):
            line(p, q)
    return big


def make(original, base_hd, kind, accent_below=False):
    a = alpha(original)
    h, w = a.shape
    (ay0, ay1), (ly0, ly1) = split_rows(a, accent_below)
    lx0, _, lx1, _ = bbox(a[ly0:ly1])
    ax0, _, ax1, _ = bbox(a[ay0:ay1], 20)
    out = Image.new('RGBA', (w * SCALE, h * SCALE), (0, 0, 0, 0))
    # the HD letter, shrunk to the small letter's place
    hd = base_hd.convert('RGBA')
    hx0, hy0, hx1, hy1 = bbox(alpha(hd), 60)
    letter = hd.crop((hx0, hy0, hx1, hy1))
    # The HD letter at the same size as the plain letter (as big as its neighbours in a bubble), sitting
    # on the small letter's baseline (the game lines letters up by their bottoms, the accent rising
    # above); only shrunk if it wouldn't fit the picture's width or leave room for the accent.
    W, H = w * SCALE, h * SCALE
    gap = SCALE // 2
    accent_room = 2.4 * SCALE if not accent_below else 0
    base_line = ly1 * SCALE
    k = min(1.0, (W - 2) / letter.width, (base_line - accent_room - gap) / letter.height)
    tw, th = max(1, int(round(letter.width * k))), max(1, int(round(letter.height * k)))
    letter = letter.resize((tw, th), Image.LANCZOS)
    cx = (lx0 + lx1) / 2 * SCALE
    left = int(round(min(max(cx - tw / 2, 1), W - 1 - tw)))
    letter_top = base_line - th
    out.alpha_composite(letter, (left, letter_top))
    # the HD letters' grey and stroke weight
    la = alpha(letter)
    colour = tuple(int(c) for c in np.array(letter)[la > 200][:, :3].mean(axis=0)) if (la > 200).any() else (216, 216, 216)
    stroke = max(4, int(round(0.12 * th)))
    # The accent: over the letter's middle as wide as the original's, in the room left above it (or
    # below it, for the cedilla).
    aw = max((ax1 - ax0) * SCALE, 3 * stroke)
    acx = left + tw / 2 if kind in ('circumflex', 'diaeresis') else (ax0 + ax1) / 2 * SCALE
    if accent_below:
        box = (acx - aw / 2, base_line - stroke, acx + aw / 2, H - 1)
    else:
        bottom = letter_top - gap
        top = max(1, bottom - max(accent_room, 1.6 * stroke))
        box = (acx - aw / 2, top, acx + aw / 2, bottom)
    box = tuple(int(round(v)) for v in box)
    big = draw_accent(kind, box, w * SCALE, stroke, colour)
    mark = big.resize((w * SCALE, big.height // SUPER), Image.LANCZOS).crop((0, 0, w * SCALE, h * SCALE))
    layer = Image.new('RGBA', out.size, colour + (0,))
    layer.putalpha(mark)
    out.alpha_composite(layer)
    return out


# (accented letter's picture, its HD letter's picture, accent, accent below the letter)
LIST = [
    ('101ed1feb1be80d7', '1e7a441c7f38a233', 'circumflex', False),  # Ê
    ('3d341b0dd52615d9', '1e7a441c7f38a233', 'acute', False),  # É
    ('903bfc2762f9d0d9', 'cf30942b05d42789', 'circumflex', False),  # Â
    ('96df22e34f7d58b0', '2a330b1030a72f23', 'diaeresis', False),  # Ï
    ('abd31701bed91c92', '8a17518380eaad08', 'circumflex', False),  # Û
    ('bf1a8d9a422ad039', '0e9248879e05de57', 'circumflex', False),  # Ô
    ('e5527722615d8732', 'cf30942b05d42789', 'grave', False),  # À
    ('f1a3adb3e705c16b', '8a17518380eaad08', 'grave', False),  # Ù
    ('f21baa17f4d6b794', '2a330b1030a72f23', 'circumflex', False),  # Î
    ('f737e17f499f93ed', '5e4f17df32a51301', 'cedilla', True),  # Ç
    ('f764115a0b6496d3', '1e7a441c7f38a233', 'grave', False),  # È
]


def main():
    originals, mixed = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else mixed
    made = 0
    for accented, letter, kind, below in LIST:
        try:
            original = Image.open(f'{originals}/{accented}.png')
            base = Image.open(f'{mixed}/{letter}.png')
        except FileNotFoundError:
            continue
        make(original, base, kind, below).save(f'{out}/{accented}.png')
        made += 1
    print(f'made {made} accented letters')


if __name__ == '__main__':
    main()
