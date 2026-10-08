// Ledge Grab (Accessibility tab): when Conker walks off anything high (not a step), he catches the
// edge and hangs on, as he does on the barn's beams and some wooden platforms; and coming down from
// a jump just short of an edge, he catches it instead of falling back.
//
// The game already has the move. Falling, its movement step asks the collision for an edge to
// grab (func_15044380's second pass, func_150AC3E4), and if one is found func_1504CB98 turns Conker
// to it, hangs him from it (animation 0x42) and lets him climb up or drop. But the search only
// looks at ground triangles marked as grabbable (their flags, D_800DBE5C, with all of 0x0E000000
// set: the mask func_150AC3E4 passes the shared triangle walk at 0x150AC474). With the option on,
// when Conker has just walked off a real drop or comes down from a jump, the mask is cleared so
// every ground edge counts; everything else (where he hangs, the animations, climbing up,
// dropping) is the game's own.
//
// The search only takes an edge within 21 units of him (D_8009F6FC = 21 squared) once it's at his
// hands' height; walking off at speed he'd be 80 or 90 units out by then (on the game's own grab
// spots you're creeping along a beam). So his forward speed stops as he steps off: he drops
// straight down beside the edge, which is also what catching yourself looks like.
//
// The search only knows the level's own ground, not objects such as crates and boxes, which the
// game collides with separately. Walking off one of those, the edge is the one he just crossed: the
// last spot he stood on and the first he was in the air are on either side of it. When his hands
// come down to its height and the search found nothing, that edge is handed to the game's hang
// (func_1504CB98 reads the search's results at 0x15050ACC) the way the search would: the edge's
// height, the point he hangs from, and the way he faces (back toward it). The game hangs him where
// the point is, 79 below the top (its own grabs leave him about 14 units past the edge).
//
// One catch per fall: once he has hung, letting go or dropping doesn't catch again until he's back
// on the ground (otherwise letting go took several presses).
//
// A safety net: hanging (0x42), climbing up (0x40) and hopping up (0xEA) never take him down, so if
// he's been in them a while and has slid down well below where he was, he's holding on to nothing (a
// player saw it once, near Bat's Tower's cogs: stuck in the climb, sinking down the wall). Then he
// lets go and falls as usual.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "conker.hpp"


namespace {
    constexpr int32_t player = (int32_t)0x800CC2D0;
    constexpr float walking_gravity = 5.0f; // walking off keeps it; a jump sets 6.2 (the spin 1.0)
    constexpr float high_enough = 100.0f;   // a drop bigger than this counts (steps and stairs don't; he hangs ~80 below the edge)
    constexpr float reach_fall = 160.0f;    // how far into the fall his hands can still reach the edge
    constexpr uint16_t hanging_animation = 0x42;
    // The search's results (func_150AC3E4), as func_1504CB98 reads them.
    constexpr int32_t found_edge_a = (int32_t)0x800CBDF4;   // -32768: none
    constexpr int32_t found_edge_b = (int32_t)0x800CBDF8;   // the edge's height + ... (he hangs 25 below it)
    constexpr int32_t found_angle_b = (int32_t)0x800CBD98;  // degrees; he faces it + 90
    constexpr int32_t found_x = (int32_t)0x800CBDFC;
    constexpr int32_t found_z = (int32_t)0x800CBD90;
    constexpr float none_found = -32768.0f;
    // Where the game hangs him from an edge it found: 79 below its top (so the result is top - 54,
    // less the 25 it takes off), a little past it.
    constexpr float hang_result_below_top = 54.0f;
    constexpr float hands_reach_below_top = 75.0f; // his hands come to the edge's height about here
    // Where he hangs: a little out from the last spot he stood on. He keeps his footing until his
    // middle is a little past the edge, so that spot is from about 10 inside the edge to 6 past it
    // (measured on a crate's edge); 6 more puts his hands at the edge within a few units, close
    // enough that climbing up (which carries him about 30 forward) lands him on top.
    constexpr float hang_from_last_ground = 6.0f;

    struct Fall {
        bool grounded_seen = false;
        float last_ground[2] = {};  // the last spot he stood on
        bool walked_off = false;    // this frame: walking off a real drop
        bool first_air_seen = false;
        float first_air[2] = {};    // the first spot he was in the air
        uint16_t facing = 0;        // his facing as he stepped off
        bool caught = false;        // he has hung since he was last on the ground
    } fall;

    float real(uint8_t* rdram, int32_t address) {
        const int32_t bits = MEM_W(0, (gpr)address);
        float value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }

    void set_real(uint8_t* rdram, int32_t address, float value) {
        int32_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        MEM_W(0, (gpr)address) = bits;
    }

    uint16_t animation(uint8_t* rdram) {
        return (uint16_t)((uint32_t)MEM_W(0, (gpr)(player + 0x84)) >> 16);
    }

    constexpr uint16_t climbing_animation = 0x40, hopping_animation = 0xEA, falling_animation = 0x38;
    constexpr int32_t stuck_after = 48;    // area timer ticks (60 a second) in those animations
    constexpr float stuck_sink = 40.0f;    // this far below the highest he was in them
    struct Hold {
        bool on = false;
        int32_t since = 0;
        float highest = 0.0f;
    } hold;
}

extern "C" void func_1505E650(uint8_t* rdram, recomp_context* ctx); // the game: set an object's animation

// Each frame, at its start (testing.cpp's frame hook): the safety net (above).
extern "C" void conker_ledge_grab_watchdog(uint8_t* rdram, recomp_context* ctx) {
    const uint16_t anim = animation(rdram);
    const bool holding = anim == hanging_animation || anim == climbing_animation || anim == hopping_animation;
    if (!holding || !conker::qol::ledge_grab()) {
        hold.on = false;
        return;
    }
    const int32_t timer = MEM_W(0, (gpr)(int32_t)0x800E0A90);
    const float y = real(rdram, player + 0x18);
    if (!hold.on || timer < hold.since) {
        hold = { true, timer, y };
        return;
    }
    hold.highest = std::max(hold.highest, y);
    if (timer - hold.since < stuck_after || hold.highest - y < stuck_sink) {
        return;
    }
    // Let go: the falling animation, in the air as when walking off, no speed left over.
    recomp_context c = *ctx;
    c.r29 = ctx->r29 - 0x40;
    MEM_W(0x10, c.r29) = 0;
    MEM_W(0x14, c.r29) = 0;
    MEM_W(0x18, c.r29) = 0;
    c.r4 = player; c.r5 = falling_animation; c.r6 = 0x3F800000; c.r7 = 0; // 1.0f, 0.0f
    func_1505E650(rdram, &c);
    MEM_B(0x100, (gpr)player) = 0x11;
    set_real(rdram, player + 0x20, 0.0f);
    set_real(rdram, player + 0x24, walking_gravity);
    set_real(rdram, player + 0x1CC, y); // a fall from here, not from the ledge
    fall.caught = true;
    hold.on = false;
    std::printf("[grab] safety net: let go after sliding %.0f below the hold (animation %02X)\n", hold.highest - y, anim);
}

// func_150AC3E4 before 0x150AC474: $t0 is the mask about to be stored, $a3 the object falling.
// It runs every frame for Conker (several times: once per collision layer), on the ground too.
extern "C" void conker_ledge_grab_mask(uint8_t* rdram, recomp_context* ctx) {
    static const bool debug = std::getenv("CONKER_GRAB_DEBUG") != nullptr;
    if (debug && (int32_t)ctx->r7 == player) {
        std::printf("[grab] %.2fs search: flags %02X gravity %.2f from %.1f ground %.1f at %.1f %.1f %.1f anim %02X caught %d\n", conker::testing::game_seconds(),
            (uint32_t)MEM_W(0, (gpr)(player + 0x100)) >> 24, real(rdram, player + 0x24), real(rdram, player + 0x1CC), real(rdram, player + 0x180),
            real(rdram, player + 0x14), real(rdram, player + 0x18), real(rdram, player + 0x1C), animation(rdram), (int)fall.caught);
    }
    if ((int32_t)ctx->r7 != player) {
        return;
    }
    fall.walked_off = false;
    const uint8_t flags = (uint8_t)((uint32_t)MEM_W(0, (gpr)(player + 0x100)) >> 24);
    const float x = real(rdram, player + 0x14), z = real(rdram, player + 0x1C);
    if (flags == 0x01) { // on the ground
        fall.grounded_seen = true;
        fall.last_ground[0] = x;
        fall.last_ground[1] = z;
        fall.first_air_seen = false;
        fall.caught = false;
        fall.facing = (uint16_t)MEM_HU(0, (gpr)(player + 0x76));
        return;
    }
    if (animation(rdram) == hanging_animation) {
        fall.caught = true;
    }
    if (!conker::qol::ledge_grab() || fall.caught) {
        return;
    }
    const float vertical_speed = real(rdram, player + 0x20);
    // Coming down from a jump (0x00; the tail spin too): any edge level with his hands. Clearing the
    // edge, he lands on top as usual; just short of it, he catches it. Not on the way up (+0x20, his
    // vertical speed, above zero), and low steps never come level with his hands.
    if (flags == 0x00 && vertical_speed < 0.0f) {
        ctx->r8 = 0;
        return;
    }
    // Walked off: 0x11 as he steps off, 0x10 once the fall goes on (a moment in, which can be before
    // his hands reach the edge's height, on a slow step off); 0x10 is also hanging, which doesn't move
    // down. Not 0x12: letting go of an edge (pushing away).
    const bool walked_off = flags == 0x11 || (flags == 0x10 && vertical_speed < 0.0f);
    if (!walked_off || real(rdram, player + 0x24) != walking_gravity) {
        return;
    }
    // How far below where he fell from (+0x1CC) the ground under him is (+0x180).
    if (real(rdram, player + 0x1CC) - real(rdram, player + 0x180) <= high_enough) {
        return;
    }
    ctx->r8 = 0; // every ground edge may be grabbed
    fall.walked_off = fall.grounded_seen;
    if (!fall.first_air_seen) {
        fall.first_air_seen = true;
        fall.first_air[0] = x;
        fall.first_air[1] = z;
    }
    // The first part of the fall: no more forward speed (+0x3C, +0x44, +0x1F4: what moved him on).
    if (real(rdram, player + 0x1CC) - real(rdram, player + 0x18) < reach_fall) {
        const int32_t zero = 0;
        MEM_W(0, (gpr)(player + 0x3C)) = zero;
        MEM_W(0, (gpr)(player + 0x44)) = zero;
        MEM_W(0, (gpr)(player + 0x1F4)) = zero;
    }
}

// func_1504CB98 at 0x15050ACC, about to read the grab search's results: walked off an object (a
// crate, a box) the search can't see, the edge he crossed is handed over once his hands reach it.
extern "C" void conker_ledge_grab_found(uint8_t* rdram, recomp_context* ctx) {
    static const bool debug = std::getenv("CONKER_GRAB_DEBUG") != nullptr;
    if ((int32_t)ctx->r16 != player) {
        return;
    }
    if (debug) {
        std::printf("[grab] %.2fs check: edge %.1f / %.1f, y %.1f\n", conker::testing::game_seconds(),
            real(rdram, found_edge_a), real(rdram, found_edge_b), real(rdram, player + 0x18));
    }
    if (!conker::qol::ledge_grab() || !fall.walked_off || fall.caught || !fall.first_air_seen) {
        return;
    }
    if (real(rdram, found_edge_a) != none_found || real(rdram, found_edge_b) != none_found) {
        return; // the search found an edge: the game's own
    }
    const float top = real(rdram, player + 0x1CC);
    if (real(rdram, player + 0x18) > top - hands_reach_below_top) {
        return; // his hands aren't down to it yet
    }
    // Which way he went: from the last spot he stood on to the first he was in the air.
    float dx = fall.first_air[0] - fall.last_ground[0], dz = fall.first_air[1] - fall.last_ground[1];
    const float step = std::sqrt(dx * dx + dz * dz);
    if (step < 0.5f) {
        return;
    }
    dx /= step;
    dz /= step;
    // Facing back toward the edge: his facing as he stepped off, turned around. The game turns him to
    // his heading (+0x40, degrees) + the found angle + 90 degrees, in 256ths of a turn (the extra half
    // step keeps its rounding down from landing one short).
    const uint16_t facing = (uint16_t)(fall.facing + 0x8000);
    const float angle = ((facing >> 8) + 0.5f) * 1.40625f - 90.0f - real(rdram, player + 0x40);
    set_real(rdram, found_edge_b, top - hang_result_below_top);
    set_real(rdram, found_angle_b, angle);
    set_real(rdram, found_x, fall.last_ground[0] + dx * hang_from_last_ground);
    set_real(rdram, found_z, fall.last_ground[1] + dz * hang_from_last_ground);
    if (debug) {
        std::printf("[grab] handed over the edge he walked off: top %.1f step %.1f facing %04X\n", top, step, facing);
    }
}
