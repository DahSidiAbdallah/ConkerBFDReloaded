"""Unpack every compressed asset in the ROM into files laid out like the game's memory, so
scan_rdram.py can find a texture pack's pictures in them, including ones the game only loads
now and then (e.g. each HUD digit, loaded only while that number is on screen).
  rom_assets.py ROM.z64 OUT_DIR
Rare's compression (the decomp's tools/rareunzip.py): a 4-byte big-endian size, then raw
deflate. Every position after the code is tried; what unpacks to exactly its size is an asset.
Assets are written 8-byte aligned, 32-bit words byte-swapped as RT64 keeps memory, into 8 MB
files OUT_DIR/assets_<n>.rdram."""
import os
import sys
import zlib

import numpy as np

START = 0x1A37E0 # the compressed files (7760 of them) and the assets after them
FILE_SIZE = 8 * 1024 * 1024


def main():
    rom, out = open(sys.argv[1], 'rb').read(), sys.argv[2]
    os.makedirs(out, exist_ok=True)
    data = np.frombuffer(rom, dtype=np.uint8)
    n = len(rom) - 8
    sizes = (data[START:n].astype(np.uint32) << 24) | (data[START + 1:n + 1].astype(np.uint32) << 16) | \
            (data[START + 2:n + 2].astype(np.uint32) << 8) | data[START + 3:n + 3]
    first = data[START + 4:n + 4]
    # A plausible size, and the first deflate block not of the invalid type 3.
    candidates = np.nonzero((sizes >= 64) & (sizes <= 0x200000) & (((first >> 1) & 3) != 3))[0] + START
    assets, pos = [], 0
    for c in candidates:
        if c < pos:
            continue
        size = int.from_bytes(rom[c:c + 4], 'big')
        d = zlib.decompressobj(wbits=-15)
        try:
            res = d.decompress(rom[c + 4:c + 4 + size * 2 + 1024], size + 1)
        except zlib.error:
            continue
        if len(res) == size and d.eof:
            assets.append(res)
            pos = c + 4 + (len(rom[c + 4:c + 4 + size * 2 + 1024]) - len(d.unused_data))
    print(len(assets), 'assets,', sum(len(a) for a in assets), 'bytes')
    buf, count = bytearray(), 0

    def flush():
        nonlocal buf, count
        if buf:
            b = np.frombuffer(bytes(buf.ljust(FILE_SIZE, b'\0')), dtype=np.uint8).reshape(-1, 4)[:, ::-1]
            open(os.path.join(out, f'assets_{count}.rdram'), 'wb').write(b.tobytes())
            count += 1
            buf = bytearray()

    for a in assets:
        a = a + b'\0' * (-len(a) % 8)
        if len(a) > FILE_SIZE:
            continue
        if len(buf) + len(a) > FILE_SIZE:
            flush()
        buf += a
    flush()
    print(count, 'files')


if __name__ == '__main__':
    main()
