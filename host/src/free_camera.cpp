// From CBFD-Recompiled's mouse camera (Copyright (c) 2026 Sean Ciaschi, MIT License; see
// recomp/THIRD_PARTY_LICENSE). Ours adds the right stick (the General tab's Free Camera
// option): it turns and tilts the same orbit, at up to stick_degrees_per_second.
//
// Free camera: a free orbit camera, called from hooks in recomp/conker.toml.
//
// RecompFrontend's General tab has a Mouse Sensitivity option. Above 0, the cursor is
// captured while the game is played (and released in the menus), and
// recompinput::get_mouse_deltas() gives the mouse's movement since the game last read
// its controllers, scaled by that sensitivity.
//
// The follow camera (struct108: gObjects[0].camera for player 1, D_800DBFF0 the one
// being played) looks at a point (+0x2BC) above its pivot at Conker's feet (+0x2A4),
// from an eye kept at a horizontal distance (+0x374) and height (+0x344) from the
// pivot. It doesn't keep its angle: every frame func_15125330 works it out (+0x37C)
// from where the eye is, and Conker's movement is relative to it. So once the mouse
// moves, the eye the camera wants (+0x2F8) is placed each frame from our own yaw and
// pitch around the look-at point, and the rest of the game follows. The camera stays
// where the mouse leaves it.
//
// Walls: the eye is placed as the game's camera collision (func_1512BB10) starts, the
// way the C-buttons' turning places it earlier in the same update (func_15122C5C). As in the
// HarbourMasters ports' free look, the player's angle is kept and only the distance gives: the
// eye goes as far out along its line from the look-at point as the camera's ball has room
// (ball_room), in at once and back out eased (in the view, func_151284C4). It's placed directly,
// so the game's collision (which slides the camera from last frame's eye and lets it through the
// barn's big posts) has nothing left to move.
//
// The orbit only runs where the C-buttons turn the camera (func_1512D390 ran this
// frame) and not in the look mode (func_15120158: hold R, aiming), so cutscenes, special
// cameras and aiming are the game's. Pressing C-left or C-right
// hands the camera back to the game until the mouse moves again.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include <SDL.h>

#include "recomp.h"
#include "recompinput/input_state.h"
#include "recompinput/input_types.h"

#include "recompinput/players.h"
#include "conker.hpp"

namespace {
    // Degrees per pixel of mouse movement at 100% sensitivity.
    constexpr float degrees_per_pixel = 0.2f;
    // The right stick (Free Camera): degrees a second at full tilt, and how far it must
    // be pushed before it turns the camera.
    constexpr float stick_degrees_per_second = 150.0f;
    constexpr float stick_dead_zone = 0.2f;
    // The stick's turning eases toward the speed it asks for, over about this many seconds,
    // so it starts and stops smoothly instead of all at once. Past the dead zone, half of
    // the speed grows with the push and half with its square: small pushes turn finely.
    constexpr float stick_ease_seconds = 0.1f;
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    // How far the camera may look up or down: pitch is the eye's angle above the look-at point.
    // Below it, the floor brings the eye in toward Conker (looking up at him), as far as the
    // camera's ball allows.
    constexpr float min_pitch = -35.0f * degrees_to_radians;
    constexpr float max_pitch = 75.0f * degrees_to_radians;
    // The stick tilts the camera this much slower than it turns it (as Zelda64Recompiled's).
    constexpr float stick_tilt_share = 0.5f;
    constexpr uint32_t current_camera = 0x800DBFF0; // D_800DBFF0
    // Scroll wheel zoom: each notch scales the distance by this, between the nearest and
    // farthest of the game's own camera distances (D_800A34B0: the controller's four,
    // each a horizontal distance and a height from the pivot, 530 x 400 the farthest).
    constexpr float zoom_step = 1.12f;
    constexpr uint32_t camera_distances = 0x800A34B0; // D_800A34B0, 4 x { horizontal, height }
    constexpr int camera_distance_count = 4;
    // The orbit's distance (the wheel's, the game's closer one in tight spots) glides to a new
    // value over about this many seconds instead of jumping there.
    constexpr float reach_ease_seconds = 0.2f;
    // Walls and tight spots: when something holds the eye nearer than it was, the view follows at
    // once (it mustn't go through walls); when there's room again, it eases back out over about
    // this many seconds, so a camera brushing along walls doesn't jump.
    constexpr float ease_out_seconds = 0.35f;
    // The camera's ball: the eye goes as far out along its line from the look-at point as a ball
    // of this radius fits without touching anything (walls, posts, floor, ceiling), as the game's
    // own camera keeps a 30-unit cylinder (+0x95C) clear for its 4:3 picture. The picture is cut
    // 40 units in front of the eye (the near plane), which in widescreen reaches about 33 to the
    // sides: so a little more here. Where even the smaller ones leave less than min_reach, the
    // eye takes the most room any of them gives.
    constexpr float ball_radii[] = { 36.0f, 20.0f, 8.0f };
    constexpr float min_reach = 60.0f;
    // Not below Conker's feet (+0x2A8): tilted down, the eye comes in along its line instead, staying
    // this far above the level of his feet (the near plane reaches about 19 below the eye), looking
    // up at him; there it stops tilting once it's come in to closest_tilted, with all of him in
    // view. Otherwise, standing near an edge, it went down past the edge and the ledge hid him.
    constexpr float above_feet = 24.0f;
    // And as far above the ground under the eye itself (the game's own camera keeps 40: func_1512BB10).
    constexpr float above_ground = 24.0f;
    constexpr float ground_step = 40.0f, ground_climb = 60.0f;
    constexpr float closest_tilted = 140.0f;
    // Tilting up further than that, the view aims up from the eye, to this angle above the horizon at most.
    constexpr float highest_view = 80.0f * degrees_to_radians;
    // Objects the game's camera collision counts but the ball doesn't (a pushable crate, It's War's metal boxes):
    // where the collision holds the eye nearer than it was placed (by more than held_slack), the orbit keeps to
    // that distance, plus held_margin, for held_hold frames, then lets it out held_step a frame. Placed back
    // behind the object each frame instead, the collision pushed the eye out of it and the view jumped in and
    // out every few frames (a player's snapshots: 193, 61, 110, 153, 193, 61...), as CBFD-Recompiled's camera keeps
    // to a wall that holds it.
    constexpr float held_slack = 8.0f;
    constexpr float held_margin = 10.0f;
    constexpr int held_hold = 15;
    constexpr float held_step = 6.0f;
    // The ball itself can see a moving object only on some frames (It's War's pushable metal box, being pushed:
    // 65, 391, 391, 391, 65... in a player's recording), and the view came in, eased out and came in again. So where
    // the ball's room comes in suddenly (by more than held_slack, short of the orbit's reach), the orbit keeps to it
    // for seen_hold frames, kept as long as the ball sees it again, then eases out as after a wall.
    constexpr int seen_hold = 10;
    // Slopes (as the HarbourMasters ports' camera does): walking up or down a slope, the view
    // tilts with it so the ground ahead shows: the eye rises (downhill, away from the camera: looking
    // down it) or comes down (uphill), by slope_follow of the slope's angle along the view, at most
    // slope_tilt_most. The slope is measured from Conker's own steps on the ground (each
    // slope_sample_step apart) and followed over slope_ease_seconds. (Only the angle: moving the
    // game's look-at point instead, the game eased its own from it and it crept away.)
    constexpr float slope_follow = 0.5f;
    constexpr float slope_tilt_most = 12.0f * degrees_to_radians;
    constexpr float slope_sample_step = 24.0f;
    constexpr float slope_ease_seconds = 0.5f;
    constexpr uint32_t player_object = 0x800CC2D0; // Conker (+0x14 position, +0x100 flags: 0x01 on the ground)
    // The farthest-room search: halvings of the step the ball can't make, and how near the ball
    // must end to its goal to have got there.
    constexpr int reach_halvings = 4;
    constexpr float reached_within = 2.0f;
    // A squeeze (no ball has min_reach of room at the player's angle: Conker down a narrow gap,
    // walls all around): the eye rises by squeeze_lift_step tries, as the game's camera does,
    // looking down at him, to the first that has; it comes back down over lift_ease_seconds once
    // there's room. Anywhere else the player's angle is kept.
    constexpr float squeeze_lift_step = 10.0f * degrees_to_radians;
    constexpr float lift_ease_seconds = 0.6f;
    // Before rising, it turns aside (a wall at the player's angle, often with rock above too, as at
    // the bend at the top of the slope outside the barn, where rising leaves it just above Conker's
    // head): by side_step tries up to side_most each way (on the side it last turned to first), to
    // the nearest angle with side_room of room; quickly (side_in_seconds), as there's nothing to see
    // where it was, and back to the player's angle over side_out_seconds once that has room.
    constexpr float side_step = 15.0f * degrees_to_radians;
    constexpr float side_most = 165.0f * degrees_to_radians;
    constexpr float side_room = 120.0f;
    constexpr float side_in_seconds = 0.12f;
    constexpr float side_out_seconds = 0.6f;

    // Scroll wheel notches since the view last read them (SDL event watch: the
    // frontend's own event loop consumes the events).
    std::atomic<int> wheel_notches = 0;
    bool zoom_click = false; // the right stick was clicked (the next distance)
    int SDLCALL watch_wheel(void*, SDL_Event* event) {
        if (event->type == SDL_MOUSEWHEEL) {
            wheel_notches.fetch_add(event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event->wheel.y : event->wheel.y);
        }
        return 1;
    }

    struct Orbit {
        bool engaged = false;
        bool follow_camera_ran = false; // func_1512D390 ran since the last view
        bool look_mode_ran = false;     // func_15120158 (hold R, aiming) ran since the last view
        bool turned = false;            // the mouse and wheel were read since the last view
        bool has_target = false;        // target and next_target hold eyes the orbit wanted
        float target[3] = {};           // the eye the orbit wanted last frame
        float next_target[3] = {};      // this frame's, kept as target once the frame ends
        float yaw = 0.0f;               // radians, the eye's direction from the look-at point
        float pitch = 0.0f;
        float wanted = 0.0f;            // the scroll wheel's distance from the look-at point
        bool zoomed = false;            // the player has zoomed (wanted is theirs from then on)
        double last_frame = 0.0;        // game seconds at the last orbit update
        float stick_speed[2] = {};      // the stick's eased turning, in mouse pixels a second
        bool placed_directly = false;   // this frame's eye was placed at the orbit's target
        float placed[3] = {};           // the eye placed for the collision this frame
        float shown_distance = 0.0f;    // the eye's eased distance from the look-at point (0: none yet)
        float reach = 0.0f;             // the orbit's eased distance (0: none yet)
        float lift = 0.0f;              // radians the eye is raised out of a squeeze
        float side = 0.0f;              // radians the eye is turned aside from the player's angle (walls)
        float aim = 0.0f;               // radians the view aims up past the lowest the eye can go (0: at the look-at point)
        float ground_at_eye = 0.0f;     // the ground last found under the eye's line, at its far end
        float held = 0.0f;              // the game's collision held the eye this near (0: not held), with:
        int held_frames = 0;            // frames left to keep to it, before letting it out held_step a frame
        bool held_seen = false;         // held where the ball saw something (seen_hold): let go at once, not by held_step
        bool step_known = false;        // step_from holds where Conker's last slope sample was
        float step_from[3] = {};
        float slope = 0.0f;             // the ground's rise per unit along the view, as last measured
        float shown_slope = 0.0f;       // eased
        double last_view = 0.0;         // game seconds at the last view
    } orbit;
    bool was_normal_camera = false; // conker::normal_camera (Camera: Field of View)

    float read_float(uint8_t* rdram, gpr base, int32_t offset) {
        uint32_t word = (uint32_t)MEM_W(offset, base);
        float value;
        std::memcpy(&value, &word, sizeof(value));
        return value;
    }

    void write_float(uint8_t* rdram, gpr base, int32_t offset, float value) {
        uint32_t word;
        std::memcpy(&word, &value, sizeof(word));
        MEM_W(offset, base) = (int32_t)word;
    }
}

// func_1512D390 (the C-buttons' turning), before its last restore: $s0 is the camera.
// Marks that the follow camera is running this frame, and hands the camera back to the
// game while C-left or C-right is held (+0x36C points at the buttons held).
extern "C" void conker_mouse_camera_follow(uint8_t* rdram, recomp_context* ctx) {
    const gpr camera = ctx->r16;
    static const bool probe = std::getenv("CONKER_PROBE") != nullptr;
    static int calls = 0;
    if (probe && calls++ % 60 == 0) {
        std::printf("[freecam] follow camera ran (camera %08X, current %08X)\n", (uint32_t)camera, (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera));
    }
    if ((uint32_t)camera != (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        return;
    }
    orbit.follow_camera_ran = true;
    const gpr buttons = (gpr)(int32_t)MEM_W(0x36C, camera);
    if (((uint32_t)MEM_HU(0, buttons) & 0x3) != 0) {
        orbit.engaged = false;
    }
}

// func_15120158 (the look mode: hold R, and aiming such as the slingshot on a B pad), after
// its first instruction. The mouse aims there (look_aim.cpp), so the orbit leaves the
// camera to it: otherwise both turned with the mouse, and the view ran ahead of the aim.
// Aiming: Face Where the Camera Looks: aiming a gun (the twin guns 0x09, the shotgun 0x3B: Conker's weapon state,
// +0x31C's +0x78) starts the way the free camera looked, not the way Conker faced. Pressing R, the camera's mode
// (+0x2C) turns to aiming (0x100) and the game glides the view into his aim over about a second from his facing,
// before the look mode (func_15120158) runs: so on the first frame of that mode his facing (+0x7A, and +0x76 he
// moves along) is turned to the free camera's last view and the aiming angle (+0x12 of the camera's +0x3D4) is
// cleared, and the glide's target turned with them (+0x774): turned only when the look mode started, it glided to
// his old facing and then cut across. Single player only.
namespace {
    bool view_facing_known = false;      // view_facing holds the free camera's last view, as a facing
    uint16_t view_facing = 0;
    double view_facing_at = 0.0;         // game seconds then
    constexpr double view_facing_fresh = 0.15;
    constexpr uint32_t aiming_mode = 0x100;
    uint32_t last_camera_mode = 0;

    void face_camera_on_aim(uint8_t* rdram, gpr camera) {
        const uint32_t mode = (uint32_t)MEM_W(0x2C, camera);
        const bool started = mode == aiming_mode && last_camera_mode != aiming_mode;
        last_camera_mode = mode;
        if (!started || !view_facing_known || !conker::look_aim::face_camera() || !recompinput::players::is_single_player_mode() ||
            conker::testing::game_seconds() - view_facing_at > view_facing_fresh) {
            return;
        }
        const gpr conker = (gpr)(int32_t)MEM_W(0x3D0, camera);
        if ((uint32_t)conker != player_object) {
            return;
        }
        const gpr state = (gpr)(int32_t)MEM_W(0x31C, conker);
        const uint8_t weapon = ((uint32_t)state & 0xFF000000u) == 0x80000000u ? (uint8_t)MEM_BU(0x78, state) : 0;
        if (weapon != 0x09 && weapon != 0x3B) {
            return;
        }
        MEM_H(0x7A, conker) = (int16_t)view_facing;
        MEM_H(0x76, conker) = (int16_t)view_facing;
        // And the glide's target yaw (+0x774, degrees: the facing less 90, taken from his facing as R was pressed),
        // so the view glides to the camera's side, not to where he faced.
        float yaw = (float)view_facing * (360.0f / 65536.0f) - 90.0f;
        if (yaw < 0.0f) yaw += 360.0f;
        write_float(rdram, camera, 0x774, yaw);
        const gpr aim = (gpr)(int32_t)MEM_W(0x3D4, camera);
        if (((uint32_t)aim & 0xFF000000u) == 0x80000000u) {
            MEM_H(0x12, aim) = 0;
        }
    }
}

extern "C" void conker_mouse_camera_look_mode(uint8_t* rdram, recomp_context* ctx) {
    orbit.look_mode_ran = true;
}

extern "C" void func_15044380(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1510E8BC(uint8_t* rdram, recomp_context* ctx); // the game: the ground under a point

namespace {
    // The camera's ball: can a ball of this radius go from `from` to `to` without touching
    // anything? The game's movement step (func_15044380, which the camera's collision uses to
    // slide a stand-in from last frame's eye to the new one) slides a stand-in of the camera's kind
    // (0x2D, sized by the camera's +0x95C radius and +0x960 height) from `from` toward `to`: if
    // anything is in the way, it stops or slides aside and doesn't end there. Unlike the camera's
    // own collision (which marks the slide as the camera's, D_800CBDD2, and lets it through the
    // barn's big posts), this is an ordinary object's slide, which they stop.
    constexpr int32_t collide_scratch = (int32_t)0x800CBDC0, collide_scratch_size = 0x40;
    constexpr int32_t collide_layers = (int32_t)0x80089120; // 4 bytes: which collision layers count

    bool ball_reaches(uint8_t* rdram, recomp_context* ctx, gpr camera, const float from[3], const float to[3], float radius) {
        uint32_t scratch_copy[collide_scratch_size / 4], layers_copy;
        for (int i = 0; i < collide_scratch_size / 4; i++) scratch_copy[i] = (uint32_t)MEM_W(4 * i, (gpr)collide_scratch);
        layers_copy = (uint32_t)MEM_W(0, (gpr)collide_layers);
        const uint32_t radius_copy = (uint32_t)MEM_W(0x95C, camera);
        recomp_context saved = *ctx;
        // The stand-in, built on the stack below the hooked function's frame as the camera's
        // collision builds its own: kind 0x2D, position = where it's going, the camera's sizes.
        const gpr sp = ctx->r29 - 0x400;
        const gpr object = sp + 0x40;
        for (int i = 0; i < 0x340 / 4; i++) MEM_W(4 * i, object) = 0;
        MEM_W(0x0, object) = 0x2D;
        for (int i = 0; i < 3; i++) write_float(rdram, object, 0x14 + i * 4, to[i]);
        write_float(rdram, object, 0x28, to[1] - read_float(rdram, camera, 0x354));
        MEM_W(0x40, object) = MEM_W(0x37C, camera);
        MEM_W(0x180, object) = MEM_W(0x354, camera);
        MEM_W(0x188, object) = MEM_W(0x644, camera);
        MEM_W(0x318, object) = (int32_t)camera;
        write_float(rdram, camera, 0x95C, radius);
        MEM_B(0, (gpr)(collide_scratch + 0x12)) = 0; // D_800CBDD2: not the camera's slide
        MEM_B(0, (gpr)(collide_scratch + 0x13)) = 0; // D_800CBDD3
        MEM_B(0, (gpr)(collide_scratch + 0x14)) = 0; // D_800CBDD4
        MEM_W(0, (gpr)collide_layers) = 0x01010101;
        ctx->f12.fl = from[0];
        ctx->f14.fl = from[1];
        uint32_t z_bits;
        std::memcpy(&z_bits, &from[2], 4);
        ctx->r6 = (gpr)(int32_t)z_bits;
        ctx->r7 = object;
        MEM_W(0x10, sp) = 0;
        MEM_W(0x14, sp) = 0;
        ctx->r29 = sp;
        func_15044380(rdram, ctx);
        float off = 0.0f;
        for (int i = 0; i < 3; i++) {
            const float d = read_float(rdram, object, 0x14 + i * 4) - to[i];
            off += d * d;
        }
        *ctx = saved;
        MEM_W(0x95C, camera) = (int32_t)radius_copy;
        for (int i = 0; i < collide_scratch_size / 4; i++) MEM_W(4 * i, (gpr)collide_scratch) = (int32_t)scratch_copy[i];
        MEM_W(0, (gpr)collide_layers) = (int32_t)layers_copy;
        return off <= reached_within * reached_within;
    }

    // How far along dir (a unit vector) from look the camera's ball has room, up to full: the
    // farthest point the ball gets to from look untouched, with the biggest ball that leaves at least
    // min_reach, else the most room any of them leaves. The movement step is made for the short moves
    // of a frame: moved far at once, the ball can pass through a thin post. So it goes in steps of
    // about its size, and the step it can't make is halved down to where it stops. A smaller ball
    // goes at least as far as a bigger one: it starts where that one stopped.
    // The ground under a point, as the game's own camera finds it under its eye (func_1512BB10 at 0x1512C00C, which
    // then keeps the eye 40 above it): func_1510E8BC with the point's x, y, z on the stack, the height out at $a2.
    // Looked for from `from_y` down (above the eye, so ground above it is found too). no_ground where there's none.
    constexpr float no_ground = -10000.0f; // D_800A3690
    float ground_under(uint8_t* rdram, recomp_context* ctx, float x, float from_y, float z) {
        recomp_context saved = *ctx;
        const gpr sp = ctx->r29 - 0x100;
        const gpr out = sp + 0x60;
        write_float(rdram, out, 0, no_ground);
        MEM_W(0x10, sp) = 0;
        MEM_W(0x14, sp) = 0;
        write_float(rdram, sp, 0x18, x);
        write_float(rdram, sp, 0x1C, from_y);
        write_float(rdram, sp, 0x20, z);
        write_float(rdram, sp, 0x24, from_y);
        MEM_W(0x28, sp) = 0;
        MEM_W(0x2C, sp) = 0;
        write_float(rdram, sp, 0x30, no_ground);
        write_float(rdram, sp, 0x34, from_y);
        MEM_W(0x38, sp) = 1;
        ctx->r4 = 0;
        ctx->r5 = 0;
        ctx->r6 = out;
        ctx->r7 = 0;
        ctx->r29 = sp;
        func_1510E8BC(rdram, ctx);
        const float ground = read_float(rdram, out, 0);
        *ctx = saved;
        return ground;
    }

    float ball_room(uint8_t* rdram, recomp_context* ctx, gpr camera, const float look[3], const float dir[3], float full) {
        auto point = [&](float along, float out[3]) {
            for (int i = 0; i < 3; i++) out[i] = look[i] + dir[i] * along;
        };
        float clear = 0.0f;
        for (float radius : ball_radii) {
            const float step = std::max(radius, 12.0f);
            float from[3], to[3];
            while (clear < full) {
                const float next = std::min(clear + step, full);
                point(clear, from);
                point(next, to);
                if (!ball_reaches(rdram, ctx, camera, from, to, radius)) {
                    float blocked = next;
                    for (int k = 0; k < reach_halvings; k++) {
                        const float mid = 0.5f * (clear + blocked);
                        point(mid, to);
                        if (ball_reaches(rdram, ctx, camera, from, to, radius)) {
                            clear = mid;
                            point(clear, from);
                        } else {
                            blocked = mid;
                        }
                    }
                    break;
                }
                clear = next;
            }
            if (clear >= std::min(min_reach, full)) {
                return clear;
            }
        }
        return clear;
    }
}

// func_1512BB10 (the camera's collision), after its first instruction: $a0 is the camera.
// Places the eye the orbit wants, for the collision to move the camera toward. The game
// calls it a second time in some frames (camera +0x23C set): the mouse and wheel are read
// only the first time, and the second places the same eye.
extern "C" void conker_mouse_camera_collide(uint8_t* rdram, recomp_context* ctx) {
    const gpr camera = ctx->r4;
    if ((uint32_t)camera != (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        return;
    }
    static const bool probe = std::getenv("CONKER_PROBE") != nullptr;
    if (probe) {
        float sx = 0.0f, sy = 0.0f;
        conker::free_camera_stick(&sx, &sy);
        static int logged = 0;
        if ((sx != 0.0f || sy != 0.0f) && logged++ % 30 == 0) {
            std::printf("[freecam] stick %.2f %.2f follow=%d look=%d engaged=%d\n", sx, sy, (int)orbit.follow_camera_ran, (int)orbit.look_mode_ran, (int)orbit.engaged);
        }
    }
    if (!orbit.follow_camera_ran || orbit.look_mode_ran) {
        orbit.engaged = false;
        return;
    }

    float mouse_x = 0.0f, mouse_y = 0.0f;
    int notches = 0;
    if (!orbit.turned) {
        recompinput::get_mouse_deltas(&mouse_x, &mouse_y);
        notches = wheel_notches.exchange(0);
        if (!conker::free_camera_mouse()) { // Free Camera Off or Right Stick: the mouse only aims
            mouse_x = mouse_y = 0.0f;
            notches = 0;
        }
        // Camera: Invert Turning, as for the stick (free_camera_stick).
        if (conker::camera_inverted()) {
            mouse_x = -mouse_x;
        }
        if (conker::camera_tilt_inverted()) {
            mouse_y = -mouse_y;
        }
        // Clicking the right stick (Free Camera on): the next of the near, middle and far distances
        // (a controller has no wheel, on a Steam Deck say). Testing: CONKER_TEST_ZOOM="sec;sec;..."
        // counts as clicks at those game times.
        if (conker::free_camera_enabled()) {
            using recompinput::InputField;
            bool held = recompinput::get_input_digital(0, InputField::controller_digital(SDL_CONTROLLER_BUTTON_RIGHTSTICK));
            static std::vector<double> test_clicks = [] {
                std::vector<double> list;
                if (const char* t = std::getenv("CONKER_TEST_ZOOM")) {
                    for (const char* c = t; *c; ) { list.push_back(std::atof(c)); while (*c && *c != ';') c++; if (*c) c++; }
                }
                return list;
            }();
            const double now_s = conker::testing::game_seconds();
            for (double at : test_clicks) {
                held = held || (now_s >= at && now_s < at + 0.2);
            }
            static bool was_held = false;
            if (held && !was_held) {
                zoom_click = true;
            }
            was_held = held;
        }
        orbit.turned = true;
        // The right stick, turned into mouse-like pixels for this frame (game time, so
        // it turns as fast at any frame rate or test speed).
        const double now = conker::testing::game_seconds();
        static double last = now;
        const float seconds = (float)std::clamp(now - last, 0.0, 0.1);
        last = now;
        float stick_x = 0.0f, stick_y = 0.0f;
        conker::free_camera_stick(&stick_x, &stick_y);
        auto curve = [](float v) { return 0.5f * v + 0.5f * v * std::abs(v); };
        const float pixels_per_second = stick_degrees_per_second * conker::camera_turn_speed() / degrees_per_pixel;
        const float wanted_speed[2] = { curve(stick_x) * pixels_per_second, -curve(stick_y) * pixels_per_second * stick_tilt_share }; // pushed up: the view looks up
        const float ease = 1.0f - std::exp(-seconds / stick_ease_seconds);
        for (int i = 0; i < 2; i++) {
            orbit.stick_speed[i] += (wanted_speed[i] - orbit.stick_speed[i]) * ease;
            if (wanted_speed[i] == 0.0f && std::abs(orbit.stick_speed[i]) < 1.0f) {
                orbit.stick_speed[i] = 0.0f;
            }
        }
        mouse_x += orbit.stick_speed[0] * seconds;
        mouse_y += orbit.stick_speed[1] * seconds;
    }

    const float cx = read_float(rdram, camera, 0x2BC);
    const float cy = read_float(rdram, camera, 0x2C0);
    const float cz = read_float(rdram, camera, 0x2C4);
    // The distance the game keeps: its eye's horizontal distance and height from the
    // pivot, measured from the look-at point.
    const float horizontal = read_float(rdram, camera, 0x374);
    const float height = read_float(rdram, camera, 0x344) - (cy - read_float(rdram, camera, 0x2A8));
    const float wanted_distance = std::sqrt(horizontal * horizontal + height * height);

    if (!orbit.engaged) {
        if (mouse_x == 0.0f && mouse_y == 0.0f && notches == 0 && !zoom_click) {
            return;
        }
        // Take over from where the game's camera was drawn.
        const float ex = read_float(rdram, camera, 0x2EC) - cx;
        const float ey = read_float(rdram, camera, 0x2F0) - cy;
        const float ez = read_float(rdram, camera, 0x2F4) - cz;
        orbit.yaw = std::atan2(ez, ex);
        orbit.pitch = std::atan2(ey, std::sqrt(ex * ex + ez * ez));
        // The distance it had: the game's own setting, or further where the game's camera was further
        // out than that (a player saw it come in close as soon as the stick moved it). Not where it was
        // nearer: walls had pulled it in, which the orbit works out itself. Once the player has zoomed
        // (the wheel), their distance is kept instead.
        if (!orbit.zoomed) {
            orbit.wanted = std::max(wanted_distance, std::sqrt(ex * ex + ey * ey + ez * ez));
        }
        orbit.engaged = true;
    }

    // The controller's nearest and farthest distances from the look-at point.
    const float look_height = cy - read_float(rdram, camera, 0x2A8);
    float nearest = 0.0f, farthest = 0.0f;
    for (int i = 0; i < camera_distance_count; i++) {
        const gpr preset = (gpr)(int32_t)(camera_distances + i * 8);
        const float h = read_float(rdram, preset, 0), v = read_float(rdram, preset, 4) - look_height;
        const float d = std::sqrt(h * h + v * v);
        nearest = (i == 0) ? d : std::min(nearest, d);
        farthest = (i == 0) ? d : std::max(farthest, d);
    }
    if (notches != 0) {
        orbit.zoomed = true;
    }
    if (zoom_click) {
        // Near, middle, far: the next one out from where it is, back to near after far.
        zoom_click = false;
        orbit.zoomed = true;
        const float middle = 0.5f * (nearest + farthest);
        orbit.wanted = orbit.wanted < middle - 1.0f ? middle : (orbit.wanted < farthest - 1.0f ? farthest : nearest);
    }
    // (Never past the game's own farthest: taking over again and again, the camera took its own distance, which
    // grew each time, and flew off.)
    orbit.wanted = std::clamp(orbit.wanted * std::pow(zoom_step, (float)-notches), nearest, farthest);
    orbit.yaw += mouse_x * degrees_per_pixel * degrees_to_radians;

    // Tilting to look up while the view is aimed up (below): the view comes back down before the eye moves.
    float tilt = mouse_y * degrees_per_pixel * degrees_to_radians;
    if (tilt > 0.0f && orbit.aim > 0.0f) {
        const float back = std::min(tilt, orbit.aim);
        orbit.aim -= back;
        tilt -= back;
    }
    const float pitch_asked = orbit.pitch + tilt;
    orbit.pitch = std::clamp(pitch_asked, min_pitch, max_pitch);

    // The distance is the player's (the wheel's), except where the game pulls its own camera in
    // closer than the controller can (tight spots, depending on its angle): so does the orbit.
    // The change glides (orbit.reach) rather than jumps.
    static const bool no_game_zoom = std::getenv("CONKER_CAM_NOZOOM") != nullptr; // (testing)
    const float reach = (wanted_distance < nearest && !no_game_zoom) ? std::min(orbit.wanted, wanted_distance) : orbit.wanted;
    // Glide to a new distance rather than jump (a game-time ease, so the same at any frame rate).
    const double now = conker::testing::game_seconds();
    const float seconds = (float)std::clamp(now - orbit.last_frame, 0.0, 0.1);
    orbit.last_frame = now;
    // Slopes: Conker's rise per unit walked on the ground, along the view (the eye looks the other way
    // from its direction, yaw). Stepping sideways to the view, or in the air, it's kept.
    {
        const gpr player = (gpr)(int32_t)player_object;
        const bool grounded = ((uint32_t)MEM_W(0x100, player) >> 24) == 0x01;
        const float pos[3] = { read_float(rdram, player, 0x14), read_float(rdram, player, 0x18), read_float(rdram, player, 0x1C) };
        if (!grounded) {
            orbit.step_known = false;
        } else if (!orbit.step_known) {
            std::memcpy(orbit.step_from, pos, sizeof(pos));
            orbit.step_known = true;
        } else {
            const float dx = pos[0] - orbit.step_from[0], dz = pos[2] - orbit.step_from[2];
            const float walked = std::sqrt(dx * dx + dz * dz);
            if (walked >= slope_sample_step) {
                const float rise = std::clamp((pos[1] - orbit.step_from[1]) / walked, -1.0f, 1.0f);
                const float along = -(dx * std::cos(orbit.yaw) + dz * std::sin(orbit.yaw)) / walked;
                orbit.slope = rise * along;
                std::memcpy(orbit.step_from, pos, sizeof(pos));
            }
        }
        static const char* test_slope = std::getenv("CONKER_TEST_SLOPE"); // (testing: a made-up slope)
        if (test_slope != nullptr) {
            orbit.slope = (float)std::atof(test_slope);
        }
        orbit.shown_slope += (orbit.slope - orbit.shown_slope) * (1.0f - std::exp(-seconds / slope_ease_seconds));
    }
    if (orbit.reach <= 0.0f) {
        orbit.reach = reach;
    } else {
        orbit.reach += (reach - orbit.reach) * (1.0f - std::exp(-seconds / reach_ease_seconds));
    }
    // The eye: at the player's angle, as far out as the camera's ball has room (walls, posts, the
    // floor tilted down into, ceilings). The angle is never changed for the player.
    const float lowest = read_float(rdram, camera, 0x2A8) + above_feet;
    const float tilt_room = std::min(closest_tilted, orbit.reach);
    const float lowest_pitch = std::max(min_pitch, -std::asin(std::clamp((cy - lowest) / tilt_room, 0.0f, 1.0f)));
    // Looking up past where the eye can go (it stays above Conker's feet and outside him): the eye stays there and
    // the view aims up from it instead, up to highest_view above the horizon, so the top of anything tall comes into
    // view beside it (as the Zelda ports' camera and CBFD-Recompiled V0.1.6's do; conker_mouse_camera_view).
    if (pitch_asked < lowest_pitch) {
        orbit.aim = std::min(orbit.aim + (lowest_pitch - std::max(pitch_asked, orbit.pitch - 0.5f)), highest_view);
    }
    orbit.pitch = std::max(orbit.pitch, lowest_pitch);
    const float look[3] = { cx, cy, cz };
    const float slope_tilt = std::clamp(-std::atan(orbit.shown_slope) * slope_follow, -slope_tilt_most, slope_tilt_most);
    auto direction = [&](float lift, float out[3], float side = 0.0f) {
        const float pitch = std::min(orbit.pitch + slope_tilt + lift, max_pitch);
        out[0] = std::cos(pitch) * std::cos(orbit.yaw + side);
        out[1] = std::sin(pitch);
        out[2] = std::cos(pitch) * std::sin(orbit.yaw + side);
    };
    const auto timing_start = std::chrono::steady_clock::now(); // (CONKER_CAM_TRACE)
    const float enough = std::min(min_reach, orbit.reach);
    // The player's angle: as far out as there's room.
    float dir[3];
    direction(0.0f, dir);
    float distance = ball_room(rdram, ctx, camera, look, dir, orbit.reach);
    // No room there (the eye would be inside Conker): first turn aside, to the nearest angle with
    // plenty of room (on the side it last turned to first); where there's none (walls all around),
    // rise instead, as the game's camera does, looking down at him.
    float needed_side = 0.0f, needed_lift = 0.0f;
    if (distance < enough) {
        const float want = std::min(side_room, orbit.reach);
        const float first = orbit.side < 0.0f ? -1.0f : 1.0f;
        for (float turn = side_step; turn <= side_most && needed_side == 0.0f; turn += side_step) {
            for (float sign : { first, -first }) {
                float aside[3];
                direction(0.0f, aside, sign * turn);
                if (ball_room(rdram, ctx, camera, look, aside, orbit.reach) >= want) {
                    needed_side = sign * turn;
                    break;
                }
            }
        }
        if (needed_side == 0.0f) {
            for (float lift = squeeze_lift_step; orbit.pitch + lift - squeeze_lift_step < max_pitch; lift += squeeze_lift_step) {
                float lifted[3];
                direction(lift, lifted);
                if (ball_room(rdram, ctx, camera, look, lifted, orbit.reach) >= enough) {
                    needed_lift = lift;
                    break;
                }
            }
        }
    }
    // Up at once, back down eased.
    if (needed_lift >= orbit.lift) {
        orbit.lift = needed_lift;
    } else {
        orbit.lift += (needed_lift - orbit.lift) * (1.0f - std::exp(-seconds / lift_ease_seconds));
    }
    // Aside quickly, back eased; to the other side in one go (not back through Conker).
    if ((needed_side > 0.0f && orbit.side < 0.0f) || (needed_side < 0.0f && orbit.side > 0.0f)) {
        orbit.side = needed_side;
    } else {
        const bool back = std::abs(needed_side) < std::abs(orbit.side);
        orbit.side += (needed_side - orbit.side) * (1.0f - std::exp(-seconds / (back ? side_out_seconds : side_in_seconds)));
        if (std::abs(orbit.side - needed_side) < 0.2f * degrees_to_radians) {
            orbit.side = needed_side;
        }
    }
    if (orbit.lift > 0.0f || orbit.side != 0.0f) {
        direction(orbit.lift, dir, orbit.side);
        distance = ball_room(rdram, ctx, camera, look, dir, orbit.reach);
        // Easing back, an angle on the way without room: straight to the one wanted.
        if (distance < enough && (orbit.lift != needed_lift || orbit.side != needed_side)) {
            orbit.lift = needed_lift;
            orbit.side = needed_side;
            direction(orbit.lift, dir, orbit.side);
            distance = ball_room(rdram, ctx, camera, look, dir, orbit.reach);
        }
    }
    // Something the ball sees only now and then (see seen_hold): in suddenly, or seen again while held.
    if (distance < orbit.reach - held_slack &&
        ((orbit.shown_distance > 0.0f && distance < orbit.shown_distance - held_slack) || (orbit.held_seen && distance <= orbit.held + held_slack))) {
        orbit.held = distance;
        orbit.held_frames = seen_hold;
        orbit.held_seen = true;
    }
    if (dir[1] < 0.0f) {
        distance = std::min(distance, std::max((cy - lowest) / -dir[1], std::min(tilt_room, distance)));
    }
    // The ball doesn't stop at floors (it slides along them, and along anything sloped enough to stand on, like
    // It's War's tank traps), and the rule above only keeps the eye above Conker's feet: where the ground rises behind
    // him (It's War's beach) the eye went under it. So the line out to the eye is walked in ground_step steps from his
    // feet, the ground under each found from a little over the last one (ground_climb: following a rise, not an
    // overhang high above), and the eye stops where it would come within above_ground of it.
    {
        float ground = read_float(rdram, camera, 0x2A8); // under Conker
        float clear = 0.0f;
        bool blocked = false;
        auto sinks = [&](float along, float& found) {
            const float x = look[0] + dir[0] * along, y = look[1] + dir[1] * along, z = look[2] + dir[2] * along;
            found = ground_under(rdram, ctx, x, std::max(y, ground) + ground_climb, z);
            return found > no_ground + 1.0f && y < found + above_ground;
        };
        while (clear < distance) {
            const float next = std::min(clear + ground_step, distance);
            float found;
            if (sinks(next, found)) {
                float low = clear, high = next;
                for (int k = 0; k < reach_halvings; k++) {
                    const float mid = 0.5f * (low + high);
                    float f;
                    (sinks(mid, f) ? high : low) = mid;
                }
                clear = low;
                blocked = true;
                break;
            }
            if (found > no_ground + 1.0f) {
                ground = found;
            }
            clear = next;
        }
        if (blocked) {
            distance = std::max(clear, std::min(min_reach, distance));
        }
        orbit.ground_at_eye = ground;
    }
    // Held by the collision (an object the ball doesn't see): no further out than it allows (see held_hold).
    if (orbit.held > 0.0f) {
        if (orbit.held_frames > 0) {
            orbit.held_frames--;
        } else if (orbit.held_seen) {
            orbit.held = 0.0f;
        } else {
            orbit.held += held_step;
        }
        if (orbit.held <= 0.0f || (!orbit.held_seen && orbit.held >= distance)) {
            orbit.held = 0.0f;
            orbit.held_seen = false;
        } else {
            distance = std::min(distance, orbit.held);
        }
    }
    float target[3];
    for (int i = 0; i < 3; i++) target[i] = look[i] + dir[i] * distance;
    // Still under the ground there (a bump between him and the eye, or a rise too steep to come in under): up over
    // it, as the game's own camera does.
    {
        const float ground = ground_under(rdram, ctx, target[0], std::max(target[1], orbit.ground_at_eye) + ground_climb, target[2]);
        if (ground > no_ground + 1.0f && target[1] < ground + above_ground) {
            target[1] = ground + above_ground;
        }
    }
    static const bool trace_sight = std::getenv("CONKER_CAM_TRACE") != nullptr;
    if (trace_sight) {
        std::printf("[ground] eye y %.1f ground %.1f feet %.1f\n", target[1], ground_under(rdram, ctx, target[0], std::max(target[1], orbit.ground_at_eye) + ground_climb, target[2]), read_float(rdram, camera, 0x2A8));
        std::printf("[sight] t=%.3f reach %.1f distance %.1f pitch %.1f lift %.1f side %.1f slope %.2f us %lld\n", now, orbit.reach, distance, orbit.pitch * 57.2958f, orbit.lift * 57.2958f, orbit.side * 57.2958f, orbit.shown_slope,
            (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - timing_start).count());
    }
    // Placed directly: the game's collision (which would slide the camera from last frame's eye,
    // a few units a frame, and lets it through the big posts) is given nothing to move.
    for (int i = 0; i < 3; i++) {
        write_float(rdram, camera, 0x304 + i * 4, target[i]);
        write_float(rdram, camera, 0x2F8 + i * 4, target[i]);
        orbit.next_target[i] = target[i];
        orbit.placed[i] = target[i];
    }
    orbit.placed_directly = true;
}

// func_151284C4 (builds the view), after its first instruction: $a0 is the camera. The
// frame's camera update is done: start over for the next one.
extern "C" void conker_mouse_camera(uint8_t* rdram, recomp_context* ctx) {
    const gpr camera = ctx->r4;
    if ((uint32_t)camera != (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        return;
    }
    was_normal_camera = orbit.follow_camera_ran && !orbit.look_mode_ran;
    face_camera_on_aim(rdram, camera);
    if (!orbit.follow_camera_ran || orbit.look_mode_ran) {
        orbit.engaged = false;
    }
    // The free camera's view as a facing, for Aiming: Face Where the Camera Looks (facing 0x4000 looks down -z,
    // 0 down +x: atan2(-dz, dx)).
    if (orbit.engaged) {
        const float dx = read_float(rdram, camera, 0x2BC) - read_float(rdram, camera, 0x2F8);
        const float dz = read_float(rdram, camera, 0x2C4) - read_float(rdram, camera, 0x300);
        if (dx * dx + dz * dz > 1.0f) {
            view_facing = (uint16_t)(int32_t)std::lround(std::atan2(-dz, dx) * 32768.0 / 3.14159265358979);
            view_facing_known = true;
            view_facing_at = conker::testing::game_seconds();
        }
    }
    // Walls and tight spots: the collision left the eye at +0x2F8. Nearer than the view was: go
    // there at once. Further: ease back out. Along the same line from the look-at point.
    if (orbit.engaged) {
        const double now = conker::testing::game_seconds();
        const float seconds = (float)std::clamp(now - orbit.last_view, 0.0, 0.1);
        orbit.last_view = now;
        float look[3], eye[3], distance = 0.0f;
        for (int i = 0; i < 3; i++) {
            look[i] = read_float(rdram, camera, 0x2BC + i * 4);
            eye[i] = read_float(rdram, camera, 0x2F8 + i * 4) - look[i];
            distance += eye[i] * eye[i];
        }
        distance = std::sqrt(distance);
        if (orbit.shown_distance <= 0.0f || distance <= orbit.shown_distance) {
            orbit.shown_distance = distance;
        } else {
            orbit.shown_distance += (distance - orbit.shown_distance) * (1.0f - std::exp(-seconds / ease_out_seconds));
            if (distance > 0.0f) {
                for (int i = 0; i < 3; i++) {
                    write_float(rdram, camera, 0x2F8 + i * 4, look[i] + eye[i] * (orbit.shown_distance / distance));
                }
            }
        }
    } else {
        orbit.shown_distance = 0.0f;
    }
    static const bool trace = std::getenv("CONKER_CAM_TRACE") != nullptr;
    if (trace && orbit.engaged) {
        std::printf("[cam] t=%.3f target %.1f %.1f %.1f placed %.1f %.1f %.1f collided %.1f %.1f %.1f look %.1f %.1f %.1f shown %.1f direct %d\n",
            conker::testing::game_seconds(), orbit.next_target[0], orbit.next_target[1], orbit.next_target[2],
            orbit.placed[0], orbit.placed[1], orbit.placed[2],
            read_float(rdram, camera, 0x2F8), read_float(rdram, camera, 0x2FC), read_float(rdram, camera, 0x300),
            read_float(rdram, camera, 0x2BC), read_float(rdram, camera, 0x2C0), read_float(rdram, camera, 0x2C4),
            orbit.shown_distance, (int)orbit.placed_directly);
    }
    orbit.placed_directly = false;
    // The eye wanted this frame, to tell next frame how far the orbit itself moved.
    orbit.has_target = orbit.engaged;
    std::memcpy(orbit.target, orbit.next_target, sizeof(orbit.target));
    orbit.follow_camera_ran = false;
    orbit.look_mode_ran = false;
    orbit.turned = false;
    if (!orbit.engaged) {
        orbit.reach = 0.0f;
        orbit.lift = 0.0f;
        orbit.side = 0.0f;
        orbit.aim = 0.0f;
        orbit.held = 0.0f;
        orbit.held_seen = false;
        orbit.step_known = false;
        orbit.slope = 0.0f;
        orbit.shown_slope = 0.0f;
    }
}


// func_1512C490 at 0x1512C844, about to build the view from its own copies on the stack: the eye at $sp+0xEC and
// the look-at point at $sp+0xE0 (both copied from the camera's). While the orbit aims up (orbit.aim), the copy of the
// look-at point is turned up about the eye, at the same distance and across the ground the same way, at most
// highest_view above the horizon: the view looks that much higher from the same eye. Only this copy: the camera's
// look-at point and its record of the view (+0x2E0, which the game follows from the next frame) stay the game's.
namespace {
    // Testing (F9, free_camera_write_history): each frame's camera, at the view (conker_mouse_camera_view).
    struct HistoryFrame {
        double t; bool engaged; float placed, collided, shown, reach, held; int held_frames; float yaw, pitch;
        uint16_t anim;
    };
    constexpr int history_size = 300;
    HistoryFrame history[history_size] = {};
    int history_next = 0;
    std::mutex history_mutex;
}

void conker::free_camera_write_history(const std::filesystem::path& path) {
    std::lock_guard lock(history_mutex);
    if (FILE* f = std::fopen(path.string().c_str(), "w")) {
        std::fprintf(f, "time engaged placed collided shown reach held held_frames yaw pitch anim\n");
        for (int k = 0; k < history_size; k++) {
            const HistoryFrame& h = history[(history_next + k) % history_size];
            if (h.t == 0.0) continue;
            std::fprintf(f, "%.3f %d %.1f %.1f %.1f %.1f %.1f %d %.1f %.1f %04X\n", h.t, (int)h.engaged, h.placed, h.collided, h.shown, h.reach,
                h.held, h.held_frames, h.yaw * 57.2958f, h.pitch * 57.2958f, h.anim);
        }
        std::fclose(f);
    }
}

extern "C" void conker_mouse_camera_view(uint8_t* rdram, recomp_context* ctx) {
    const gpr camera = ctx->r16;
    if ((uint32_t)camera == (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        float placed = 0.0f, collided = 0.0f;
        for (int i = 0; i < 3; i++) {
            const float look = read_float(rdram, camera, 0x2BC + i * 4);
            const float p = orbit.placed[i] - look, e = read_float(rdram, camera, 0x2F8 + i * 4) - look;
            placed += p * p;
            collided += e * e;
        }
        std::lock_guard lock(history_mutex);
        history[history_next] = { conker::testing::game_seconds(), orbit.engaged, std::sqrt(placed), std::sqrt(collided), orbit.shown_distance,
            orbit.reach, orbit.held, orbit.held_frames, orbit.yaw, orbit.pitch,
            (uint16_t)((uint32_t)MEM_W(0, (gpr)(int32_t)(player_object + 0x84)) >> 16) };
        history_next = (history_next + 1) % history_size;
    }
    if (!orbit.engaged || (uint32_t)camera != (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        return;
    }
    // The game's collision has run by now (the eye at +0x2F8, copied to $sp+0xEC): where it held the eye nearer
    // than the orbit placed it (an object the ball doesn't see), the orbit keeps to that distance (held_hold).
    {
        float placed = 0.0f, held = 0.0f;
        for (int i = 0; i < 3; i++) {
            const float look = read_float(rdram, camera, 0x2BC + i * 4);
            const float p = orbit.placed[i] - look, e = read_float(rdram, camera, 0x2F8 + i * 4) - look;
            placed += p * p;
            held += e * e;
        }
        placed = std::sqrt(placed);
        held = std::sqrt(held);
        // (Nearer than our own ease-out left it, that is: shown_distance, which eases back out after walls.)
        const float ours = orbit.shown_distance > 0.0f ? std::min(placed, orbit.shown_distance) : placed;
        if (held < ours - held_slack) {
            orbit.held = held + held_margin;
            orbit.held_frames = held_hold;
            orbit.held_seen = false;
        }
    }
    if (orbit.aim <= 0.0f) {
        return;
    }
    const gpr eye = ctx->r29 + 0xEC, look = ctx->r29 + 0xE0;
    const float ex = read_float(rdram, eye, 0), ey = read_float(rdram, eye, 4), ez = read_float(rdram, eye, 8);
    const float lx = read_float(rdram, look, 0) - ex, ly = read_float(rdram, look, 4) - ey, lz = read_float(rdram, look, 8) - ez;
    const float across = std::sqrt(lx * lx + lz * lz);
    if (across < 1.0f) {
        return;
    }
    const float distance = std::sqrt(across * across + ly * ly);
    const float view = std::min(std::atan2(ly, across) + orbit.aim, highest_view);
    const float scale = distance * std::cos(view) / across;
    write_float(rdram, look, 0, ex + lx * scale);
    write_float(rdram, look, 4, ey + distance * std::sin(view));
    write_float(rdram, look, 8, ez + lz * scale);
}

bool conker::normal_camera() {
    return was_normal_camera;
}

// frontend.cpp, once SDL is up: listen for the scroll wheel.
void conker_mouse_camera_init() {
    SDL_AddEventWatch(watch_wheel, nullptr);
}

// The right stick for the Free Camera, x (right) and y (up), -1 to 1 past the dead zone;
// zero when the option is off. Test runs can drive it (testing.cpp).
void conker::free_camera_stick(float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
    float test_x = 0.0f, test_y = 0.0f;
    if (conker::testing::right_stick(&test_x, &test_y)) {
        *x = test_x;
        *y = test_y;
        return;
    }
    if (!conker::free_camera_enabled()) {
        return;
    }
    using recompinput::InputField;
    const float sx = recompinput::get_input_analog(0, InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTX, true)) -
        recompinput::get_input_analog(0, InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTX, false));
    const float sy = recompinput::get_input_analog(0, InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTY, false)) -
        recompinput::get_input_analog(0, InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTY, true));
    auto shape = [](float v) {
        const float a = std::abs(v);
        return a < stick_dead_zone ? 0.0f : std::copysign((a - stick_dead_zone) / (1.0f - stick_dead_zone), v);
    };
    *x = shape(sx);
    *y = shape(sy);
    if (conker::camera_inverted()) {
        *x = -*x;
    }
    if (conker::camera_tilt_inverted()) {
        *y = -*y;
    }
}

