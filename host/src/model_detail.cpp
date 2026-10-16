// From CBFD-Recompiled V0.1.6 (Copyright (c) 2026 Sean Ciaschi, MIT License; see
// recomp/THIRD_PARTY_LICENSE), used unchanged except for this note; the option is in settings.cpp.
// Graphics: Conker's Detail, Conker kept at his most detailed model (called from a hook in conker.toml).
//
// Each character has a few models, from the most detailed down, and every game frame
// func_1502C6E8 picks one for each object by how far it is from the camera: under 500 units
// the first (level 0), then further levels at the distances in D_80096DE0 and D_80096DE4, and
// past 2000 the last. Nearly still (xz speed, +0x3C, under 3) a level from 2 on is moved one
// closer. Two flags override it: D_800C35EA set to 1 makes every object level 0, and in the
// multiplayer arenas (D_800BE616, set by func_15015F40 for scenes 26, 36, 43, 45, 48, 51, 52 and
// 63, with the player count) a character whose first model is 0, Conker, is kept at level 1.
// The level is clamped to the object's models (+0x2C8 is their count), and when it changed the
// new model is loaded (func_150837D4, with the model's number from the list at +0x2C4; the
// loaded one is then at +0x4) and the level stored at +0x1C8. An object whose level the game
// sets itself (+0x1C9, func_1502FBE8) isn't picked for.
//
// Conker is object 0, his models 0 to 4. Measured (each level picked logged as he walked about
// the first room of a save): he flipped between level 0 and 1 as the camera's distance crossed
// 500, and between 1 and 2 each time he started or stopped walking, from the nearly-still rule.
// With Always Highest he stays at level 0 in single player; multiplayer keeps the original's
// level 1, which the game chose for its split screens.

#include <cstdint>

#include "recomp.h"

#include "conker.hpp"

namespace {
    // Set in the multiplayer arenas (see the top): Conker is kept at level 1 there.
    constexpr uint32_t multiplayer_detail_flag = 0x800BE616;
}

// func_1502C6E8 at 0x1502C92C, with the level picked: $v1 is the level, $a3 the object
// (gObjects, 0x32C bytes each) and 0x50($sp) the object's index. The level is compared with the
// object's last after this, so a changed one loads the model as the game's own would. Level 0
// is always one of the object's models (+0x2C8, their count, is at least 1 for an object that
// gets here: the game clamps the level to count - 1 and stops if that's below 0).
extern "C" void conker_model_detail(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::model_detail::always_highest() || MEM_BU(0, (gpr)(int32_t)multiplayer_detail_flag) != 0) {
        return;
    }
    const gpr object = ctx->r7;
    const uint32_t index = (uint32_t)MEM_W(0x50, ctx->r29);
    const gpr models = (gpr)MEM_W(0x2C4, object);
    // Conker: the player's object, or a character whose first model is Conker's (the game's own
    // test, for its multiplayer rule).
    if (index == 0 || MEM_BU(0, models) == 0) {
        ctx->r3 = 0;
    }
}
