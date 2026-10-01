/* Finds textures in a snapshot of the game's memory by their Rice fingerprints (the ones
   GLideN64 texture packs are named by), for scan_rdram.py.
     rice_scan MEMORY WIDTH HEIGHT SIZ BPL STEP CRCS...
   MEMORY: 8 MB as RT64 keeps it (CONKER_RDRAM_DUMP). The fingerprint is RiceCRC32 over HEIGHT
   rows of WIDTH texels (SIZ: 0 4-bit ... 3 32-bit), BPL bytes apart, as RT64's texture_hasher
   computes it. Tries every STEP bytes; prints "address crc" for each one in CRCS (hex).
   Build: gcc -O2 -o rice_scan rice_scan.c */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rice_crc32(const uint8_t *src, int width, int height, int size, int row_stride) {
    uint32_t crc = 0;
    const int bytes_per_line = width << size >> 1;
    for (int y = height - 1; y >= 0; y--) {
        uint32_t esi = 0;
        for (int x = bytes_per_line - 4; x >= 0; x -= 4) {
            memcpy(&esi, src + x, 4);
            esi ^= (uint32_t)x;
            crc = (crc << 4) + ((crc >> 28) & 15);
            crc += esi;
        }
        esi ^= (uint32_t)y;
        crc += esi;
        src += row_stride;
    }
    return crc;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr, "rice_scan MEMORY WIDTH HEIGHT SIZ BPL STEP CRCS...\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    static uint8_t mem[8 * 1024 * 1024];
    size_t n = fread(mem, 1, sizeof(mem), f);
    fclose(f);
    int width = atoi(argv[2]), height = atoi(argv[3]), siz = atoi(argv[4]), bpl = atoi(argv[5]), step = atoi(argv[6]);
    int count = argc - 7;
    uint32_t *crcs = malloc(sizeof(uint32_t) * count);
    for (int i = 0; i < count; i++) {
        crcs[i] = (uint32_t)strtoul(argv[7 + i], NULL, 16);
    }
    qsort(crcs, count, sizeof(uint32_t), cmp_u32);
    size_t span = (size_t)(height - 1) * bpl + (width << siz >> 1);
    for (size_t a = 0; a + span <= n; a += step) {
        uint32_t crc = rice_crc32(mem + a, width, height, siz, bpl);
        if (bsearch(&crc, crcs, count, sizeof(uint32_t), cmp_u32)) {
            printf("%zu %08x\n", a, crc);
        }
    }
    return 0;
}
