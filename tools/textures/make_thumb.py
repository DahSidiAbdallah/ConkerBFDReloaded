"""The HD Icons mod's picture for the Mods menu (shown small, about 100 pixels): a close-up of the
"Bad Fur Day" logo (the biggest 2D picture, 29 pieces) split down the middle of its "A", the
original's pixels on the left and the HD picture on the right, labelled N64 and HD.
  make_thumb.py WORK_DIR OUT.png
WORK_DIR: build_icon_pack.sh's icons_work/all (layout.json, originals/, mixed/)."""
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

SIZE = 512
CENTRE, CROP = (82, 62), 92 # the close-up, in the original logo's pixels (160x192)
FONT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../host/art/Poppins-Black.ttf')
DARK, LIGHT = (20, 12, 6, 255), (255, 244, 214, 255)


def main():
    work, out = sys.argv[1], sys.argv[2]
    logo = [p for p in json.load(open(f'{work}/layout.json')) if p['size'] == [160, 192]][0]

    def build(folder, scale):
        picture = Image.new('RGBA', (160 * scale, 192 * scale), (0, 0, 0, 0))
        for h, x, y in logo['tiles']:
            piece = Image.open(f'{folder}/{h}.png').convert('RGBA').resize((32 * scale, 32 * scale), Image.LANCZOS)
            picture.alpha_composite(piece, (x * scale, y * scale))
        return picture

    box = (CENTRE[0] - CROP // 2, CENTRE[1] - CROP // 2, CENTRE[0] + CROP // 2, CENTRE[1] + CROP // 2)
    before = build(f'{work}/originals', 1).crop(box).resize((SIZE, SIZE), Image.NEAREST) # its pixels, big
    after = build(f'{work}/mixed', 8).crop(tuple(v * 8 for v in box)).resize((SIZE, SIZE), Image.LANCZOS)
    left, right = Image.new('RGBA', (SIZE, SIZE), (24, 15, 9, 255)), Image.new('RGBA', (SIZE, SIZE), (24, 15, 9, 255))
    left.alpha_composite(before)
    right.alpha_composite(after)
    half = Image.new('L', (SIZE, SIZE), 0)
    half.paste(255, (SIZE // 2, 0, SIZE, SIZE))
    thumb = Image.composite(right, left, half)
    draw = ImageDraw.Draw(thumb)
    # A thick light line with dark edges, so it shows on any colour even when small.
    draw.rectangle([SIZE // 2 - 9, 0, SIZE // 2 + 9, SIZE], fill=DARK)
    draw.rectangle([SIZE // 2 - 5, 0, SIZE // 2 + 5, SIZE], fill=LIGHT)
    font = ImageFont.truetype(FONT, 84)
    draw.text((22, SIZE - 22), 'N64', font=font, anchor='ls', fill=(255, 255, 255, 255), stroke_width=10, stroke_fill=DARK)
    draw.text((SIZE - 22, SIZE - 22), 'HD', font=font, anchor='rs', fill=(255, 255, 255, 255), stroke_width=10, stroke_fill=DARK)
    thumb.convert('RGB').save(out)


if __name__ == '__main__':
    main()
