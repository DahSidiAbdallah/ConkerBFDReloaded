// Aiming: Move While Shooting (General tab, look_aim.cpp's options): with the shotgun, Conker keeps walking
// as he fires, as in the later remakes, where on the N64 a shot stopped him for a second. With Swap Sticks as
// well, the right stick aims and the left moves the whole time: twin-stick aiming.
//
// The shotgun is Conker's weapon state 0x3B (+0x31C's +0x78). Its state code (func_15065A5C's case for 0x3B)
// picks a shot when Z is pressed: 0x222 or 0x236 from the hip, 0x227 or 0x237 aimed with R, and the gun's wait
// (+0x31C's +0x19E) is set to 60 and counts down, no other shot until it's out. The shot animation stands
// still for its second (the blast, then the pump), whatever the stick does: his speed (+0x3C) drops to 0.
// The blast is out within about a tenth of a second (screenshots 0.05 s apart: the flash, then the sparks
// where it hits); the rest is the recoil and the pump.
//
// So, with the option on and the stick pushed: through the blast his speed is kept at what it was as he
// fired, and once the blast is out (shot_moves_after of the gun's wait gone) the walk he was in is played
// again, his speed with it. The game's wait still counts down, so the shots come no faster than before.
// Standing still, the stick let go, a shot plays as in the original.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "recompinput/players.h"
#include "conker.hpp"

namespace {
    constexpr int32_t player = (int32_t)0x800CC2D0;
    constexpr uint8_t shotgun = 0x3B;
    constexpr int shot_wait = 60;           // the gun's wait as a shot starts
    constexpr int shot_moves_after = 12;    // of it gone (about 0.2 s): the blast is out, the walk plays again
    constexpr float pushed = 5.0f;          // the stick's push (+0x44, 35 at full) that counts as moving
    // The animation's speed after the blast (1 as it plays, about 30 of its 47 frames a second): the rest in about a
    // quarter of a second. Conker's +0x2D0 points at what his model is playing: +0x4 the animation (high half; the
    // one it's blending from in the low), +0x8 its frame, +0x10 its speed, +0x18 its length (as func_150649A0 reads).
    constexpr float shot_rest_speed = 6.0f;

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

    bool shot_animation(uint16_t animation) {
        return animation == 0x222 || animation == 0x236 || animation == 0x227 || animation == 0x237;
    }

    // The shotgun's walks: from the hip (0x224), aimed with R and Z (0x223, 0x31B; Swap Sticks' R alone too).
    bool walk_animation(uint16_t animation) {
        return animation == 0x224 || animation == 0x223 || animation == 0x31B;
    }

    struct {
        uint16_t walk = 0;      // the walk he was last in (0: not walking)
        float speed = 0.0f;     // and his speed in it
        bool moving = false;    // walking as this shot started
        bool done = false;      // this shot's walk is back
    } shot;
}

// Each frame, at its end (testing.cpp's frame hook).
extern "C" void conker_move_while_shooting(uint8_t* rdram, recomp_context* ctx) {
    const gpr state = (gpr)(int32_t)MEM_W(0, (gpr)(player + 0x31C));
    // (Single player: in multiplayer only player 1's shots would.)
    if (!conker::look_aim::move_while_shooting() || !recompinput::players::is_single_player_mode() ||
        (state & 0xFF000000u) != 0x80000000u ||
        (uint8_t)MEM_BU(0x78, state) != shotgun) {
        shot = {};
        return;
    }
    const uint16_t animation = (uint16_t)((uint32_t)MEM_W(0, (gpr)(player + 0x84)) >> 16);
    const float speed = real(rdram, player + 0x3C);
    const bool stick = real(rdram, player + 0x44) >= pushed;
    if (!shot_animation(animation)) {
        if (walk_animation(animation) && speed > 0.0f && stick) {
            shot.walk = animation;
            shot.speed = speed;
        } else {
            shot.walk = 0;
        }
        shot.moving = false;
        shot.done = false;
        return;
    }
    // A shot: was he walking as it started (the frame before was a walk)?
    const int wait = (int)(uint16_t)MEM_HU(0x19E, state);
    if (wait >= shot_wait - 1 && !shot.done) {
        shot.moving = shot.walk != 0;
    }
    if (!stick) {
        return;
    }
    if (shot.moving) {
        set_real(rdram, player + 0x3C, shot.speed);
    }
    if (shot.done) {
        return;
    }
    if (shot_wait - wait < shot_moves_after) {
        return;
    }
    // The blast is out: the rest (the recoil, the pump) played quicker, so the game's own walk comes back sooner.
    const int32_t playing = MEM_W(0, (gpr)(player + 0x2D0));
    if (((uint32_t)playing & 0xFF000000u) == 0x80000000u && (uint32_t)MEM_W(0x4, (gpr)playing) >> 16 == animation) {
        set_real(rdram, playing + 0x10, shot_rest_speed);
    }
    shot.done = true;
    static const bool trace = std::getenv("CONKER_CAM_TRACE") != nullptr;
    if (trace) {
        std::printf("[shoot] the rest of the shot %X quicker (wait %d)\n", animation, wait);
    }
}
