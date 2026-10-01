"""The save menu's NEW GAME button for the HD Icons pack, redrawn: GameBeast92's 4K pack has no HD
version of it (its pieces are the original 32x32 size). The button's frame is kept from the pack's
picture (MIXED_DIR, made by mix_pack.py), its old blurry lettering cleared and "NEW GAME" drawn
fresh in the same green glow, in Segoe Print Bold. Writes the three pieces back into MIXED_DIR.
  make_new_game.py MIXED_DIR [FONT.ttf]"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from scipy import ndimage

PIECES = ['dc36845ff141fa1b', 'f64c0a861b172a26', '4bec47ee2d384894'] # left to right, 32x32 each
SCALE = 8
FONT = '/mnt/c/Windows/Fonts/segoeprb.ttf'


def main():
    mixed = sys.argv[1]
    font_path = sys.argv[2] if len(sys.argv) > 2 else FONT
    if not all(os.path.exists(os.path.join(mixed, h + '.png')) for h in PIECES) or not os.path.exists(font_path):
        print('NEW GAME: pieces or font missing, left as it is')
        return
    size = 32 * SCALE
    width, height = size * len(PIECES), size
    base = Image.new('RGBA', (width, height))
    for i, h in enumerate(PIECES):
        base.alpha_composite(Image.open(os.path.join(mixed, h + '.png')).convert('RGBA').resize((size, size), Image.LANCZOS), (i * size, 0))
    a = np.asarray(base).astype(float)
    # The frame: the widest opaque shape; inside it, clear the old lettering (softly at the edge).
    labels, n = ndimage.label(a[..., 3] > 150)
    objects = ndimage.find_objects(labels)
    ring = labels == (max(range(n), key=lambda i: objects[i][1].stop - objects[i][1].start) + 1)
    interior = ndimage.binary_fill_holes(ring) & ~ndimage.binary_dilation(ring, iterations=1)
    soft = ndimage.gaussian_filter(ndimage.binary_erosion(interior, iterations=3).astype(float), 3)
    a[..., 3] *= 1 - soft
    out = Image.fromarray(a.astype(np.uint8), 'RGBA')
    ys, xs = np.nonzero(interior)
    x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
    # The lettering, as large as fits, thickened, tilted like the original.
    text, stroke = 'NEW GAME', 7
    probe = ImageDraw.Draw(Image.new('L', (8, 8)))
    font = ImageFont.truetype(font_path, 150)
    box = probe.textbbox((0, 0), text, font=font, stroke_width=stroke)
    k = min((x1 - x0 - 70) / (box[2] - box[0]), (y1 - y0 - 44) / (box[3] - box[1]))
    font, stroke = ImageFont.truetype(font_path, int(150 * k)), max(1, int(stroke * k))
    box = probe.textbbox((0, 0), text, font=font, stroke_width=stroke)
    letters = Image.new('L', (width, height), 0)
    ImageDraw.Draw(letters).text(((x0 + x1) / 2 - (box[2] - box[0]) / 2 - box[0], (y0 + y1) / 2 - (box[3] - box[1]) / 2 - box[1]),
                                 text, font=font, fill=255, stroke_width=stroke, stroke_fill=255)
    letters = letters.rotate(-2.5, resample=Image.BICUBIC, center=(width / 2, height / 2))
    # Glow, a green core and a lighter middle, as the game's lettering.
    for blur, colour, strength in ((16, (60, 220, 0), 1.2), (5, (100, 255, 0), 1.0)):
        glow = Image.new('RGBA', (width, height), colour + (0,))
        glow.putalpha(letters.filter(ImageFilter.GaussianBlur(blur)).point(lambda v, s=strength: min(255, int(v * s))))
        out.alpha_composite(glow)
    core = Image.new('RGBA', (width, height), (115, 250, 0, 0))
    core.putalpha(letters)
    out.alpha_composite(core)
    light = Image.new('RGBA', (width, height), (205, 255, 60, 0))
    light.putalpha(letters.filter(ImageFilter.MinFilter(int(stroke * 1.4) | 1)).filter(ImageFilter.GaussianBlur(2)).point(lambda v: int(v * 0.55)))
    out.alpha_composite(light)
    for i, h in enumerate(PIECES):
        out.crop((i * size, 0, (i + 1) * size, size)).save(os.path.join(mixed, h + '.png'))
    print('NEW GAME: redrawn')


if __name__ == '__main__':
    main()
