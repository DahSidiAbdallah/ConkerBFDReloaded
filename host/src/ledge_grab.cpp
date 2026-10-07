// Ledge Grab (Accessibility tab): when Conker walks off anything high (not a jump, not a step), he
// catches the edge and hangs on, as he does on the barn's beams and some wooden platforms.
//
// The game already has the move. Falling, its movement step asks the collision for an edge to
// grab (func_15044380's second pass, func_150AC3E4), and if one is found func_1504CB98 turns Conker
// to it, hangs him from it (animation 0x42) and lets him climb up or drop. But the search only
// looks at ground triangles marked as grabbable (their flags, D_800DBE5C, with all of 0x0E000000
// set: the mask func_150AC3E4 passes the shared triangle walk at 0x150AC474). With the option on,
// when Conker has just walked off a real drop, the mask is cleared so every ground edge counts;
// everything else (where he hangs, the animations, climbing up, dropping) is the game's own.
//
// The search only takes an edge within 21 units of him (D_8009F6FC = 21 squared) once it's at his
// hands' height; walking off at speed he'd be 80 or 90 units out by then (on the game's own grab
// spots you're creeping along a beam). So his forward speed stops as he steps off: he drops
// straight down beside the edge, which is also what catching yourself looks like.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "conker.hpp"

namespace {
    constexpr int32_t player = (int32_t)0x800CC2D0;
    constexpr float walking_gravity = 5.0f; // walking off keeps it; a jump sets 6.2 (the spin 1.0)
    constexpr float high_enough = 120.0f;   // a drop bigger than this counts (steps and stairs don't)
    constexpr float reach_fall = 160.0f;    // how far into the fall his hands can still reach the edge

    float real(uint8_t* rdram, int32_t address) {
        const int32_t bits = MEM_W(0, (gpr)address);
        float value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
}

// func_150AC3E4 before 0x150AC474: $t0 is the mask about to be stored, $a3 the object falling.
extern "C" void conker_ledge_grab_mask(uint8_t* rdram, recomp_context* ctx) {
    static const bool debug = std::getenv("CONKER_GRAB_DEBUG") != nullptr;
    if (debug && (int32_t)ctx->r7 == player) {
        std::printf("[grab] %.2fs search: flags %02X gravity %.2f from %.1f ground %.1f\n", conker::testing::game_seconds(),
            (uint32_t)MEM_W(0, (gpr)(player + 0x100)) >> 24, real(rdram, player + 0x24), real(rdram, player + 0x1CC), real(rdram, player + 0x180));
    }
    if (!conker::qol::ledge_grab() || (int32_t)ctx->r7 != player) {
        return;
    }
    const uint8_t flags = (uint8_t)((uint32_t)MEM_W(0, (gpr)(player + 0x100)) >> 24);
    if (flags != 0x11 || real(rdram, player + 0x24) != walking_gravity) {
        return; // not walked off (a jump is 0x00, standing 0x01)
    }
    // How far below where he fell from (+0x1CC) the ground under him is (+0x180).
    if (real(rdram, player + 0x1CC) - real(rdram, player + 0x180) <= high_enough) {
        return;
    }
    ctx->r8 = 0; // every ground edge may be grabbed
    // The first part of the fall: no more forward speed (+0x3C, +0x44, +0x1F4: what moved him on).
    if (real(rdram, player + 0x1CC) - real(rdram, player + 0x18) < reach_fall) {
        const int32_t zero = 0;
        MEM_W(0, (gpr)(player + 0x3C)) = zero;
        MEM_W(0, (gpr)(player + 0x44)) = zero;
        MEM_W(0, (gpr)(player + 0x1F4)) = zero;
    }
}

// (testing, CONKER_GRAB_DEBUG) func_1504CB98 at 0x15050ACC, about to read the grab search's result.
extern "C" void conker_ledge_grab_found(uint8_t* rdram, recomp_context* ctx) {
    static const bool debug = std::getenv("CONKER_GRAB_DEBUG") != nullptr;
    if (debug && (int32_t)ctx->r16 == player) {
        std::printf("[grab] %.2fs check: edge %.1f / %.1f, y %.1f\n", conker::testing::game_seconds(),
            real(rdram, (int32_t)0x800CBDF4), real(rdram, (int32_t)0x800CBDF8), real(rdram, player + 0x18));
    }
}
