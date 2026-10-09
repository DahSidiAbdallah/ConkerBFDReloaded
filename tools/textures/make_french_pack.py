"""The French version of the HD Icons pack: the HD Icons pictures, with the menus, titles and logos
of the French translation (Corrigo's text, Djipi's graphics; emulation64.fr) in place of the English
ones. The French graphics come as a GLideN64 texture pack ('CONKER BFD_HIRESTEXTURES.htc', the old
format: no N64 format in the entries), keyed by the Rice fingerprints of the game's pictures; the
texture dumps' rt64.json files (texture_hasher --rice) pair those with RT64's hashes.
  make_french_pack.py HTC MIXED OUT [DUMP_DIR ...]   (MIXED: the HD Icons pack's pictures; OUT gets
  them plus the French ones, named <rt64 hash>.png)"""
import gzip
import json
import os
import shutil
import struct
import sys
import zlib

from PIL import Image

COMPRESSED = 0x80000000


def entries(path):
    """The old .htc: gzip; a 4-byte header, then per picture: checksum (u64: Rice crc low, palette crc
    high), width, height, GL internal format (u32, top bit: zlib), GL format, GL type (u16), a flag
    (u8), data size (u32), data (RGBA8)."""
    data = gzip.decompress(open(path, 'rb').read())
    p = 4
    while p < len(data):
        checksum, width, height, gl_format, _, _, _, size = struct.unpack_from('<QIIIHHBI', data, p)
        p += 29
        pixels = data[p:p + size]
        p += size
        if gl_format & COMPRESSED:
            pixels = zlib.decompress(pixels)
        yield checksum & 0xFFFFFFFF, checksum >> 32, Image.frombytes('RGBA', (width, height), pixels[:width * height * 4])


def main():
    htc, mixed, out, dumps = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    rt64 = {}
    for d in dumps:
        path = os.path.join(d, 'rt64.json')
        if not os.path.exists(path):
            continue
        for t in json.load(open(path))['textures']:
            rice = t['hashes']['rice'].split('#')
            palette = int(rice[3], 16) if len(rice) > 3 else 0
            rt64.setdefault((int(rice[0], 16), palette), set()).add(t['hashes']['rt64'])
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(mixed, out)
    found = replaced = missing = 0
    # Some of the colour-indexed pictures' entries have no palette fingerprint (0): those go by the
    # picture's alone, whatever its palette.
    any_palette = {}
    for (crc, _), hashes in rt64.items():
        any_palette.setdefault(crc, set()).update(hashes)
    for crc, palette, picture in entries(htc):
        hashes = rt64.get((crc, palette)) or (any_palette.get(crc) if palette == 0 else None)
        if not hashes:
            missing += 1
            continue
        found += 1
        for h in hashes:
            target = os.path.join(out, h + '.png')
            replaced += os.path.exists(target)
            picture.save(target)
    print(f'French pictures: {found} placed ({replaced} over HD Icons ones), {missing} not seen in the dumps')


if __name__ == '__main__':
    main()
