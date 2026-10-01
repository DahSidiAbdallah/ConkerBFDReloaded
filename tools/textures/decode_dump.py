"""Turn RT64 texture dumps (<hash>.v<ver>.tmem + .tile.json, from RT64_DUMP_TEXTURES) into PNGs.
Decoding follows RT64's shaders/TextureDecoder.hlsli (sampleTMEM).
  decode_dump.py DUMP_DIR OUT_DIR [--rect-only]
--rect-only: only the textures listed in DUMP_DIR/rect_hashes.txt (drawn as 2D rectangles:
the HUD, menus, text), and not the ones read from a picture of the screen (the pause menu's
blurred background: loaded from an image 200 or more pixels wide). Writes OUT_DIR/<hash>.png and OUT_DIR/index.json (hash -> size, format)."""
import json
import os
import sys

from PIL import Image

G_IM_FMT_RGBA, G_IM_FMT_YUV, G_IM_FMT_CI, G_IM_FMT_IA, G_IM_FMT_I = 0, 1, 2, 3, 4
TMEM_BYTES, TMEM_PALETTE, MASK8, MASK16 = 0x1000, 0x800, 0xFFF, 0x7FF


def rgba16(v):
    r, g, b = (v >> 11) & 0x1F, (v >> 6) & 0x1F, (v >> 1) & 0x1F
    return ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255 if v & 1 else 0)


def ia16(v):
    i = (v >> 8) & 0xFF
    return (i, i, i, v & 0xFF)


def decode(tmem, tile, width, height, tlut):
    fmt, siz = tile['fmt'], tile['siz']
    address, stride, palette = tile['tmem'] << 3, tile['line'] << 3, tile['palette']
    rgba32 = fmt == G_IM_FMT_RGBA and siz == 3
    uses_tlut = tlut in ('RGBA16', 'IA16')
    shift = 2 if rgba32 else siz
    mask = MASK16 if (rgba32 or uses_tlut) else MASK8

    def load(rel, odd_row, or_address=0):
        row_start = (rel // stride) * stride if stride else 0
        word = (rel - row_start) // 4
        if odd_row:
            final = address + row_start + ((word ^ 1) * 4) + (rel & 3)
        else:
            final = address + rel
        return tmem[((final & mask) | or_address) & MASK8]

    img = Image.new('RGBA', (width, height))
    px = img.load()
    for y in range(height):
        odd_row = y & 1
        for x in range(width):
            pa = y * stride + ((x << shift) >> 1)
            p0, p1 = load(pa, odd_row), load(pa + 1, odd_row)
            p4 = (p0 >> (0 if x & 1 else 4)) & 0xF
            if uses_tlut:
                pal = TMEM_PALETTE + (palette << 7) + (p4 << 3) if siz == 0 else TMEM_PALETTE + (p0 << 3)
                v = tmem[(pal + 1) & MASK8] | (tmem[pal & MASK8] << 8)
                c = rgba16(v) if tlut == 'RGBA16' else ia16(v)
            elif siz == 0:
                if fmt == G_IM_FMT_IA:
                    i = p4 & 0b1110
                    i = ((i << 4) | (i << 1) | (i >> 2)) & 0xFF
                    c = (i, i, i, 255 if p4 & 1 else 0)
                elif fmt == G_IM_FMT_CI:
                    i = (palette << 4) | p4
                    c = (i, i, i, i)
                else:
                    i = (p4 << 4) | p4
                    c = (i, i, i, i)
            elif siz == 1:
                if fmt == G_IM_FMT_IA:
                    i, a = (p0 >> 4) & 0xF, p0 & 0xF
                    i, a = (i << 4) | i, (a << 4) | a
                    c = (i, i, i, a)
                else:
                    c = (p0, p0, p0, p0)
            elif siz == 2:
                v = p1 | (p0 << 8)
                if fmt == G_IM_FMT_RGBA:
                    c = rgba16(v)
                elif fmt == G_IM_FMT_IA:
                    c = ia16(v)
                else:
                    c = (p0, p1, p0, p1)
            else:
                pa2 = pa if rgba32 else pa + 2
                orad = (TMEM_BYTES >> 1) if rgba32 else 0
                p2, p3 = load(pa2, odd_row, orad), load(pa2 + 1, odd_row, orad)
                c = (p0, p1, p2, p3) if fmt == G_IM_FMT_RGBA else ((p0, p1, p0, p1) if x & 1 else (p2, p3, p2, p3))
            px[x, y] = c
    return img


def main():
    dump, out = sys.argv[1], sys.argv[2]
    rect_only = '--rect-only' in sys.argv
    os.makedirs(out, exist_ok=True)
    wanted = None
    if rect_only:
        wanted = set(l.strip() for l in open(os.path.join(dump, 'rect_hashes.txt')) if l.strip())
    index = {}
    for name in sorted(os.listdir(dump)):
        if not name.endswith('.tile.json'):
            continue
        base = name[:-len('.tile.json')]
        h = base.split('.')[0]
        if wanted is not None and h not in wanted:
            continue
        if rect_only:
            rice = os.path.join(dump, base + '.rice.json')
            if os.path.exists(rice) and json.load(open(rice))['texture']['width'] >= 200:
                continue
        info = json.load(open(os.path.join(dump, name)))
        tmem = open(os.path.join(dump, base + '.tmem'), 'rb').read()
        w, hgt = info['width'], info['height']
        if w <= 0 or hgt <= 0 or w > 1024 or hgt > 1024:
            continue
        img = decode(tmem, info['tile'], w, hgt, info.get('tlut'))
        img.save(os.path.join(out, h + '.png'))
        index[h] = {'width': w, 'height': hgt, 'fmt': info['tile']['fmt'], 'siz': info['tile']['siz'], 'tlut': info.get('tlut')}
    json.dump(index, open(os.path.join(out, 'index.json'), 'w'), indent=1)
    print(len(index), 'textures')


if __name__ == '__main__':
    main()
