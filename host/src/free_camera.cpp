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
    // Auto-Follow (Conker tab): once the camera hasn't been turned for follow_delay seconds,
    // while Conker moves faster than follow_min_speed (units a second), the orbit eases
    // round behind him and back to the game's own height, turning at most
    // follow_degrees_per_second. Standing still, or running toward the camera, it stays where it
    // was left. (Movement is relative to the camera, so a held sideways run curves gently.)
    constexpr double follow_delay = 1.5;
    constexpr float follow_min_speed = 40.0f;
    constexpr float follow_degrees_per_second = 45.0f;
    // It doesn't follow while he runs toward the camera (the view would spin round to his
    // back): only while his heading is no more than follow_max_angle off the view's.
    constexpr float follow_max_angle = 110.0f;
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
    // When the camera is behind the eye the orbit wants (held by a wall, or freed after one),
    // it's sent this fraction of the way each frame, and a gap this many units larger than the
    // orbit's own move counts as behind. The collision only takes the C-buttons' camera a
    // little way each frame; one long move from where a wall held it jumped about.
    constexpr float catch_up = 0.3f;
    constexpr float behind_slack = 8.0f;

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
        double last_input = -100.0;     // game seconds when the player last turned or zoomed
        double last_frame = 0.0;        // game seconds at the last orbit update
        bool has_pivot = false;
        float pivot[3] = {};            // where Conker's feet were last frame (camera + 0x2A4)
        float stick_speed[2] = {};      // the stick's eased turning, in mouse pixels a second
        bool placed_directly = false;   // this frame's eye was placed at the orbit's target
        float placed[3] = {};           // the eye placed for the collision this frame
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

    // Auto-Follow: how far Conker's feet moved since the last update, and when the camera
    // was last turned.
    const double now = conker::testing::game_seconds();
    const float frame_seconds = (float)std::clamp(now - orbit.last_frame, 0.0, 0.1);
    orbit.last_frame = now;
    if (mouse_x != 0.0f || mouse_y != 0.0f || notches != 0) {
        orbit.last_input = now;
    }
    const float px = read_float(rdram, camera, 0x2A4), pz = read_float(rdram, camera, 0x2AC);
    if (orbit.has_pivot && frame_seconds > 0.0f && conker::camera_auto_follow() && now - orbit.last_input >= follow_delay) {
        const float vx = (px - orbit.pivot[0]) / frame_seconds, vz = (pz - orbit.pivot[2]) / frame_seconds;
        const float speed = std::sqrt(vx * vx + vz * vz);
        // The view's heading on the ground: from the eye toward the look-at point.
        const float view_x = -std::cos(orbit.yaw), view_z = -std::sin(orbit.yaw);
        const float heading_cos = (speed > 0.0f) ? (vx * view_x + vz * view_z) / speed : -1.0f;
        if (speed > follow_min_speed && heading_cos >= std::cos(follow_max_angle * degrees_to_radians)) {
            // Behind him: the eye on the far side of where he's heading. Ease by a share of
            // the difference each frame, capped, so it glides in and settles.
            const float behind = std::atan2(-vz, -vx);
            float diff = std::remainder(behind - orbit.yaw, 2.0f * 3.14159265f);
            const float max_step = follow_degrees_per_second * degrees_to_radians * frame_seconds;
            orbit.yaw += std::clamp(diff * std::min(1.0f, 1.5f * frame_seconds), -max_step, max_step);
            // And the game's own height: the angle of its eye above the look-at point.
            const float game_pitch = std::atan2(height, horizontal);
            float pitch_diff = game_pitch - orbit.pitch;
            orbit.pitch += std::clamp(pitch_diff * std::min(1.0f, 2.0f * frame_seconds), -max_step, max_step);
        }
    }
    orbit.pivot[0] = px;
    orbit.pivot[2] = pz;
    orbit.has_pivot = true;
    orbit.pitch = std::clamp(orbit.pitch + mouse_y * degrees_per_pixel * degrees_to_radians, min_pitch, max_pitch);

    // Where the game pulls its own camera in closer than the controller can (tight spots),
    // so does the orbit.
    const float zoomed = (wanted_distance < nearest) ? std::min(orbit.wanted, wanted_distance) : orbit.wanted;
    // Not below the ground at Conker's feet: pushed into it, the game's collision shoves the eye
    // back up, and aiming it down again every frame shook the view.
    const float lowest = std::asin(std::clamp((read_float(rdram, camera, 0x2A8) + eye_above_feet - cy) / zoomed, -1.0f, 1.0f));
    if (orbit.pitch < lowest) {
        orbit.pitch = std::min(lowest, max_pitch);
    }
    const float dx = std::cos(orbit.pitch) * std::cos(orbit.yaw);
    const float dy = std::sin(orbit.pitch);
    const float dz = std::cos(orbit.pitch) * std::sin(orbit.yaw);
    const float target[3] = { cx + dx * zoomed, cy + dy * zoomed, cz + dz * zoomed };

    // The collision moves the camera from last frame's eye (+0x304). If that's further from
    // the eye wanted now than the orbit itself moved since last frame, the camera is behind:
    // ease it there rather than sending it the whole way at once.
    float gap = 0.0f, moved = 0.0f;
    float from[3];
    for (int i = 0; i < 3; i++) {
        from[i] = read_float(rdram, camera, 0x304 + i * 4);
        gap += (target[i] - from[i]) * (target[i] - from[i]);
        const float step = orbit.has_target ? (target[i] - orbit.target[i]) : 0.0f;
        moved += step * step;
    }
    const bool behind = orbit.has_target && (std::sqrt(gap) > std::sqrt(moved) + behind_slack);
    for (int i = 0; i < 3; i++) {
        const float eye = behind ? from[i] + (target[i] - from[i]) * catch_up : target[i];
        write_float(rdram, camera, 0x2F8 + i * 4, eye);
        orbit.next_target[i] = target[i];
        orbit.placed[i] = eye;
    }
    orbit.placed_directly = !behind;
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
    orbit.placed_directly = false;
    // The eye wanted this frame, to tell next frame how far the orbit itself moved.
    orbit.has_target = orbit.engaged;
    std::memcpy(orbit.target, orbit.next_target, sizeof(orbit.target));
    orbit.follow_camera_ran = false;
    orbit.look_mode_ran = false;
    orbit.turned = false;
    if (!orbit.engaged) {
        orbit.has_pivot = false;
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
