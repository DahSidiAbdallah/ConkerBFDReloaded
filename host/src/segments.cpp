// The game's code sections, and the TLB-mapped pages its code reads data from.
//
// Conker runs .init through a TLB alias at 0x10001000 and demand-pages .game
// (0x15000000) and .debugger (0x16000000). The runtime has no TLB, so each section
// is registered at the address its code uses, and the original .game/.debugger
// bytes are placed at those addresses for the hand-written code's lookup tables.
// Approach from CBFD-Recompiled's overlays.cpp (MIT, Sean Ciaschi).

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#   include <Windows.h>
#else
#   include <sys/mman.h>
#endif

#include "librecomp/overlays.hpp"
#include "conker.hpp"

extern "C" {
    // segments.c, written by tools/emit_segments.py.
    extern const uint32_t conker_game_segment_vram, conker_game_segment_rom;
    extern const size_t conker_game_segment_size;
    extern const uint32_t conker_game_segment[];
    extern const uint32_t conker_debugger_segment_vram, conker_debugger_segment_rom;
    extern const size_t conker_debugger_segment_size;
    extern const uint32_t conker_debugger_segment[];
    extern const uint32_t conker_code_rewrites[][2];
    extern const size_t conker_code_rewrite_count;
}

// N64Recomp's section tables: section_table, num_sections, overlay_sections_by_index.
#include "recomp_overlays.inl"

void conker::register_code_sections() {
    recomp::overlays::register_overlays(
        { .code_sections = section_table, .num_code_sections = ARRLEN(section_table), .total_num_sections = num_sections },
        { .table = overlay_sections_by_index, .len = ARRLEN(overlay_sections_by_index) });
}

void conker::register_tlb_code() {
    load_overlays(0x00001000, 0x10001000, 0x000280D0); // .init
    load_overlays(0x0002D4B0, 0x15000000, 0x001FA130); // .game
    load_overlays(0x00255880, 0x16000000, 0x00004D58); // .debugger
}

// The runtime reserves the whole 32-bit space but only maps RDRAM: make the
// segment's range readable and copy the original words in (host word order).
static void map_segment(uint8_t* rdram, uint32_t vram, const uint32_t* words, size_t size) {
    uint8_t* host = rdram + ((int64_t)(int32_t)vram + 0x80000000LL);
    constexpr uintptr_t page = 0x10000;
    uint8_t* begin = (uint8_t*)((uintptr_t)host & ~(page - 1));
    uint8_t* end = (uint8_t*)(((uintptr_t)host + size + page - 1) & ~(page - 1));
#ifdef _WIN32
    DWORD old;
    bool ok = VirtualProtect(begin, end - begin, PAGE_READWRITE, &old) != 0;
#else
    bool ok = mprotect(begin, end - begin, PROT_READ | PROT_WRITE) == 0;
#endif
    if (!ok) {
        std::fprintf(stderr, "[segments] could not map 0x%08X\n", vram);
        return;
    }
    std::memcpy(host, words, size);
}

void conker::map_tlb_pages(uint8_t* rdram) {
    map_segment(rdram, conker_game_segment_vram, conker_game_segment, conker_game_segment_size);
    map_segment(rdram, conker_debugger_segment_vram, conker_debugger_segment, conker_debugger_segment_size);
}

// The ROM as the recompiled code's layout sees it (.game is compressed in the real
// ROM), for mods that rebuild a hooked function from its original instructions.
std::vector<uint8_t> conker::unpacked_rom(std::span<const uint8_t> rom) {
    std::vector<uint8_t> out(rom.begin(), rom.end());
    auto put = [&out](uint32_t addr, uint32_t word) {
        if (out.size() < addr + 4) {
            out.resize(addr + 4);
        }
        for (int i = 0; i < 4; i++) {
            out[addr + i] = (uint8_t)(word >> (24 - 8 * i));
        }
    };
    for (size_t i = 0; i < conker_game_segment_size / 4; i++) {
        put(conker_game_segment_rom + 4 * i, conker_game_segment[i]);
    }
    for (size_t i = 0; i < conker_debugger_segment_size / 4; i++) {
        put(conker_debugger_segment_rom + 4 * i, conker_debugger_segment[i]);
    }
    for (size_t i = 0; i < conker_code_rewrite_count; i++) {
        put(conker_code_rewrites[i][0], conker_code_rewrites[i][1]);
    }
    return out;
}
