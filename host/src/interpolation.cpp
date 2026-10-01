// From CBFD-Recompiled (Copyright (c) 2026 Sean Ciaschi, MIT License; see
// recomp/THIRD_PARTY_LICENSE), used unchanged except for this note.

// Frame interpolation help, called from hooks in conker.toml.
//
// RT64 draws frames between the game's by pairing each transform with one from the
// previous frame. Without help it guesses, from draw calls that look alike, and the
// game batches a character's triangles differently from frame to frame, so some of
// Conker's parts were paired with others and jumped about. func_1502CCFC draws a game
// object (from gObjects) into a display list: its commands are wrapped in an RT64
// matrix group whose ID names the object, with linear ordering, so the n-th matrix
// of an object is always paired with the n-th of the same object last frame. An
// object whose count of matrices changed isn't paired at all for that frame (RT64).
//
// A character's shadow (func_15186794) is the ground under it, clipped anew every
// frame and drawn with the shadow's texture projected onto it: its own group asks RT64
// to interpolate its texture coordinates from last frame's projection (rt64.patch),
// so the shadow slides with the character instead of stepping at the game's rate.

#include <cstdint>

#include "recomp.h"

namespace {
    // RT64's extended GBI (tools/rt64/include/rt64_extended_gbi.h) for F3DEX2,
    // whose no-op (0xE0) carries RT64's hooks.
    constexpr uint32_t rt64_hook_opcode = 0xE0;
    constexpr uint32_t rt64_hook_magic = 0x525464;
    constexpr uint32_t rt64_hook_op_enable = 0x1;
    constexpr uint32_t rt64_extended_opcode = 0x64;
    constexpr uint32_t g_ex_matrixgroup_v1 = 0x00000C;
    constexpr uint32_t g_ex_popmatrixgroup_v1 = 0x00000D;

    constexpr uint32_t g_ex_push = 1;
    constexpr uint32_t g_ex_interpolate_simple = 0;
    constexpr uint32_t g_ex_interpolate_decompose = 1;
    constexpr uint32_t g_ex_component_skip = 0;
    constexpr uint32_t g_ex_component_interpolate = 1;
    constexpr uint32_t g_ex_component_auto = 2;
    constexpr uint32_t g_ex_order_linear = 0;

    // gEXMatrixGroup's parameters for the model matrix, matched in the order drawn:
    // mode (bit 2), the matrices' position, rotation, scale, skew and perspective,
    // vertices, tile, ordering (bit 17), texture coordinates (bit 22) and look-at.
    constexpr uint32_t group_params(uint32_t mode, uint32_t matrices, uint32_t vertices, uint32_t texcoords) {
        return (g_ex_push << 0) | (mode << 2) |
            (matrices << 3) | (matrices << 5) | (matrices << 7) |
            (matrices << 9) | (matrices << 11) | (vertices << 13) |
            (g_ex_component_auto << 15) | (g_ex_order_linear << 17) | (texcoords << 22) |
            (g_ex_component_auto << 24);
    }

    // An object: every component interpolated automatically, vertices too (the game
    // bends joints on the CPU), texture coordinates as they are.
    constexpr uint32_t object_group_params = group_params(g_ex_interpolate_decompose, g_ex_component_auto, g_ex_component_auto, g_ex_component_skip);
    // A shadow: the ground it's drawn on stays where it is (its matrices and vertices as they
    // are), and only its texture coordinates are interpolated.
    constexpr uint32_t shadow_group_params = group_params(g_ex_interpolate_simple, g_ex_component_skip, g_ex_component_skip, g_ex_component_interpolate);

    void put_command(uint8_t* rdram, gpr& dl, uint32_t w0, uint32_t w1) {
        MEM_W(0, dl) = (int32_t)w0;
        MEM_W(4, dl) = (int32_t)w1;
        dl += 8;
    }

    // Opens a matrix group at dl, with RT64's extended GBI enabled (RT64 turns it off
    // at the start of every display list).
    void put_group(uint8_t* rdram, gpr& dl, uint32_t id, uint32_t params) {
        put_command(rdram, dl, (rt64_hook_opcode << 24) | rt64_hook_magic, (rt64_hook_op_enable << 28) | rt64_extended_opcode);
        put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_matrixgroup_v1, id);
        put_command(rdram, dl, params, 0);
    }

    void put_pop(uint8_t* rdram, gpr& dl) {
        put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_popmatrixgroup_v1, 1);
    }
}

// func_1502CCFC, after its first instruction (the stack frame): $a0 is where it
// writes its first command, $a1 the object's index, and its 7th argument (0x168 on
// the new stack) tells its callers apart (the object, its shadow...). The group's
// commands go first, and the function writes after them. IDs are neither 0
// (G_EX_ID_IGNORE) nor 0xFFFFFFFF (G_EX_ID_AUTO).
extern "C" void conker_object_matrix_group_begin(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t object = (uint32_t)ctx->r5 & 0xFFFF;
    const uint32_t kind = (uint32_t)MEM_W(0x168, ctx->r29) & 0xFF;
    gpr dl = ctx->r4;
    put_group(rdram, dl, 0x43000000u | (kind << 16) | object, object_group_params);
    ctx->r4 = dl;
}

// At its return: $v0 is the end of what it wrote. The group is popped after it.
extern "C" void conker_object_matrix_group_end(uint8_t* rdram, recomp_context* ctx) {
    gpr dl = ctx->r2;
    put_pop(rdram, dl);
    ctx->r2 = dl;
}

// func_15186794, after its first instruction: $a0 is where it writes its first
// command, $a1 the shadow's index.
extern "C" void conker_shadow_matrix_group_begin(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t shadow = (uint32_t)ctx->r5 & 0xFFFF;
    gpr dl = ctx->r4;
    put_group(rdram, dl, 0x53480000u | shadow, shadow_group_params);
    ctx->r4 = dl;
}

// At its return: $v0 is the end of what it wrote.
extern "C" void conker_shadow_matrix_group_end(uint8_t* rdram, recomp_context* ctx) {
    gpr dl = ctx->r2;
    put_pop(rdram, dl);
    ctx->r2 = dl;
}
