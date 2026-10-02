// From CBFD-Recompiled's mouse camera (Copyright (c) 2026 Sean Ciaschi, MIT License; see
// recomp/THIRD_PARTY_LICENSE). Ours adds the right stick (the Conker tab's
// Free Camera option): it turns and tilts the same orbit, at up to stick_degrees_per_second.
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
// way the C-buttons' turning places it earlier in the same update (func_15122C5C). The
// collision moves the camera from where it was drawn last frame (+0x304) toward that eye
// and stops it at walls, sliding along them, and leaves the result in +0x2F8. The view
// (func_151284C4, in func_1512C490) then draws from there (+0x2EC). So the orbit stops at
// walls the way the game's own camera does.
//
// The orbit only runs where the C-buttons turn the camera (func_1512D390 ran this
// frame) and not in the look mode (func_15120158: hold R, aiming), so cutscenes, special
// cameras and aiming are the game's. Pressing C-left or C-right
// hands the camera back to the game until the mouse moves again.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL.h>

#include "recomp.h"
#include "recompinput/input_state.h"
#include "recompinput/input_types.h"

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
    // The ground: the eye is kept at least this many units above Conker's feet (+0x2A8), where
    // flat ground is; slopes the game's collision pushes it up from are handled after it.
    constexpr float eye_above_feet = 12.0f;
    // The collision pushing the eye up by more than this counts as the ground holding it.
    constexpr float ground_push = 2.0f;
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    // How far the camera may look up or down: pitch is the eye's angle above the
    // look-at point.
    constexpr float min_pitch = -25.0f * degrees_to_radians;
    constexpr float max_pitch = 75.0f * degrees_to_radians;
    constexpr uint32_t current_camera = 0x800DBFF0; // D_800DBFF0
    // Scroll wheel zoom: each notch scales the distance by this, between the nearest and
    // farthest of the game's own camera distances (D_800A34B0: the controller's four,
    // each a horizontal distance and a height from the pivot, 530 x 400 the farthest).
    constexpr float zoom_step = 1.12f;
    constexpr uint32_t camera_distances = 0x800A34B0; // D_800A34B0, 4 x { horizontal, height }
    constexpr int camera_distance_count = 4;
    // The orbit's distance (the wheel's, the game's closer one in tight spots, the floor's) glides
    // to a new value over about this many seconds instead of jumping there.
    constexpr float reach_ease_seconds = 0.2f;
    // Tilted toward the floor, the eye comes in along the orbit (looking up at Conker) rather
    // than stopping at once, but no nearer the look-at point than this; there it stops tilting.
    constexpr float closest = 170.0f;
    // Walls and tight spots: when the collision holds the eye nearer than it was, the view
    // follows at once (it mustn't go through walls); when it lets it go further again, the view
    // eases back out over about this many seconds, so a camera bumping along walls doesn't jump.
    constexpr float ease_out_seconds = 0.35f;
    // Line of sight (clear_reach): the eye stays sight_margin in front of anything in the way. If
    // that leaves it nearer Conker than sight_closest, it rises instead (looking down past what's in
    // the way, up to sight_lift_max more), by sight_lift_step tries; it comes back down over
    // lift_ease_seconds once there's room.
    constexpr float sight_margin = 14.0f;
    constexpr float sight_closest = 150.0f;
    constexpr float sight_nearest = 60.0f;
    constexpr float sight_lift_step = 15.0f * 3.14159265358979f / 180.0f;
    constexpr int sight_lift_tries = 4;
    constexpr float lift_ease_seconds = 0.6f;

    // Scroll wheel notches since the view last read them (SDL event watch: the
    // frontend's own event loop consumes the events).
    std::atomic<int> wheel_notches = 0;
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
        double last_frame = 0.0;        // game seconds at the last orbit update
        float stick_speed[2] = {};      // the stick's eased turning, in mouse pixels a second
        bool placed_directly = false;   // this frame's eye was placed at the orbit's target
        float placed[3] = {};           // the eye placed for the collision this frame
        float shown_distance = 0.0f;    // the eye's eased distance from the look-at point (0: none yet)
        float reach = 0.0f;             // the orbit's eased distance (0: none yet)
        float lift = 0.0f;              // radians the eye is raised to see past something
        float shown_yaw = 0.0f;         // the yaw the eye was placed at last frame (has_shown_yaw)
        bool has_shown_yaw = false;
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
extern "C" void conker_mouse_camera_look_mode(uint8_t* rdram, recomp_context* ctx) {
    orbit.look_mode_ran = true;
}

extern "C" void func_15044380(uint8_t* rdram, recomp_context* ctx);

namespace {
    // Line of sight: is anything between Conker (the look-at
    // point) and the eye? The game's movement step (func_15044380, which the camera's collision uses
    // to slide a stand-in object from last frame's eye to the new one) slides a stand-in from the
    // look-at point toward the eye; where it stops short (or is pushed aside), something is in the
    // way. Unlike the camera's own collision (which marks the slide as the camera's, D_800CBDD2, and
    // lets it through the barn's big posts), this is an ordinary object's slide, which they stop.
    constexpr int32_t collide_scratch = (int32_t)0x800CBDC0, collide_scratch_size = 0x40;
    constexpr int32_t collide_layers = (int32_t)0x80089120; // 4 bytes: which collision layers count

    // How far along dir (a unit vector) from look the eye can be, up to full.
    float clear_reach(uint8_t* rdram, recomp_context* ctx, gpr camera, const float look[3], const float dir[3], float full) {
        float eye[3];
        for (int i = 0; i < 3; i++) eye[i] = look[i] + dir[i] * full;
        uint32_t scratch_copy[collide_scratch_size / 4], layers_copy;
        for (int i = 0; i < collide_scratch_size / 4; i++) scratch_copy[i] = (uint32_t)MEM_W(4 * i, (gpr)collide_scratch);
        layers_copy = (uint32_t)MEM_W(0, (gpr)collide_layers);
        recomp_context saved = *ctx;
        // The stand-in, built on the stack below the hooked function's frame as the camera's
        // collision builds its own: kind 0x2D, position = where it's going, the camera's sizes.
        const gpr sp = ctx->r29 - 0x400;
        const gpr object = sp + 0x40;
        for (int i = 0; i < 0x340 / 4; i++) MEM_W(4 * i, object) = 0;
        MEM_W(0x0, object) = 0x2D;
        for (int i = 0; i < 3; i++) write_float(rdram, object, 0x14 + i * 4, eye[i]);
        write_float(rdram, object, 0x28, eye[1] - read_float(rdram, camera, 0x354));
        MEM_W(0x40, object) = MEM_W(0x37C, camera);
        MEM_W(0x180, object) = MEM_W(0x354, camera);
        MEM_W(0x188, object) = MEM_W(0x644, camera);
        MEM_W(0x318, object) = (int32_t)camera;
        MEM_B(0, (gpr)(collide_scratch + 0x12)) = 0; // D_800CBDD2: not the camera's slide
        MEM_B(0, (gpr)(collide_scratch + 0x13)) = 0; // D_800CBDD3
        MEM_B(0, (gpr)(collide_scratch + 0x14)) = 0; // D_800CBDD4
        MEM_W(0, (gpr)collide_layers) = 0x01010101;
        ctx->f12.fl = look[0];
        ctx->f14.fl = look[1];
        uint32_t z_bits;
        std::memcpy(&z_bits, &look[2], 4);
        ctx->r6 = (gpr)(int32_t)z_bits;
        ctx->r7 = object;
        MEM_W(0x10, sp) = 0;
        MEM_W(0x14, sp) = 0;
        ctx->r29 = sp;
        func_15044380(rdram, ctx);
        float along = 0.0f, off = 0.0f, stop[3];
        for (int i = 0; i < 3; i++) {
            stop[i] = read_float(rdram, object, 0x14 + i * 4);
            along += (stop[i] - look[i]) * dir[i];
        }
        for (int i = 0; i < 3; i++) {
            const float d = stop[i] - (look[i] + dir[i] * along);
            off += d * d;
        }
        *ctx = saved;
        for (int i = 0; i < collide_scratch_size / 4; i++) MEM_W(4 * i, (gpr)collide_scratch) = (int32_t)scratch_copy[i];
        MEM_W(0, (gpr)collide_layers) = (int32_t)layers_copy;
        // Grazing something (the floor under a low eye, a wall it runs along) slides the stand-in a
        // little aside or short: only a real stop counts, or the eye would flicker between the two.
        if (along >= full - 24.0f) {
            return full;
        }
        if (std::sqrt(off) > 24.0f) {
            along = std::min(along, full - 24.0f); // pushed well aside: a wall along the way
        }
        return std::clamp(along, 0.0f, full);
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
        if (!conker::mouse_turns_camera()) { // Mouse: Turn the Camera off: the mouse only aims
            mouse_x = mouse_y = 0.0f;
            notches = 0;
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
        const float wanted_speed[2] = { curve(stick_x) * pixels_per_second, -curve(stick_y) * pixels_per_second }; // pushed up: the view looks up
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
        if (mouse_x == 0.0f && mouse_y == 0.0f && notches == 0) {
            return;
        }
        // Take over from where the game's camera was drawn.
        const float ex = read_float(rdram, camera, 0x2EC) - cx;
        const float ey = read_float(rdram, camera, 0x2F0) - cy;
        const float ez = read_float(rdram, camera, 0x2F4) - cz;
        orbit.yaw = std::atan2(ez, ex);
        orbit.pitch = std::atan2(ey, std::sqrt(ex * ex + ez * ez));
        if (orbit.wanted == 0.0f) {
            orbit.wanted = wanted_distance;
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
    orbit.wanted = std::clamp(orbit.wanted * std::pow(zoom_step, (float)-notches), nearest, farthest);
    orbit.yaw += mouse_x * degrees_per_pixel * degrees_to_radians;

    orbit.pitch = std::clamp(orbit.pitch + mouse_y * degrees_per_pixel * degrees_to_radians, min_pitch, max_pitch);

    // The distance is the player's (the wheel's), except where the game pulls its own camera in
    // closer than the controller can (tight spots, depending on its angle): so does the orbit. It
    // knows where big posts and beams the collision lets the camera through would block the view.
    // The change glides (orbit.reach) rather than jumps.
    static const bool no_game_zoom = std::getenv("CONKER_CAM_NOZOOM") != nullptr; // (testing)
    const float zoomed = (wanted_distance < nearest && !no_game_zoom) ? std::min(orbit.wanted, wanted_distance) : orbit.wanted;
    // Not below the ground at Conker's feet: tilted down that far, the eye comes in along the
    // orbit instead, resting just above the floor and looking up at him (pushed into the ground,
    // the game's collision shoved it back up, and aiming it down again every frame shook the view).
    float reach = zoomed;
    // (orbit.reach eases toward this below)
    const float above_floor = cy - (read_float(rdram, camera, 0x2A8) + eye_above_feet);
    const float nearest_reach = std::min(closest, zoomed);
    const float lowest = -std::asin(std::clamp(above_floor / nearest_reach, 0.0f, 1.0f));
    if (orbit.pitch < lowest) {
        orbit.pitch = lowest;
    }
    if (orbit.pitch < 0.0f && above_floor > 0.0f) {
        reach = std::clamp(above_floor / -std::sin(orbit.pitch), nearest_reach, zoomed);
    }
    // Glide to a new distance rather than jump (a game-time ease, so the same at any frame rate).
    const double now = conker::testing::game_seconds();
    const float seconds = (float)std::clamp(now - orbit.last_frame, 0.0, 0.1);
    orbit.last_frame = now;
    if (orbit.reach <= 0.0f) {
        orbit.reach = reach;
    } else {
        orbit.reach += (reach - orbit.reach) * (1.0f - std::exp(-seconds / reach_ease_seconds));
    }
    const float look[3] = { cx, cy, cz };
    auto direction = [](float yaw, float pitch, float out[3]) {
        out[0] = std::cos(pitch) * std::cos(yaw);
        out[1] = std::sin(pitch);
        out[2] = std::cos(pitch) * std::sin(yaw);
    };
    // Line of sight: the eye comes in front of anything between it and
    // Conker; where that would be too near him, it rises to look past it instead. best_view tries
    // the lifts at a yaw and returns the lift to use and the distance it allows.
    auto best_view = [&](float yaw, float* lift_out) {
        float best_along = -1.0f, best_lift = 0.0f;
        for (int k = 0; k <= sight_lift_tries; k++) {
            const float lift = k * sight_lift_step;
            float dir[3];
            direction(yaw, std::min(orbit.pitch + lift, max_pitch), dir);
            const float along = clear_reach(rdram, ctx, camera, look, dir, orbit.reach);
            if (along >= std::min(sight_closest, orbit.reach - 1.0f)) {
                *lift_out = lift;
                return along;
            }
            if (along > best_along) {
                best_along = along;
                best_lift = lift;
            }
        }
        *lift_out = best_lift;
        return best_along;
    };
    float needed_lift = 0.0f;
    float view = best_view(orbit.yaw, &needed_lift);
    const bool good = view >= std::min(sight_closest, orbit.reach - 1.0f);
    // Where it can't see him from far enough even raised (a post right beside him, turned toward or
    // come to as he moves), the camera turns the least it can to where it can, either way, as it
    // would slide along a wall, rather than squeeze in behind him.
    if (!good) {
        static const float offsets[] = { 4.0f, 8.0f, 14.0f, 22.0f, 32.0f, 45.0f };
        bool found = false;
        for (float offset : offsets) {
            for (float side : { 1.0f, -1.0f }) {
                // The side the player last turned from first: back where it came from.
                const float sign = (mouse_x > 0.0f) ? -side : side;
                const float yaw = orbit.yaw + sign * offset * degrees_to_radians;
                float lift = 0.0f;
                if (best_view(yaw, &lift) >= std::min(sight_closest, orbit.reach - 1.0f)) {
                    orbit.yaw = yaw;
                    needed_lift = lift;
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        // Nowhere near: stay where it was last frame (it could see him there) rather than dive in.
        if (!found && orbit.has_shown_yaw) {
            float lift = 0.0f;
            if (best_view(orbit.shown_yaw, &lift) >= std::min(sight_closest, orbit.reach - 1.0f)) {
                orbit.yaw = orbit.shown_yaw;
                needed_lift = lift;
            }
        }
    }
    orbit.shown_yaw = orbit.yaw;
    orbit.has_shown_yaw = true;
    if (needed_lift >= orbit.lift) {
        orbit.lift = needed_lift;
    } else {
        orbit.lift += (needed_lift - orbit.lift) * (1.0f - std::exp(-seconds / lift_ease_seconds));
    }
    float dir[3];
    direction(orbit.yaw, std::min(orbit.pitch + orbit.lift, max_pitch), dir);
    float along = clear_reach(rdram, ctx, camera, look, dir, orbit.reach);
    if (along < std::min(sight_closest, orbit.reach - 1.0f) && orbit.lift != needed_lift) {
        // Easing back down, the angle on the way can catch something (a beam above): the one found
        // clear instead.
        orbit.lift = needed_lift;
        direction(orbit.yaw, std::min(orbit.pitch + orbit.lift, max_pitch), dir);
        along = clear_reach(rdram, ctx, camera, look, dir, orbit.reach);
    }
    const float distance = (along >= orbit.reach) ? orbit.reach : std::clamp(along - sight_margin, std::min(sight_nearest, orbit.reach), orbit.reach);
    float target[3];
    for (int i = 0; i < 3; i++) target[i] = look[i] + dir[i] * distance;
    static const bool trace_sight = std::getenv("CONKER_CAM_TRACE") != nullptr;
    if (trace_sight) {
        std::printf("[sight] t=%.3f reach %.1f along %.1f lift %.1f deg distance %.1f\n", now, orbit.reach, along, orbit.lift * 57.2958f, distance);
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
    if (!orbit.follow_camera_ran || orbit.look_mode_ran) {
        orbit.engaged = false;
    }
    // Sloping ground the collision pushed the eye up from (+0x2F8, where it left the eye):
    // the orbit rests there, at that angle, instead of aiming into the ground again next frame.
    if (orbit.engaged && orbit.placed_directly && read_float(rdram, camera, 0x2F8 + 4) > orbit.placed[1] + ground_push) {
        const float ex = read_float(rdram, camera, 0x2F8) - read_float(rdram, camera, 0x2BC);
        const float ey = read_float(rdram, camera, 0x2FC) - read_float(rdram, camera, 0x2C0);
        const float ez = read_float(rdram, camera, 0x300) - read_float(rdram, camera, 0x2C4);
        orbit.pitch = std::clamp(std::max(orbit.pitch, std::atan2(ey, std::sqrt(ex * ex + ez * ez))), min_pitch, max_pitch);
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
        orbit.has_shown_yaw = false;
    }
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
