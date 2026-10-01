// From CBFD-Recompiled (Copyright (c) 2026 Sean Ciaschi, MIT License; see
// recomp/THIRD_PARTY_LICENSE), used unchanged except for this note.
// Gyro and mouse aiming in the look mode (hold R, look around with the stick), and a choice of
// smooth or direct response for each of the stick, the mouse and gyro.
//
// func_15120158 runs the look mode once a frame. The stick moves a target yaw and pitch, in
// degrees ($t0 + 0x34 and + 0x38; yaw grows to the left and isn't wrapped), then the pitch target
// is clamped to limits that depend on what Conker is doing (0x15120B44 to 0x15120D28), and then a
// spring (func_15049688) eases the current angles ($s0 + 0x37C yaw, $t0 + 0x3C pitch) toward the
// targets, keeping their velocities in $t0 + 0x18 and + 0x1C. Everything the camera is built from
// is copied from the current angles after that.
//
// Hooks (conker.toml):
//   conker_look_stick_yaw_a/_b and conker_look_stick_pitch, where the stick's turn is stored into
//     the targets (0x15120A14, 0x15120A44, 0x15120A98): each can turn its axis around. The game's
//     stick has X normal and Y inverted (up looks down).
//   conker_look_targets, before the clamp: the mouse and gyro turn the targets, so the game's own
//     limits apply to them just as they do to the stick.
//   conker_look_currents, after the springs (L_15120E8C, where both of their branches meet): for
//     an input set to Direct, the current angles take its movement in the same frame instead of
//     being eased toward it.
//   conker_look_yaw_from_facing(_scaled), where some states work the yaw target out each frame from
//     Conker's facing less an aiming angle the stick turns: the mouse and gyro turn that angle too.
//   conker_aim_stick, in the second aiming mode (func_15126378: the sniper scope, the magnum,
//     throwables): the stick's invert, and the mouse and gyro, scaled by the zoom.
// The mouse and gyro are player 1's only: in multiplayer both modes run for each player's camera.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>

#include "recomp.h"

#include "conker.hpp"

#if defined(CONKER_RT64)
#include "recompinput/input_state.h"
#include "recompui/config.h"
#include "util/steam_deck.h"
#endif

namespace {
    // Field offsets (see above).
    constexpr int32_t target_yaw = 0x34, target_pitch = 0x38;      // $t0
    constexpr int32_t yaw_velocity = 0x18, pitch_velocity = 0x1C;  // $t0
    constexpr int32_t current_yaw = 0x37C;                         // $s0
    constexpr int32_t current_pitch = 0x3C;                        // $t0

    // Degrees per pixel of mouse movement at 100% mouse sensitivity.
    constexpr float mouse_degrees_per_pixel = 0.1f;
    // recompinput's gyro delta isn't an angle: it adds up the controller's angular velocity
    // (degrees per second) once per sensor event, without the time between events, so a frame's
    // worth depends on the controller's report rate. Tuned by feel, not derived.
    constexpr float gyro_scale = 1.0f / 60.0f;

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

    // Each input poll's mouse and gyro movement, queued so the look mode takes one poll's worth a
    // frame. Measured in the look mode: 30 polls and 30 look updates a second, but on different
    // threads (the game's controller thread polls), so two polls sometimes land between two
    // updates and none before the next. recompinput keeps only the latest poll's movement, and
    // taking that once per update left some frames still and lost others: the view hitched.
    // Queued, nothing is lost, and a second poll waits a frame instead of doubling one. Outside
    // the look mode nothing takes from the queue, so the oldest is dropped past two, rather than
    // saved up and turned into a jump when R is pressed.
    struct Movement {
        float mouse_x, mouse_y, gyro_x, gyro_y;
    };
    std::mutex queue_mutex;
    std::deque<Movement> queue;

    // What conker_look_targets did this frame, for conker_look_currents.
    struct Frame {
        bool targets_moved = false;
        float pitch_target_set = 0.0f;  // the pitch target as conker_look_targets left it
        float direct_yaw = 0.0f;        // the part of the turn from inputs set to Direct
        float direct_pitch = 0.0f;
    } frame;

    // Set by conker_look_yaw_from_facing(_scaled) when this frame's look works its yaw out from the
    // way Conker faces less the aiming angle, for conker_look_targets: how many of the aiming angle's
    // 16-bit units turn the view a degree (0 when it doesn't). And the part of a unit not yet taken.
    float aim_units_per_degree = 0.0f;
    float aim_remainder = 0.0f;

#if defined(CONKER_RT64)
    namespace options {
        const std::string stick_response = "look_stick_response";
        const std::string stick_invert = "look_stick_invert";
        const std::string mouse_response = "look_mouse_response";
        const std::string gyro_response = "look_gyro_response";
        const std::string mouse_invert = "look_mouse_invert";
        const std::string gyro_invert = "look_gyro_invert";
        const std::string camera_turn_invert = "camera_invert_turning";
        const std::string camera_turn_speed = "camera_turn_speed";
        // From CBFD-Recompiled V0.1.5 (same ids, so their settings carry over):
        const std::string mouse_camera = "mouse_turns_camera";
        const std::string camera_fov = "camera_field_of_view_degrees";
    }
    enum class Toggle : uint32_t { On, Off };

    enum class Response : uint32_t { Smooth, Direct };
    enum class Invert : uint32_t { None, X, Y, Both };

    template <typename T>
    T option(const std::string& id) {
        return static_cast<T>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(id)));
    }

    void apply_invert(Invert invert, float& x, float& y) {
        if (invert == Invert::X || invert == Invert::Both) x = -x;
        if (invert == Invert::Y || invert == Invert::Both) y = -y;
    }
#endif
}

#if defined(CONKER_RT64)
// Smooth is every input's default. Known: with Direct (or the stick's Direct, which makes every
// input direct), gyro now and then hitches turning left and right, never up and down; with Smooth
// throughout it doesn't. Not yet traced: input noise the spring hides, or something the game does
// to the yaw alone.
void conker::look_aim::add_options(recomp::config::Config& config) {
    using EnumOptions = const std::vector<recomp::config::ConfigOptionEnumOption>;
    static EnumOptions response = {
        {Response::Smooth, "Smooth", "Smooth"},
        {Response::Direct, "Direct", "Direct"},
    };
    static EnumOptions invert = {
        {Invert::None, "None", "None"},
        {Invert::X, "InvertX", "Invert X"},
        {Invert::Y, "InvertY", "Invert Y"},
        {Invert::Both, "InvertBoth", "Invert Both"},
    };
    const std::string about =
        "<br /><recomp-color primary>Smooth</recomp-color>: the view eases toward where you aim, as in the original game."
        "<br /><recomp-color primary>Direct</recomp-color>: the view follows it exactly, with no easing.";
    static EnumOptions turn_invert = {
        {Invert::None, "None", "None"},
        {Invert::X, "InvertX", "Invert X"},
        {Invert::Y, "InvertY", "Invert Y"},
        {Invert::Both, "InvertBoth", "Invert Both"},
    };
    static EnumOptions toggle = {
        {Toggle::On, "On", "On"},
        {Toggle::Off, "Off", "Off"},
    };

    // Grouped by what they're about, each group's names starting alike: the camera, then aiming with
    // the stick, the mouse and gyro. The sensitivities are RecompFrontend's options (same ids, so
    // saved values carry over), added here instead of by its General tab to sit with their group.
    config.add_enum_option(options::camera_turn_invert, "Camera: Invert Turning",
        "Inverts the camera's left and right turning in single player, with the right stick or C-Left and C-Right. "
        "<recomp-color primary>None</recomp-color> matches the original game. Strafing in multiplayer isn't affected. "
        "Y inverts the Free Camera's tilting up and down with the right stick.",
        turn_invert, Invert::None);
    config.add_number_option(options::camera_turn_speed, "Camera: Turning Speed",
        "Sets how fast the camera turns left and right in single player, with the right stick or C-Left and C-Right. "
        "Strafing in multiplayer isn't affected.",
        50.0, 300.0, 5.0, 0, true, 100.0);
    config.add_number_option(options::camera_fov, "Camera: Field of View",
        "How wide the normal camera sees, in degrees, as its vertical field of view (the same whatever the aspect ratio). "
        "<recomp-color primary>50</recomp-color> matches the original game (60.6 degrees across at 4:3). More shows more "
        "around Conker, less brings the view in closer; the camera stays as far away. Only the normal camera: R-Look, "
        "aiming (so the scope's zoom), cutscenes and other special cameras stay as the game has them.",
        35.0, 80.0, 1.0, 0, false, 50.0);
    config.add_enum_option(options::mouse_camera, "Mouse: Turn the Camera",
        "Whether the mouse turns the Free Camera around Conker, outside R-Look and aiming. Needs Mouse: Sensitivity above zero. "
        "<recomp-color primary>Off</recomp-color> leaves that camera to the stick and C-buttons; the mouse still aims in "
        "R-Look and the second aiming mode (e.g. the sniper scope).",
        toggle, Toggle::On);

    config.add_enum_option(options::stick_response, "Stick: Aiming Response",
        "How the view follows the stick in R-Look (hold R and look around)." + about +
        " This also applies to the mouse and gyro, as the view catches up with the stick at once.",
        response, Response::Smooth);
    config.add_enum_option(options::stick_invert, "Stick: Invert Aiming",
        "Inverts the stick in R-Look (hold R and look around) and in the second aiming mode (e.g. the sniper scope, the magnum, throwables), separately from the mouse and gyro. <recomp-color primary>Invert Y</recomp-color> is the default and matches the original game: pushing the stick up looks down.",
        invert, Invert::Y);

    config.add_percent_number_option(recompui::config::general::options::mouse_sensitivity, "Mouse: Sensitivity",
        "How fast the mouse turns the camera and aims, in R-Look (hold R and look around) and the second aiming mode "
        "(e.g. the sniper scope). <b>Zero turns mouse control off</b> and leaves the cursor free. "
        "Mouse buttons can be bound to controls with the keyboard's controls.",
        recompui::is_steam_deck() ? 50.0 : 0.0);
    config.add_enum_option(options::mouse_response, "Mouse: Aiming Response",
        "How the view follows the mouse in R-Look (hold R and look around). Needs Mouse: Sensitivity above zero." + about,
        response, Response::Smooth);
    config.add_enum_option(options::mouse_invert, "Mouse: Invert Aiming",
        "Inverts the mouse in R-Look (hold R and look around) and the second aiming mode (e.g. the sniper scope), separately from the stick and gyro. With <recomp-color primary>None</recomp-color>, moving the mouse up looks up; <recomp-color primary>Invert Y</recomp-color> matches the game's stick, where up looks down.",
        invert, Invert::None);

    config.add_percent_number_option(recompui::config::general::options::gyro_sensitivity, "Gyro: Sensitivity",
        "How strongly gyro aims in R-Look (hold R and look around) and the second aiming mode (e.g. the sniper scope), "
        "on controllers that have it. <b>Zero turns gyro off.</b>"
        "<br /><br /><b>To recalibrate gyro, set the controller down on a still, flat surface for 5 seconds.</b>",
        25.0);
    config.add_enum_option(options::gyro_response, "Gyro: Aiming Response",
        "How the view follows gyro in R-Look (hold R and look around). Needs Gyro: Sensitivity above zero." + about,
        response, Response::Smooth);
    config.add_enum_option(options::gyro_invert, "Gyro: Invert Aiming",
        "Inverts gyro in R-Look (hold R and look around) and the second aiming mode (e.g. the sniper scope), separately from the stick and the mouse. With <recomp-color primary>None</recomp-color>, the view turns the way the controller is turned.",
        invert, Invert::None);
}
#endif

void conker::look_aim::on_input_poll() {
#if defined(CONKER_RT64)
    Movement m;
    recompinput::get_mouse_deltas(&m.mouse_x, &m.mouse_y);
    recompinput::get_gyro_deltas(0, &m.gyro_x, &m.gyro_y);
    std::lock_guard lock{queue_mutex};
    queue.push_back(m);
    while (queue.size() > 2) {
        queue.pop_front();
    }
#endif
}

#if defined(CONKER_RT64)
namespace {
    // The game's own stick is Invert Y, so X turns around when the setting has X, and Y when the
    // setting lacks it.
    bool turn_stick_x() {
        const Invert invert = option<Invert>(options::stick_invert);
        return invert == Invert::X || invert == Invert::Both;
    }
    bool turn_stick_y() {
        const Invert invert = option<Invert>(options::stick_invert);
        return invert == Invert::None || invert == Invert::X;
    }
}
#endif

// The stick's yaw: target - stick x * scale, stored at 0x15120A14 (one state, which then goes on
// through the next one too) and at 0x15120A44 (every state). Turned around, it's a +.
extern "C" void conker_look_stick_yaw_a(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (turn_stick_x()) {
        ctx->f6.fl = ctx->f8.fl + ctx->f18.fl;  // 0x15120A10: sub.s $f6, $f8, $f18
    }
#endif
}

extern "C" void conker_look_stick_yaw_b(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (turn_stick_x()) {
        ctx->f18.fl = ctx->f4.fl + ctx->f10.fl;  // 0x15120A40: sub.s $f18, $f4, $f10
    }
#endif
}

// The stick's pitch: target + stick y * scale, stored at 0x15120A98. Turned around, it's a -.
extern "C" void conker_look_stick_pitch(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (turn_stick_y()) {
        ctx->f10.fl = ctx->f6.fl - ctx->f8.fl;  // 0x15120A94: add.s $f10, $f6, $f8
    }
#endif
}

// Camera: Invert Turning. At 0x1512D3F0 func_1512D390 ($s0 the camera) has just stored the
// C-buttons' turn direction, 1 (C-Right) or -1 (C-Left), at + 0x6B0.
extern "C" void conker_camera_turn_invert(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    const Invert invert = option<Invert>(options::camera_turn_invert);
    if (invert == Invert::X || invert == Invert::Both) {
        MEM_W(0x6B0, ctx->r16) = -MEM_W(0x6B0, ctx->r16);
    }
#endif
}

#if defined(CONKER_RT64)
namespace {
    float camera_turn_speed() {
        return (float)(std::get<double>(recompui::config::get_general_config().get_option_value(options::camera_turn_speed)) / 100.0);
    }
}
#endif

// Camera: Turning Speed. The frame's turn, about to be passed to func_1508EF80: in $f18 while
// C-Left or C-Right is held (0x1512D4C4), in $f4 while the turn glides to a stop (0x1512D53C).
// Only the turn applied is scaled, not the speed the game keeps, so it eases in and out as before.
extern "C" void conker_camera_turn_speed_held(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    ctx->f18.fl *= camera_turn_speed();
#endif
}

extern "C" void conker_camera_turn_speed_released(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    ctx->f4.fl *= camera_turn_speed();
#endif
}

#if defined(CONKER_RT64)
namespace {
    // The mouse and keyboard are player 1's. The camera (struct108, the look mode's and the second
    // aiming mode's $s0) keeps its index into the cameras (D_800BE628) at + 0x23D, 0 for player 1;
    // in multiplayer the aiming code runs for each player's camera in turn.
    bool is_player_one(uint8_t* rdram, gpr camera) {
        return MEM_BU(0x23D, camera) == 0;
    }

    // One poll's mouse and gyro movement as a turn of the view, in degrees: the yaw grows to the left
    // and the pitch downward (see conker_look_targets). False if there was none.
    bool take_movement(float& mouse_yaw, float& mouse_pitch, float& gyro_yaw, float& gyro_pitch) {
        Movement m;
        {
            std::lock_guard lock{queue_mutex};
            if (queue.empty()) {
                return false;
            }
            m = queue.front();
            queue.pop_front();
        }
        mouse_yaw = -m.mouse_x * mouse_degrees_per_pixel;
        mouse_pitch = m.mouse_y * mouse_degrees_per_pixel;
        gyro_yaw = m.gyro_y * gyro_scale;
        gyro_pitch = -m.gyro_x * gyro_scale;
        apply_invert(option<Invert>(options::mouse_invert), mouse_yaw, mouse_pitch);
        apply_invert(option<Invert>(options::gyro_invert), gyro_yaw, gyro_pitch);
        return true;
    }
}
#endif

// The second aiming mode's stick (func_15126378, at 0x15126EA0; $s0 is the camera): the yaw's turn in
// $f14 is about to be subtracted and the pitch's in $f12 added, in degrees, the same directions as
// the look mode's stick (X normal, Y inverted), so the same setting turns them around. Some states
// have scaled or clamped them (symmetrically) already.
//
// The mouse and gyro turn it here too (issue #61: the sniper scope didn't follow the mouse at all).
// This mode turns the aim at once, with no spring to ease it. Zoomed in, the stick turns slower;
// the mouse and gyro turn by the same part of the view: their turn is scaled by the zoom, the
// tangent of the field of view in use (the camera's + 0x74) against the unzoomed one's (+ 0x6C),
// as func_1510B128 sets it.
extern "C" void conker_aim_stick(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    if (turn_stick_x()) {
        ctx->f14.fl = -ctx->f14.fl;
    }
    if (turn_stick_y()) {
        ctx->f12.fl = -ctx->f12.fl;
    }

    const gpr camera = ctx->r16;
    if (!is_player_one(rdram, camera)) {
        return;
    }
    const gpr view = (gpr)(int32_t)((uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE628) + MEM_BU(0x23D, camera) * 0x180);
    constexpr float half_degrees_to_radians = 3.14159265358979f / 360.0f;
    const float fov = read_float(rdram, view, 0x74), unzoomed = read_float(rdram, view, 0x6C);
    float zoom = 1.0f;
    if (fov > 0.0f && unzoomed > 0.0f && fov < 180.0f && unzoomed < 180.0f) {
        zoom = std::clamp(std::tan(fov * half_degrees_to_radians) / std::tan(unzoomed * half_degrees_to_radians), 0.02f, 2.0f);
    }
    // Aiming Crosshair (crosshair.cpp): aiming, and zoomed in (the scope) or not.
    conker::crosshair::aiming(zoom < 0.9f);
    float mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch;
    if (!take_movement(mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch)) {
        return;
    }
    // The yaw's turn is subtracted, the pitch's added.
    ctx->f14.fl -= (mouse_yaw + gyro_yaw) * zoom;
    ctx->f12.fl += (mouse_pitch + gyro_pitch) * zoom;
#endif
}

// At the start of the two paths where the look mode works its yaw target out each frame from the way
// Conker faces ($s0 + 0x3D0 is his object, its + 0x7A his facing) less an aiming angle (+ 0x12 of
// $s0 + 0x3D4, an s16 the stick turns: 65536 to a turn, scaled by 0.35, D_800A33B4, on the second
// path). A yaw added to the target alone was thrown away the next frame: the view sprang back.
extern "C" void conker_look_yaw_from_facing(uint8_t* rdram, recomp_context* ctx) {
    aim_units_per_degree = 65536.0f / 360.0f;
}

extern "C" void conker_look_yaw_from_facing_scaled(uint8_t* rdram, recomp_context* ctx) {
    aim_units_per_degree = 65536.0f / 360.0f / 0.35f;
}

// Before the pitch clamp: $s0 is the look state, $t0 the targets (reloaded at 0x15120B40).
extern "C" void conker_look_targets(uint8_t* rdram, recomp_context* ctx) {
    frame = Frame{};
    const float units_per_degree = aim_units_per_degree;
    aim_units_per_degree = 0.0f;
#if defined(CONKER_RT64)
    // Directions, all measured by playing: the yaw grows to the left and the pitch downward (the
    // game's stick is inverted: up looks down). The mouse's x grows to the right and its y
    // downward. recompinput's gyro gives the controller's turn on y (positive turning left) and
    // its tilt on x (positive tilting it back, toward you). Without inverting, the mouse and gyro
    // look the way they move: mouse or controller up looks up (take_movement).
    if (!is_player_one(rdram, ctx->r16)) {
        return;
    }
    // Aiming Crosshair (crosshair.cpp): the look mode, which the game also puts Conker in to throw
    // (the knives in the barn); only then, not while the player holds R to look around.
    {
        const gpr view = (gpr)(int32_t)((uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE628) + MEM_BU(0x23D, ctx->r16) * 0x180);
        conker::crosshair::look_mode(read_float(rdram, view, 0x78)); // the vertical field of view in use
    }
    float mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch;
    if (!take_movement(mouse_yaw, mouse_pitch, gyro_yaw, gyro_pitch)) {
        return;
    }

    const float yaw = mouse_yaw + gyro_yaw, pitch = mouse_pitch + gyro_pitch;
    if (yaw == 0.0f && pitch == 0.0f) {
        return;
    }
    const gpr targets = ctx->r8;
    write_float(rdram, targets, target_yaw, read_float(rdram, targets, target_yaw) + yaw);
    if (units_per_degree != 0.0f) {
        // Turn the aiming angle as far, for the next frame's target, which grows as it shrinks.
        const gpr aim = (gpr)(int32_t)MEM_W(0x3D4, ctx->r16);
        const float units = yaw * units_per_degree + aim_remainder;
        const int32_t whole = (int32_t)units;
        aim_remainder = units - (float)whole;
        MEM_H(0x12, aim) = (int16_t)(MEM_H(0x12, aim) - whole);
    }
    frame.pitch_target_set = read_float(rdram, targets, target_pitch) + pitch;
    write_float(rdram, targets, target_pitch, frame.pitch_target_set);
    frame.targets_moved = true;
    if (option<Response>(options::mouse_response) == Response::Direct) {
        frame.direct_yaw += mouse_yaw;
        frame.direct_pitch += mouse_pitch;
    }
    if (option<Response>(options::gyro_response) == Response::Direct) {
        frame.direct_yaw += gyro_yaw;
        frame.direct_pitch += gyro_pitch;
    }
#endif
}

// After the springs: $s0 is the look state, $t0 the targets (reloaded at 0x15120E28 or 0x15120E88).
extern "C" void conker_look_currents(uint8_t* rdram, recomp_context* ctx) {
#if defined(CONKER_RT64)
    const gpr state = ctx->r16, targets = ctx->r8;
    if (option<Response>(options::stick_response) == Response::Direct) {
        // The view is where the targets are, and the spring keeps no speed to overshoot with.
        write_float(rdram, state, current_yaw, read_float(rdram, targets, target_yaw));
        write_float(rdram, targets, current_pitch, read_float(rdram, targets, target_pitch));
        write_float(rdram, targets, yaw_velocity, 0.0f);
        write_float(rdram, targets, pitch_velocity, 0.0f);
        return;
    }
    if (!frame.targets_moved) {
        return;
    }
    write_float(rdram, state, current_yaw, read_float(rdram, state, current_yaw) + frame.direct_yaw);
    // The clamp may have held the pitch target back; the current pitch mustn't pass it either.
    const float target = read_float(rdram, targets, target_pitch);
    const float clamped = target - frame.pitch_target_set;
    float pitch = read_float(rdram, targets, current_pitch) + frame.direct_pitch;
    if ((clamped < 0.0f && pitch > target) || (clamped > 0.0f && pitch < target)) {
        pitch = target;
    }
    write_float(rdram, targets, current_pitch, pitch);
#endif
}
