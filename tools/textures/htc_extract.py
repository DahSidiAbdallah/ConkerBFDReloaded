"""Extract textures from a GLideN64 texture pack cache (.htc) by their Rice fingerprints.
  htc_extract.py PACK.htc RT64_JSON OUT_DIR [--only rt64hash,rt64hash...|--only-file LIST] [--list]
RT64_JSON: rt64.json made by RT64's texture_hasher (--rice) from a texture dump, pairing each
RT64 hash with its Rice one ("crc#fmt#siz[#palette crc]"). Writes OUT_DIR/<rt64 hash>.png for
the textures the pack has. --list only prints what the pack has for them.

The .htc file is gzip: an 8-byte header (version, options), then entries: checksum (u64: Rice crc low, palette crc
high), width, height (u32), GL internal format (u32), GL format (u16), GL type (u16), hi-res flag
(u8), N64 format and size (u16), data size (u32), data (RGBA8 pixels, or zlib-compressed)."""
import gzip
import json
import os
import struct
import sys
import zlib

from PIL import Image

GL_RGBA8 = 0x8058


def main():
    pack, rt64_json, out = sys.argv[1], sys.argv[2], sys.argv[3]
    only = None
    if '--only' in sys.argv:
        only = set(sys.argv[sys.argv.index('--only') + 1].split(','))
    if '--only-file' in sys.argv:
        only = set(l.strip() for l in open(sys.argv[sys.argv.index('--only-file') + 1]) if l.strip())
    list_only = '--list' in sys.argv
    wanted = {}
    for t in json.load(open(rt64_json))['textures']:
        h, rice = t['hashes'].get('rt64'), t['hashes'].get('rice')
        if not h or not rice or (only is not None and h not in only):
            continue
        parts = rice.split('#')
        crc = int(parts[0], 16)
        pal = int(parts[3], 16) if len(parts) > 3 else 0
        wanted.setdefault((pal << 32) | crc, []).append(h)
    os.makedirs(out, exist_ok=True)
    found = 0
    with gzip.open(pack, 'rb') as f:
        f.read(8)
        while True:
            head = f.read(8 + 4 + 4 + 4 + 2 + 2 + 1 + 2 + 4)
            if len(head) < 31:
                break
            checksum, w, h, fmt, gfmt, gtype, hires, n64fs, size = struct.unpack('<QIIIHHBHI', head)
            if checksum in wanted:
                data = f.read(size)
                if size != w * h * 4:
                    try:
                        data = zlib.decompress(data)
                    except zlib.error:
                        pass
                if fmt == GL_RGBA8 and len(data) == w * h * 4:
                    for rh in wanted[checksum]:
                        found += 1
                        if list_only:
                            print(rh, w, h)
                        else:
                            Image.frombytes('RGBA', (w, h), data).save(os.path.join(out, rh + '.png'))
                else:
                    print('skipped', hex(checksum), w, h, hex(fmt), size, file=sys.stderr)
            else:
                f.seek(size, 1)
    print(found, 'found of', sum(len(v) for v in wanted.values()), 'wanted')


if __name__ == '__main__':
    main()
