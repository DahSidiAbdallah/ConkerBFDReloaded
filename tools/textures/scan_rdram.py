"""Find a GLideN64 texture pack's pictures in snapshots of the game's memory, so they can be
replaced without first being seen in the game (menus not unlocked yet, HUD pieces that only
show now and then).
  scan_rdram.py PACK.htc OUT_DIR TEMPLATE_DUMP_DIR... --memory SNAPSHOT.rdram...
TEMPLATE_DUMP_DIR: RT64 texture dumps (RT64_DUMP_TEXTURES); the 2D ones (rect_hashes.txt) are
the kinds looked for: same format, size and way of loading. SNAPSHOT: CONKER_RDRAM_DUMP files.
For each kind, rice_scan (built next to this file from rice_scan.c) tries every place in memory
against the pack's fingerprints; each picture found is written to OUT_DIR as an RT64 dump
(tile, texture memory as RT64 loads it, memory bytes), for RT64's texture_hasher (--rice) to
name, like a real dump. Each dump's .rice.json keeps where it was found (the snapshot's number in
"scanName", the address in "texture"), for tile_layout.py --memory."""
import gzip
import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def pack_fingerprints(path):
    """The pack's fingerprints by picture format: {(fmt, siz): {crc}} (no palette ones)."""
    crcs = {}
    with gzip.open(path, 'rb') as f:
        f.read(8)
        while True:
            head = f.read(31)
            if len(head) < 31:
                break
            checksum, w, h, fmt, gfmt, gtype, hires, n64fs, size = struct.unpack('<QIIIHHBHI', head)
            if checksum >> 32 == 0: # no palette
                crcs.setdefault((n64fs & 0xFF, n64fs >> 8), set()).add(checksum & 0xFFFFFFFF)
            f.seek(size, 1)
    return crcs


def rice_size(tile, load):
    """Width, height and bytes per row RT64's texture_hasher hashes (block loads)."""
    tw = max((tile['lrs'] >> 2) - (tile['uls'] >> 2), 0) + 1
    th = max((tile['lrt'] >> 2) - (tile['ult'] >> 2), 0) + 1
    mw = tw if tile['masks'] == 0 else 1 << tile['masks']
    mh = th if tile['maskt'] == 0 else 1 << tile['maskt']
    clamps = tile['masks'] == 0 or (tile['cms'] & 2)
    clampt = tile['maskt'] == 0 or (tile['cmt'] & 2)
    width = min(mw, tw) if clamps and tw <= 256 else mw
    height = min(mh, th) if (clampt and th <= 256) or mh > 256 else mh
    if tile['siz'] == 3:
        bpl = tile['line'] << 4
    elif load['tile']['lrt'] == 0:
        bpl = tile['line'] << 3
    else:
        return None # (reverse DXT: not needed for the kinds here)
    return width, height, bpl


def load_block(tmem, mem, address, load):
    """RT64's load block (rt64_rdp.cpp loadToTMEMCommon) into tmem (bytearray, 4096)."""
    lt, tex = load['tile'], load['texture']
    rgba32 = lt['siz'] == 3 and lt['fmt'] == 0
    bytes_per_row = tex['width'] << tex['siz'] >> 1
    start = address + (lt['uls'] << tex['siz'] >> 1) + bytes_per_row * lt['ult']
    words = ((lt['lrs'] - lt['uls']) >> (4 - lt['siz'])) + 1
    stride = lt['line'] << 3
    mask, advance = (2047, 4) if rgba32 else (4095, 8)
    t, a, xor, dxt = start, (lt['tmem'] << 3) & mask, 0, 0
    for _ in range(words):
        if rgba32:
            for i, src in enumerate((0, 1, 4, 5)):
                tmem[(a + i) ^ xor] = mem[(t + src) ^ 3]
            for i, src in enumerate((2, 3, 6, 7)):
                tmem[((a + i) ^ xor) | 2048] = mem[(t + src) ^ 3]
        else:
            for i in range(8):
                tmem[(a + i) ^ xor] = mem[(t + i) ^ 3]
        dxt += lt['lrt']
        while dxt >= 0x800:
            a = (a + stride) & mask
            dxt -= 0x800
            xor ^= 4
        t += 8
        a = (a + advance) & mask


def main():
    args = sys.argv[1:]
    mi = args.index('--memory')
    pack, out, dumps, memories = args[0], args[1], args[2:mi], args[mi + 1:]
    os.makedirs(out, exist_ok=True)
    scanner = os.path.join(HERE, 'rice_scan')
    if not os.path.exists(scanner) or os.path.getmtime(scanner) < os.path.getmtime(scanner + '.c'):
        subprocess.run(['gcc', '-O2', '-o', scanner, scanner + '.c'], check=True)
    crcs = pack_fingerprints(pack)
    print(sum(len(v) for v in crcs.values()), 'fingerprints in the pack')
    # The kinds of 2D picture: one template each.
    kinds = {}
    for d in dumps:
        rects = set(l.strip() for l in open(os.path.join(d, 'rect_hashes.txt'))) if os.path.exists(os.path.join(d, 'rect_hashes.txt')) else set()
        for n in os.listdir(d):
            if not n.endswith('.tile.json') or n.split('.')[0] not in rects:
                continue
            base = os.path.join(d, n[:-len('.tile.json')])
            if not os.path.exists(base + '.rice.json'):
                continue
            info, load = json.load(open(base + '.tile.json')), json.load(open(base + '.rice.json'))
            if load['type'] != 'Block' or info.get('tlut', 'None') != 'None':
                continue
            if json.load(open(base + '.rice.json'))['texture']['width'] >= 200:
                continue # pictures of the screen
            key = json.dumps([info['tile'], info['width'], info['height'], {k: v for k, v in load['tile'].items()}, load['texture']['fmt'], load['texture']['siz'], load['texture']['width']], sort_keys=True)
            kinds.setdefault(key, (base, info, load))
    print(len(kinds), 'kinds of 2D picture')
    found = {} # (memory, address) -> name
    seen = set() # (kind, fingerprint): each picture once
    for key, (base, info, load) in kinds.items():
        size = rice_size(info['tile'], load)
        if size is None:
            continue
        width, height, bpl = size
        crc_args = ['%08x' % c for c in sorted(crcs.get((info['tile']['fmt'], info['tile']['siz']), ()))]
        if not crc_args:
            continue
        rdram_len = os.path.getsize(base + '.rice.rdram')
        template_tmem = open(base + '.tmem', 'rb').read()
        for mi_, mem_path in enumerate(memories):
            res = subprocess.run([scanner, mem_path, str(width), str(height), str(info['tile']['siz']), str(bpl), '8'] + crc_args,
                                 capture_output=True, text=True, check=True).stdout.split()
            if not res:
                continue
            mem = open(mem_path, 'rb').read()
            # A fingerprint found all over memory is of blank data (e.g. an empty picture).
            counts = {}
            for c in res[1::2]:
                counts[c] = counts.get(c, 0) + 1
            for i in range(0, len(res), 2):
                if counts[res[i + 1]] > 16:
                    continue
                address = int(res[i])
                name = f'scan{mi_}_{address:06x}_{len(found)}'
                if (key, res[i + 1]) in seen:
                    found[(mi_, address)] = (None, rdram_len, info['width'], info['height'])
                    continue
                seen.add((key, res[i + 1]))
                tmem = bytearray(template_tmem)
                load_block(tmem, mem, address, load)
                l2 = dict(load); l2['texture'] = dict(load['texture']); l2['texture']['address'] = address
                l2['scanName'] = name # (texture_hasher renames the files by hash; this stays)
                json.dump(info, open(os.path.join(out, name + '.tile.json'), 'w'), indent=1)
                json.dump(l2, open(os.path.join(out, name + '.rice.json'), 'w'), indent=1)
                open(os.path.join(out, name + '.rice.rdram'), 'wb').write(mem[address:address + rdram_len])
                open(os.path.join(out, name + '.tmem'), 'wb').write(bytes(tmem))
                found[(mi_, address)] = (name, rdram_len, info['width'], info['height'])
    print(len(found), 'places found,', len(seen), 'different pictures')


if __name__ == '__main__':
    main()
