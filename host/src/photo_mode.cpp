// Photo Mode (Conker tab): freezes the game where it is, hides the HUD, and lets the player fly the
// camera around the still scene, hide Conker if wanted, and save pictures of it.
//
// Opening: the P key, or hold L and press D-pad Down, while playing (not paused). The same again or B
// closes it. A saves a picture: full size, as the game drew it (no menus or HUD), as a PNG in the
// Photos folder in the settings folder. Start hides the panel (and shows it again).
//
// The camera: it circles a point (at first the one the game's camera looked at). The right stick or the
// mouse (or the C-buttons) turns it around that point; the left stick moves the point across the ground,
// and up and down while L is held; Z and R (or the scroll wheel) come closer and go further.
//
// The panel (D-pad Up and Down pick a line, Left and Right change it): Conker shown or hidden, his pose (as he
// was, or one of his idle animations), the pose playing or paused, a frame back or on, the lens (the field of
// view), the camera's tilt, a filter and a vignette. Its hints show the player's own buttons or keys.
//
// Freezing: each frame the game's main loop (func_1501878C) runs its update (func_15019130: Conker,
// the objects, the camera, the level's animated textures) and then draws the frame from what that left.
// The game's own pause skips the update the same way, but also draws a kept picture instead of the scene;
// here the pause flag stays clear, so the scene is drawn as usual, from a world that no longer moves.
// The camera's update (func_15122AE0) is part of what's skipped, so the view is built here instead by its
// last step (func_1512C490), and our eye and look-at point go into it where the free camera's turn their
// copies (func_1512C490 at 0x1512C844, conker_mouse_camera_view).
//
// Posing: an object's animation is set by func_1505E650; its clock (+0xB4, kept again in its animation
// state at +0x1DC, and in half frames at +0x5A) is moved on by the object's own update, and its body is
// worked out from the clock in the update's later pass over every object (func_1502BEE4 ->
// func_1502BD84(object, index)). Here the clock is moved on (or back) by us, and only Conker's pass is run
// (and that of whatever hangs off him: +0x65 is 1 + the index of the object a thing is attached to).
//
// Hiding Conker: the game's object drawing (func_1502C974(list, index, ...)) is skipped for him and what
// hangs off him, and he's shrunk to nothing (+0x4C, his size), which takes the props of his idle animations
// (drawn from his hands another way) with him. The HUD (func_1508FD38: the chocolate, the tail, Show Cash) is skipped while it's open.
//
// The lens is the Field of View's (field_of_view.cpp asks photo::lens_degrees()); the tilt turns the
// camera's projection (the frame's, at D_800DC2A0) about the view's axis before the view is built from it.
// The filter and vignette are RT64's (rt64_conker_filter, rt64_conker_vignette: on the screen, in its last
// pass) and worked out the same way here for the picture.
//
// The picture: RT64 copies the next frame shown back from the GPU (rt64_conker_capture_request, before
// the menus are drawn over it), and it's written out on a thread of its own.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "recompinput/input_state.h"
#include "recompinput/input_types.h"
#include "recompui/recompui.h"
#include "elements/ui_label.h"
#include "elements/ui_conker_look.h"
#include "recompinput/profiles.h"
#include "recompinput/recompinput.h"
#include "conker.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

extern "C" volatile int rt64_conker_capture_request; // RT64 (rt64_present_queue.cpp, patches/rt64_conker.patch)
extern "C" void (*volatile rt64_conker_capture_done)(const uint8_t* data, uint32_t width, uint32_t height, uint32_t row_pitch, int hdr);
extern "C" volatile int rt64_conker_filter;   // RT64 (rt64_vi_renderer.cpp): the screen's filter, 0 none
extern "C" volatile int rt64_conker_vignette; // and its vignette, 0 none

extern "C" void func_1512C490(uint8_t* rdram, recomp_context* ctx); // the game: a camera's view from its eye and look-at point
extern "C" void func_1510FC34(uint8_t* rdram, recomp_context* ctx); // the game: which zone of the level a camera's eye is in
extern "C" void func_1510B690(uint8_t* rdram, recomp_context* ctx); // the game: the cameras' frustums, from their views
extern "C" void func_1505E650(uint8_t* rdram, recomp_context* ctx); // the game: an object's animation (obj, anim, speed, blend, 0, 0, 0)
extern "C" void func_15083568(uint8_t* rdram, recomp_context* ctx); // the game: mounts an attachment (D_80086CC4 entry) on an object
extern "C" void func_150836CC(uint8_t* rdram, recomp_context* ctx); // the game: takes an attachment off (obj, id, 1)
extern "C" void func_1502BD84(uint8_t* rdram, recomp_context* ctx); // the game: an object's body from its animation (obj, index)
extern "C" void func_1510B128(uint8_t* rdram, recomp_context* ctx); // the game: a camera's field of view and projection
extern "C" volatile int rt64_conker_no_interpolation; // RT64 (rt64_workload_queue.cpp, patches/rt64_photo.patch)
extern "C" volatile int rt64_conker_tilt;     // RT64 (rt64_vi_renderer.cpp): the screen turned by this many degrees

namespace {
    constexpr uint32_t current_camera = 0x800DBFF0; // D_800DBFF0
    constexpr uint32_t pause_flag = 0x800BEAC0;     // D_800BEAC0
    constexpr uint32_t player_object = 0x800CC2D0;  // Conker (+0x14 position), the first of the objects
    constexpr uint32_t object_size = 0x32C;
    constexpr int object_count = 0x19;
    constexpr uint32_t current_room = 0x800BE9F0;   // D_800BE9F0 (0x1D the bar's menu, 0x21 and 0x25 the opening)

    constexpr uint16_t button_a = 0x8000, button_b = 0x4000, button_z = 0x2000, button_start = 0x1000;
    constexpr uint16_t dpad_up = 0x0800, dpad_down = 0x0400, dpad_left = 0x0200, dpad_right = 0x0100;
    constexpr uint16_t button_l = 0x0020, button_r = 0x0010;
    constexpr uint16_t c_up = 0x0008, c_down = 0x0004, c_left = 0x0002, c_right = 0x0001;

    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    constexpr float turn_degrees_per_second = 120.0f; // full tilt on the right stick, or a C-button
    constexpr float mouse_degrees_per_pixel = 0.2f;
    constexpr float stick_dead_zone = 0.2f;
    constexpr float max_pitch = 85.0f * degrees_to_radians;
    constexpr float nearest = 20.0f, farthest = 3000.0f;
    constexpr float zoom_per_second = 2.5f; // Z / R: the distance scales by this each second held
    constexpr float zoom_step = 1.12f;      // each notch of the scroll wheel
    constexpr float frames_per_second = 30.0f; // the animation clock's frames
    // A new pose starts at once: a blend is moved on by the game's own update, which is frozen, and would stay
    // halfway between the two.
    constexpr float pose_blend = 0.0f;

    // The poses: Conker's idle fidgets (what he does standing still), each played whole and over again. Most are
    // chains of pieces the game plays one after another (sitting: sit down, sit, shift, sit, stand up), each for
    // as long as the game plays it (measured), some more than once.
    struct Piece { int anim; float seconds; int times; };
    struct Fidget { Piece pieces[6]; const char* name = nullptr; };
    constexpr Fidget fidgets[] = {
        { { { 0x26, 0.0f, 1 } } },                                                                // standing
        { { { 0xAB, 2.2f, 1 } }, "Drunk" },                                                     // the hungover sway
        { { { 0x87, 2.0f, 1 }, { 0x89, 1.8f, 2 }, { 0xBB, 1.3f, 1 }, { 0x88, 1.3f, 2 }, { 0x8A, 1.0f, 1 } } }, // sitting
        { { { 0x171, 1.0f, 1 }, { 0x172, 0.6f, 3 }, { 0x174, 1.0f, 1 }, { 0x17A, 1.0f, 1 }, { 0x173, 0.7f, 1 } } }, // reading
        { { { 0x5E, 2.2f, 1 }, { 0x5F, 2.0f, 1 }, { 0xB3, 3.3f, 1 }, { 0xFA, 2.0f, 1 }, { 0x60, 3.7f, 1 } } },
        { { { 0x67, 1.0f, 1 }, { 0x73, 5.0f, 1 }, { 0x86, 0.8f, 1 } } },
        { { { 0x55, 1.4f, 1 }, { 0x57, 3.4f, 1 }, { 0x56, 1.0f, 1 } } },
        { { { 0x04, 1.8f, 2 }, { 0x03, 2.0f, 1 } } },                                            // arms crossed
        { { { 0x49, 1.5f, 1 }, { 0x47, 6.0f, 1 } } },
        { { { 0x25, 4.1f, 1 } } },
        { { { 0x83, 2.0f, 1 } } },
        { { { 0x82, 2.4f, 1 } } },
        { { { 0x9A, 3.3f, 1 } } },
        { { { 0x9B, 6.6f, 1 } } },
        { { { 0x81, 2.2f, 1 } } },
        { { { 0x29, 1.5f, 1 } } },
        { { { 0x11, 1.9f, 1 } } },
        { { { 0x0D, 1.9f, 1 } } },
        { { { 0x0E, 1.3f, 1 } } },
    };
    constexpr int idle_count = (int)(sizeof(fidgets) / sizeof(fidgets[0]));
    constexpr int pose_count = 1 + idle_count; // 0: as he was

    constexpr int lens_min = 15, lens_max = 120, lens_step = 5, lens_start = 50; // degrees, vertical (0: the game's)
    constexpr int tilt_limit = 45, tilt_step = 5;
    const char* const filter_names[] = { "None", "Black & White", "Sepia", "Warm", "Cold", "Vivid", "Faded", "Noir" };
    constexpr int filter_count = (int)(sizeof(filter_names) / sizeof(filter_names[0]));

    enum Row { RowConker, RowPose, RowHat, RowHolding, RowMotion, RowFrame, RowLens, RowTilt, RowFilter, RowVignette, RowCount };
    const char* const row_names[RowCount] = { "Conker", "Pose", "Hat", "Holding", "Motion", "Frame", "Lens", "Tilt", "Filter",
        "Vignette" };

    // Hats and things to hold: the game's own attachments (entries of its table D_80086CC4, 1-based), the ones that
    // sit right on Conker. func_15083568 mounts one (and loads its model), func_150836CC takes it off.
    struct Wearable { int id; const char* name; };
    constexpr Wearable hats[] = { { 0, "None" }, { 35, "Army Helmet" }, { 23, "Gas Mask" }, { 60, "Bat Cap" },
        { 119, "Green Mohawk" }, { 120, "Red Mohawk" }, { 124, "Wolf Hat" } };
    constexpr Wearable holdings[] = { { 0, "Nothing" }, { 22, "Shotgun" }, { 130, "Rifle" }, { 89, "Bazooka" }, { 1, "Club" } };
    constexpr int hat_count = (int)(sizeof(hats) / sizeof(hats[0]));
    constexpr int holding_count = (int)(sizeof(holdings) / sizeof(holdings[0]));

    std::atomic<int64_t> key_toggle_at{ 0 }; // when P was pressed (main thread; steady clock ticks, 0 none)
    std::atomic<int> wheel_notches{ 0 };
    std::atomic<bool> active_flag{ false };
    std::atomic<int64_t> saved_shown_until{ 0 }; // steady clock ticks: "Picture saved" shows until then
    std::atomic<bool> ui_ready{ false };
    std::atomic<int> lens_now{ 0 };   // degrees, 0 the game's (read by field_of_view.cpp)
    std::atomic<int> filter_now{ 0 }, vignette_now{ 0 }, tilt_now{ 0 }; // for the picture

    // What the panel shows (the game thread writes it, the main thread draws it).
    struct Panel {
        bool shown = true;
        int selected = 0;
        std::string values[RowCount];
    };
    std::mutex panel_mutex;
    Panel panel;

    // Game thread.
    bool is_open = false;
    bool toggle_request = false; // L + D-pad Down, until the next frame of play (dropped after half a second: menus)
    std::chrono::steady_clock::time_point toggle_request_time;
    uint16_t buttons_now = 0, buttons_before = 0;
    // Pressed since the last frame (the controller is read on another thread), and this frame's.
    std::atomic<uint16_t> new_presses{ 0 };
    uint16_t frame_presses = 0;
    float stick_x = 0.0f, stick_y = 0.0f;
    bool have_view = false;
    int world_steps = 0; // frames the world runs for while open (a hat or item put on)
    int hat = 0, holding = 0; // indices into hats / holdings
    float last_eye[3] = {}, last_look[3] = {};
    float focus[3] = {};
    float yaw = 0.0f, pitch = 0.0f, distance = 300.0f;
    std::chrono::steady_clock::time_point last_update;

    // The panel's settings.
    int selected = 0;
    bool panel_shown = true;
    bool conker_shown = true;
    float kept_size = 1.0f;  // his size while hidden
    int pose = 0;            // 0 as he was, then 1 + a fidget
    int piece = 0, piece_times = 0; // where the fidget is
    float piece_seconds = 0.0f;
    bool playing = false;
    int lens = 0;            // degrees, 0 the game's
    int tilt = 0;            // degrees
    int filter = 0;
    bool vignette = false;
    // Holding Left or Right repeats.
    int held_direction = 0;
    std::chrono::steady_clock::time_point held_since, held_next;

    // Conker's animation as Photo Mode found it, to put back.
    bool posed = false;      // we've changed his animation or its clock
    int original_anim = 0;
    float original_clock = 0.0f;
    int16_t original_half_frames = 0;

    // The game's last call of its field of view step for player 1's camera (func_1510B128: $a0-$a3 and the word
    // at $sp+0x10), made again each frame while it's open: the camera's update that makes it is frozen.
    bool fov_args_kept = false;
    gpr fov_args[4] = {};
    int32_t fov_arg_stack = 0;

    // Testing: CONKER_TEST_PHOTO="secs ..." opens or closes it at those game seconds, CONKER_TEST_PHOTO_SHOT="secs ..."
    // saves pictures then.
    std::vector<double> test_toggles, test_shots;
    size_t next_toggle = 0, next_shot = 0;

    float read_float(uint8_t* rdram, gpr base, int32_t offset) {
        uint32_t word = (uint32_t)MEM_W(offset, base);
        float value;
        std::memcpy(&value, &word, sizeof(value));
        return value;
    }

    uint32_t float_bits(float value) {
        uint32_t word;
        std::memcpy(&word, &value, sizeof(word));
        return word;
    }

    void write_float(uint8_t* rdram, gpr base, int32_t offset, float value) {
        uint32_t word;
        std::memcpy(&word, &value, sizeof(word));
        MEM_W(offset, base) = (int32_t)word;
    }

    std::vector<double> parse_seconds(const char* name) {
        std::vector<double> out;
        if (const char* s = std::getenv(name)) {
            for (const char* p = s; *p != '\0';) {
                char* end = nullptr;
                double v = std::strtod(p, &end);
                if (end == p) { p++; continue; }
                out.push_back(v);
                p = end;
            }
        }
        return out;
    }

    int SDLCALL watch_events(void*, SDL_Event* event) {
        if (event->type == SDL_KEYDOWN && event->key.repeat == 0 && event->key.keysym.scancode == SDL_SCANCODE_P) {
            key_toggle_at = std::chrono::steady_clock::now().time_since_epoch().count();
        }
        else if (event->type == SDL_MOUSEWHEEL && active_flag) {
            wheel_notches.fetch_add(event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event->wheel.y : event->wheel.y);
        }
        return 1;
    }

    // The filters, as RT64's last pass draws them (VideoInterfacePS.hlsl): colour 0-1, u and v 0-1 across the picture.
    void apply_filter(float c[3], int which, bool vignette_on, float u, float v) {
        auto luma = [&]() { return c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f; };
        switch (which) {
        case 1: { const float l = luma(); c[0] = c[1] = c[2] = l; break; }
        case 2: {
            const float r = c[0], g = c[1], b = c[2];
            c[0] = r * 0.393f + g * 0.769f + b * 0.189f;
            c[1] = r * 0.349f + g * 0.686f + b * 0.168f;
            c[2] = r * 0.272f + g * 0.534f + b * 0.131f;
            break;
        }
        case 3: c[0] *= 1.10f; c[1] *= 1.02f; c[2] *= 0.86f; break;
        case 4: c[0] *= 0.88f; c[1] *= 0.98f; c[2] *= 1.14f; break;
        case 5: { const float l = luma(); for (int i = 0; i < 3; i++) c[i] = ((l + (c[i] - l) * 1.45f) - 0.5f) * 1.08f + 0.5f; break; }
        case 6: { const float l = luma(); for (int i = 0; i < 3; i++) c[i] = (l + (c[i] - l) * 0.65f) * 0.85f + 0.10f; break; }
        case 7: { const float l = (luma() - 0.5f) * 1.5f + 0.5f; c[0] = c[1] = c[2] = l; break; }
        default: break;
        }
        if (vignette_on || which == 7) {
            const float du = u - 0.5f, dv = v - 0.5f;
            const float d = std::sqrt(du * du + dv * dv) * 1.2f;
            const float t = std::clamp((d - 0.35f) / (0.85f - 0.35f), 0.0f, 1.0f);
            const float k = 1.0f - 0.55f * t * t * (3.0f - 2.0f * t);
            for (int i = 0; i < 3; i++) c[i] *= k;
        }
        for (int i = 0; i < 3; i++) c[i] = std::clamp(c[i], 0.0f, 1.0f);
    }

    // Where a point of the tilted picture (u, v 0-1) comes from in the untilted one, as RT64's last pass turns
    // the screen (VideoInterfacePS.hlsl): turned about the middle, and enlarged just enough to leave no corners.
    void tilt_source(float u, float v, float aspect, int degrees, float* su, float* sv) {
        const float a = degrees * degrees_to_radians, c = std::cos(a), s = std::sin(a);
        const float fill = c + std::max(aspect, 1.0f / aspect) * std::abs(s);
        const float px = (u - 0.5f) * aspect, py = v - 0.5f;
        const float qx = (c * px - s * py) / fill, qy = (s * px + c * py) / fill;
        *su = qx / aspect + 0.5f;
        *sv = qy + 0.5f;
    }

    void tilt_picture(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, int degrees) {
        const std::vector<uint8_t> source = pixels;
        const float aspect = (float)width / (float)height;
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                float su, sv;
                tilt_source((x + 0.5f) / width, (y + 0.5f) / height, aspect, degrees, &su, &sv);
                const float fx = std::clamp(su * width - 0.5f, 0.0f, width - 1.001f), fy = std::clamp(sv * height - 0.5f, 0.0f, height - 1.001f);
                const uint32_t x0 = (uint32_t)fx, y0 = (uint32_t)fy;
                const float tx = fx - x0, ty = fy - y0;
                uint8_t* out = pixels.data() + ((size_t)y * width + x) * 4;
                for (int i = 0; i < 3; i++) {
                    auto at = [&](uint32_t xx, uint32_t yy) { return (float)source[((size_t)yy * width + xx) * 4 + i]; };
                    const float top = at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx;
                    const float bottom = at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx;
                    out[i] = (uint8_t)(top * (1 - ty) + bottom * ty + 0.5f);
                }
            }
        }
    }

    // The picture, on RT64's present thread: kept as RGBA8 (fully opaque) and written out on a thread of its own.
    void capture_done(const uint8_t* data, uint32_t width, uint32_t height, uint32_t row_pitch, int hdr) {
        std::vector<uint8_t> pixels((size_t)width * height * 4);
        const int which = filter_now;
        const bool vignette_on = vignette_now != 0;
        const int tilt_degrees = tilt_now;
        for (uint32_t y = 0; y < height; y++) {
            const uint8_t* row = data + (size_t)y * row_pitch;
            uint8_t* out = pixels.data() + (size_t)y * width * 4;
            for (uint32_t x = 0; x < width; x++) {
                if (hdr) {
                    const uint16_t* p = reinterpret_cast<const uint16_t*>(row) + x * 4;
                    out[x * 4 + 0] = (uint8_t)(p[0] >> 8);
                    out[x * 4 + 1] = (uint8_t)(p[1] >> 8);
                    out[x * 4 + 2] = (uint8_t)(p[2] >> 8);
                }
                else {
                    std::memcpy(out + x * 4, row + x * 4, 3);
                }
                out[x * 4 + 3] = 255;
            }
        }
        if (tilt_degrees != 0) {
            tilt_picture(pixels, width, height, tilt_degrees);
        }
        if (which != 0 || vignette_on) {
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    uint8_t* px = pixels.data() + ((size_t)y * width + x) * 4;
                    float c[3] = { px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f };
                    apply_filter(c, which, vignette_on, (x + 0.5f) / width, (y + 0.5f) / height);
                    for (int i = 0; i < 3; i++) px[i] = (uint8_t)(c[i] * 255.0f + 0.5f);
                }
            }
        }
        std::thread([pixels = std::move(pixels), width, height]() {
            std::error_code ec;
            const std::filesystem::path folder = recomp::get_config_path() / "Photos";
            std::filesystem::create_directories(folder, ec);
            std::time_t now = std::time(nullptr);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char stamp[64];
            std::strftime(stamp, sizeof(stamp), "Conker_%Y-%m-%d_%H-%M-%S", &local);
            std::filesystem::path file = folder / (std::string(stamp) + ".png");
            for (int n = 2; std::filesystem::exists(file, ec); n++) {
                file = folder / (std::string(stamp) + "_" + std::to_string(n) + ".png");
            }
            const bool ok = stbi_write_png(file.string().c_str(), (int)width, (int)height, 4, pixels.data(), (int)width * 4) != 0;
            std::printf("[photo] %s %ux%u: %s\n", ok ? "saved" : "could not save", width, height, file.string().c_str());
            if (ok) {
                saved_shown_until = (std::chrono::steady_clock::now() + std::chrono::milliseconds(2500)).time_since_epoch().count();
            }
        }).detach();
    }

    bool pressed(uint16_t button) {
        return (frame_presses & button) != 0;
    }

    float shape(float v) {
        const float a = std::abs(v);
        return a < stick_dead_zone ? 0.0f : std::copysign((a - stick_dead_zone) / (1.0f - stick_dead_zone), v);
    }

    void right_stick(float* x, float* y) {
        if (conker::testing::right_stick(x, y)) {
            return;
        }
        using recompinput::InputField;
        *x = shape(recompinput::get_input_analog(0, InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTX, true)) -
            recompinput::get_input_analog(0, InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTX, false)));
        *y = shape(recompinput::get_input_analog(0, InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTY, false)) -
            recompinput::get_input_analog(0, InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTY, true)));
    }

    // Conker, or something that hangs off him (his gun, what he holds).
    bool part_of_conker(uint8_t* rdram, int index) {
        for (int guard = 0; index != 0 && guard < 8; guard++) {
            const int owner = MEM_BU(0x65, (gpr)(int32_t)(player_object + index * object_size));
            if (owner == 0) {
                return false;
            }
            index = owner - 1;
        }
        return index == 0;
    }

    gpr conker_object() {
        return (gpr)(int32_t)player_object;
    }

    gpr animation_state(uint8_t* rdram) {
        return (gpr)(int32_t)MEM_W(0x31C, conker_object());
    }

    int current_anim(uint8_t* rdram) {
        return (int)((uint32_t)MEM_W(0x84, conker_object()) >> 16);
    }

    float clock_now(uint8_t* rdram) {
        return read_float(rdram, conker_object(), 0xB4);
    }

    void set_clock(uint8_t* rdram, float clock) {
        const gpr state = animation_state(rdram);
        write_float(rdram, conker_object(), 0xB4, clock);
        if (state != 0) {
            write_float(rdram, state, 0x1DC, clock);
            MEM_H(0x5A, state) = (int16_t)(int)(clock * 2.0f);
        }
    }

    void set_anim(uint8_t* rdram, recomp_context* ctx, int anim, float blend) {
        recomp_context saved = *ctx;
        const gpr sp = ctx->r29 - 0x80;
        MEM_W(0x10, sp) = 0;
        MEM_W(0x14, sp) = 0;
        MEM_W(0x18, sp) = 0;
        ctx->r4 = conker_object();
        ctx->r5 = anim;
        ctx->r6 = (gpr)(int32_t)float_bits(1.0f);
        ctx->r7 = (gpr)(int32_t)float_bits(blend);
        ctx->r29 = sp;
        func_1505E650(rdram, ctx);
        *ctx = saved;
    }

    // Conker's body (and what hangs off him) from his animation's clock.
    void pose_conker(uint8_t* rdram, recomp_context* ctx) {
        recomp_context saved = *ctx;
        for (int i = 0; i < object_count; i++) {
            const gpr object = (gpr)(int32_t)(player_object + i * object_size);
            if (MEM_W(0, object) != 0 && part_of_conker(rdram, i)) {
                ctx->r4 = object;
                ctx->r5 = i;
                func_1502BD84(rdram, ctx);
            }
        }
        *ctx = saved;
    }

    // Hidden, he's also shrunk to nothing (+0x4C, his size): what he holds in his idle animations (a lollipop, a
    // game, a can) is drawn from his hands by another way than his own drawing, and shrinks with him.
    void set_conker_shown(uint8_t* rdram, recomp_context* ctx, bool shown) {
        if (shown == conker_shown) {
            return;
        }
        conker_shown = shown;
        if (!shown) {
            kept_size = read_float(rdram, conker_object(), 0x4C);
            write_float(rdram, conker_object(), 0x4C, 0.001f);
        }
        else {
            write_float(rdram, conker_object(), 0x4C, kept_size);
        }
        pose_conker(rdram, ctx);
    }

    // Starts a piece of the pose's fidget from its beginning.
    void start_piece(uint8_t* rdram, recomp_context* ctx) {
        const Piece& p = fidgets[pose - 1].pieces[piece];
        set_anim(rdram, ctx, p.anim, pose_blend);
        set_clock(rdram, 0.0f);
        piece_seconds = 0.0f;
    }

    void keep_original_pose(uint8_t* rdram) {
        if (!posed) {
            posed = true;
            original_anim = current_anim(rdram);
            original_clock = clock_now(rdram);
            const gpr state = animation_state(rdram);
            original_half_frames = state != 0 ? (int16_t)MEM_H(0x5A, state) : 0;
        }
    }

    void apply_pose(uint8_t* rdram, recomp_context* ctx) {
        keep_original_pose(rdram);
        if (pose == 0) {
            set_anim(rdram, ctx, original_anim, pose_blend);
            set_clock(rdram, original_clock);
        }
        else {
            piece = 0;
            piece_times = 0;
            start_piece(rdram, ctx);
        }
        playing = true;
        std::printf("[photo] pose %d (anim %X)\n", pose, current_anim(rdram));
    }

    void put_pose_back(uint8_t* rdram, recomp_context* ctx) {
        if (!posed) {
            return;
        }
        set_anim(rdram, ctx, original_anim, 0.0f);
        set_clock(rdram, original_clock);
        const gpr state = animation_state(rdram);
        if (state != 0) MEM_H(0x5A, state) = original_half_frames;
        pose_conker(rdram, ctx);
        posed = false;
    }

    // Puts one of the game's attachments on Conker or takes it off. It only takes its place in the game's update, so
    // the world then runs for a couple of frames.
    void wear(uint8_t* rdram, recomp_context* ctx, bool on, int id) {
        if (id == 0) return;
        recomp_context saved = *ctx;
        const gpr sp = ctx->r29 - 0x80;
        MEM_W(0x10, sp) = 0; MEM_W(0x14, sp) = 0; MEM_W(0x18, sp) = 0;
        ctx->r4 = (gpr)(int32_t)player_object; ctx->r5 = id; ctx->r6 = on ? 0 : 1; ctx->r7 = 0; ctx->r29 = sp;
        if (on) func_15083568(rdram, ctx); else func_150836CC(rdram, ctx);
        *ctx = saved;
        if (is_open) world_steps = 2;
    }

    void open(uint8_t* rdram) {
        const int32_t room = MEM_W(0, (gpr)(int32_t)current_room);
        if (!have_view || MEM_BU(0, (gpr)(int32_t)pause_flag) != 0 || room == 0x1D || room == 0x21 || room == 0x25) {
            return;
        }
        float d[3];
        for (int i = 0; i < 3; i++) {
            focus[i] = last_look[i];
            d[i] = last_eye[i] - last_look[i];
        }
        distance = std::clamp(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), nearest, farthest);
        pitch = std::clamp(std::asin(std::clamp(d[1] / distance, -1.0f, 1.0f)), -max_pitch, max_pitch);
        yaw = std::atan2(d[0], d[2]);
        // Testing: CONKER_TEST_PHOTO_VIEW="yaw pitch distance" (degrees) starts it circling Conker, 40 up, from there.
        if (const char* v = std::getenv("CONKER_TEST_PHOTO_VIEW")) {
            float vy = 0.0f, vp = 0.0f, vd = 300.0f;
            std::sscanf(v, "%f %f %f", &vy, &vp, &vd);
            for (int i = 0; i < 3; i++) focus[i] = read_float(rdram, (gpr)(int32_t)player_object, 0x14 + i * 4);
            focus[1] += 40.0f;
            yaw = vy * degrees_to_radians;
            pitch = vp * degrees_to_radians;
            distance = vd;
        }
        float mx, my;
        recompinput::get_mouse_deltas(&mx, &my); // what the mouse moved before doesn't count
        wheel_notches = 0;
        last_update = std::chrono::steady_clock::now();
        // Each time it opens: Conker as he is, still, with the game's lens and nothing else.
        selected = 0;
        panel_shown = true;
        conker_shown = true;
        pose = 0;
        playing = false;
        lens = 0;
        tilt = 0;
        filter = 0;
        vignette = false;
        hat = 0;
        holding = 0;
        world_steps = 0;
        posed = false;
        held_direction = 0;
        is_open = true;
        active_flag = true;
        rt64_conker_no_interpolation = 1;
        std::printf("[photo] open\n");
    }

    void close(uint8_t* rdram, recomp_context* ctx) {
        wear(rdram, ctx, false, hats[hat].id);
        wear(rdram, ctx, false, holdings[holding].id);
        hat = holding = 0;
        set_conker_shown(rdram, ctx, true);
        put_pose_back(rdram, ctx);
        lens = 0;
        lens_now = 0;
        rt64_conker_filter = 0;
        rt64_conker_vignette = 0;
        rt64_conker_tilt = 0;
        is_open = false;
        active_flag = false;
        rt64_conker_no_interpolation = 0;
        std::printf("[photo] closed\n");
    }

    void eye_position(float out[3]) {
        const float across = distance * std::cos(pitch);
        out[0] = focus[0] + across * std::sin(yaw);
        out[1] = focus[1] + distance * std::sin(pitch);
        out[2] = focus[2] + across * std::cos(yaw);
    }

    void move_camera(float dt) {
        // Turning: the right stick (or else the C-buttons), and the mouse.
        float tx = 0.0f, ty = 0.0f;
        right_stick(&tx, &ty);
        if (tx == 0.0f && ty == 0.0f) {
            tx = ((buttons_now & c_right) ? 1.0f : 0.0f) - ((buttons_now & c_left) ? 1.0f : 0.0f);
            ty = ((buttons_now & c_up) ? 1.0f : 0.0f) - ((buttons_now & c_down) ? 1.0f : 0.0f);
        }
        if (conker::camera_inverted()) tx = -tx;
        if (conker::camera_tilt_inverted()) ty = -ty;
        float mx = 0.0f, my = 0.0f;
        recompinput::get_mouse_deltas(&mx, &my);
        yaw += tx * turn_degrees_per_second * degrees_to_radians * dt + mx * mouse_degrees_per_pixel * degrees_to_radians;
        pitch += ty * turn_degrees_per_second * degrees_to_radians * dt + my * mouse_degrees_per_pixel * degrees_to_radians;
        pitch = std::clamp(pitch, -max_pitch, max_pitch);

        // Closer and further.
        float scale = 1.0f;
        if (buttons_now & button_z) scale /= std::pow(zoom_per_second, dt);
        if (buttons_now & button_r) scale *= std::pow(zoom_per_second, dt);
        scale *= std::pow(zoom_step, (float)-wheel_notches.exchange(0));
        distance = std::clamp(distance * scale, nearest, farthest);

        // Moving the point it circles: the left stick across the ground (as the camera faces), or up and down with L held.
        const float speed = 150.0f + distance * 0.8f;
        if (buttons_now & button_l) {
            focus[1] += stick_y * speed * dt;
            return;
        }
        const float forward_x = -std::sin(yaw), forward_z = -std::cos(yaw);
        const float right_x = -forward_z, right_z = forward_x;
        focus[0] += (forward_x * stick_y + right_x * stick_x) * speed * dt;
        focus[2] += (forward_z * stick_y + right_z * stick_x) * speed * dt;
    }

    // Left (-1) or Right (+1) on the D-pad this frame: a press, then repeats while held.
    int panel_direction() {
        const auto now = std::chrono::steady_clock::now();
        int held = ((buttons_now & dpad_right) ? 1 : 0) - ((buttons_now & dpad_left) ? 1 : 0);
        const int press = (pressed(dpad_right) ? 1 : 0) - (pressed(dpad_left) ? 1 : 0);
        if (press != 0) {
            held_direction = press;
            held_since = now;
            held_next = now + std::chrono::milliseconds(350);
            return press;
        }
        if (held == 0 || held != held_direction) {
            held_direction = 0;
            return 0;
        }
        if (now >= held_next) {
            // Faster the longer it's held.
            const bool fast = now - held_since > std::chrono::seconds(2);
            held_next = now + std::chrono::milliseconds(fast ? 30 : 90);
            return held;
        }
        return 0;
    }

    void use_panel(uint8_t* rdram, recomp_context* ctx) {
        if (pressed(button_start)) {
            panel_shown = !panel_shown;
        }
        if (!(buttons_now & button_l)) {
            if (pressed(dpad_down)) selected = (selected + 1) % RowCount;
            if (pressed(dpad_up)) selected = (selected + RowCount - 1) % RowCount;
        }
        const int step = panel_direction();
        if (step == 0) {
            return;
        }
        switch (selected) {
        case RowConker:
            set_conker_shown(rdram, ctx, !conker_shown);
            break;
        case RowPose:
            pose = (pose + step + pose_count) % pose_count;
            apply_pose(rdram, ctx);
            break;
        case RowMotion: playing = !playing; break;
        case RowFrame: {
            keep_original_pose(rdram);
            playing = false;
            set_clock(rdram, std::max(std::floor(clock_now(rdram)) + (float)step, 0.0f));
            pose_conker(rdram, ctx);
            break;
        }
        case RowLens:
            if (lens == 0) lens = lens_start;
            lens = std::clamp(lens + step * lens_step, lens_min, lens_max);
            break;
        case RowTilt: tilt = std::clamp(tilt + step * tilt_step, -tilt_limit, tilt_limit); break;
        case RowFilter: filter = (filter + step + filter_count) % filter_count; break;
        case RowVignette: vignette = !vignette; break;
        case RowHat:
            wear(rdram, ctx, false, hats[hat].id);
            hat = (hat + step + hat_count) % hat_count;
            wear(rdram, ctx, true, hats[hat].id);
            break;
        case RowHolding:
            wear(rdram, ctx, false, holdings[holding].id);
            holding = (holding + step + holding_count) % holding_count;
            wear(rdram, ctx, true, holdings[holding].id);
            break;
        default: break;
        }
    }

    // The animation plays on (the game's own working out of the body goes round within the animation) and
    // Conker's body follows.
    void play_pose(uint8_t* rdram, recomp_context* ctx, float dt) {
        if (!playing) {
            return;
        }
        keep_original_pose(rdram);
        set_clock(rdram, clock_now(rdram) + dt * frames_per_second);
        // On to the fidget's next piece (or the same again) when this one's done; after the last, from the start.
        if (pose > 0) {
            const Fidget& f = fidgets[pose - 1];
            const Piece& p = f.pieces[piece];
            piece_seconds += dt;
            if (p.seconds > 0.0f && piece_seconds >= p.seconds) {
                if (++piece_times >= p.times) {
                    piece_times = 0;
                    piece = (piece + 1 < 6 && f.pieces[piece + 1].anim != 0) ? piece + 1 : 0;
                }
                start_piece(rdram, ctx);
            }
        }
        pose_conker(rdram, ctx);
    }

    void update_panel_text(uint8_t* rdram) {
        char text[96];
        Panel next;
        next.shown = panel_shown;
        next.selected = selected;
        next.values[RowConker] = conker_shown ? "Shown" : "Hidden";
        if (pose == 0) {
            next.values[RowPose] = "As he was";
        }
        else if (fidgets[pose - 1].name != nullptr) {
            next.values[RowPose] = fidgets[pose - 1].name;
        }
        else {
            std::snprintf(text, sizeof(text), "Idle %d of %d", pose, idle_count);
            next.values[RowPose] = text;
        }
        next.values[RowMotion] = playing ? "Playing" : "Paused";
        std::snprintf(text, sizeof(text), "%d", (int)clock_now(rdram));
        next.values[RowFrame] = text;
        if (lens == 0) next.values[RowLens] = "Normal";
        else { std::snprintf(text, sizeof(text), "%d\xC2\xB0", lens); next.values[RowLens] = text; }
        std::snprintf(text, sizeof(text), "%d\xC2\xB0", tilt);
        next.values[RowTilt] = text;
        next.values[RowFilter] = filter_names[filter];
        next.values[RowVignette] = vignette ? "On" : "Off";
        next.values[RowHat] = hats[hat].name;
        next.values[RowHolding] = holdings[holding].name;
        std::lock_guard lock(panel_mutex);
        panel = next;
    }
}

bool conker::photo::active() { return active_flag; }

int conker::photo::lens_degrees() { return active_flag ? lens_now.load() : 0; }

// frontend.cpp, once SDL is up.
void conker::photo::init() {
    SDL_AddEventWatch(watch_events, nullptr);
    rt64_conker_capture_done = capture_done;
    test_toggles = parse_seconds("CONKER_TEST_PHOTO");
    test_shots = parse_seconds("CONKER_TEST_PHOTO_SHOT");
}

// frontend.cpp's get_input, player 1: the buttons and stick are kept for the camera, and the game gets none
// while Photo Mode is open. Hold L and press D-pad Down opens and closes it.
void conker::photo::filter_input(uint16_t* buttons, float* x, float* y) {
    buttons_before = buttons_now;
    buttons_now = *buttons;
    new_presses.fetch_or((uint16_t)(buttons_now & ~buttons_before));
    stick_x = *x;
    stick_y = *y;
    if ((buttons_now & button_l) && (buttons_now & ~buttons_before & dpad_down)) {
        toggle_request = true;
        toggle_request_time = std::chrono::steady_clock::now();
    }
    if (toggle_request && std::chrono::steady_clock::now() - toggle_request_time > std::chrono::milliseconds(500)) {
        toggle_request = false;
    }
    if (is_open) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
    }
}

// Hook: the start of the game's update (func_15019130). True skips it (with $v0 = 1, as it returns).
// Testing: CONKER_TEST_ATTACH="seconds:id[:seconds2]" mounts that attachment on Conker then (as the game does at a
// level's start), and at seconds2 calls the other attachment step (func_150836CC(obj, id, 1)).
static void test_attach(uint8_t* rdram, recomp_context* ctx) {
    static double at = -1.0, at2 = -1.0; static int id = 0; static bool done = false, done2 = false, read = false;
    if (!read) { read = true; if (const char* p = std::getenv("CONKER_TEST_ATTACH")) std::sscanf(p, "%lf:%d:%lf", &at, &id, &at2); }
    if (done && !done2 && at2 > 0.0 && conker::testing::game_seconds() >= at2) {
        done2 = true;
        recomp_context saved = *ctx;
        const gpr sp = ctx->r29 - 0x80;
        MEM_W(0x10, sp) = 0; MEM_W(0x14, sp) = 0;
        ctx->r4 = (gpr)(int32_t)player_object; ctx->r5 = id; ctx->r6 = 1; ctx->r7 = 0; ctx->r29 = sp;
        func_150836CC(rdram, ctx);
        std::printf("[attach] test: step 2 for %d, returned %08X\n", id, (uint32_t)ctx->r2);
        *ctx = saved;
    }
    if (done || at < 0.0 || conker::testing::game_seconds() < at) return;
    done = true;
    recomp_context saved = *ctx;
    const gpr sp = ctx->r29 - 0x80;
    MEM_W(0x10, sp) = 0; MEM_W(0x14, sp) = 0; MEM_W(0x18, sp) = 0;
    ctx->r4 = (gpr)(int32_t)player_object; ctx->r5 = id; ctx->r6 = 0; ctx->r7 = 0; ctx->r29 = sp;
    func_15083568(rdram, ctx);
    std::printf("[attach] test: mounted %d on Conker, returned %08X\n", id, (uint32_t)ctx->r2);
    *ctx = saved;
}

// Testing: CONKER_TEST_ATTACH_SWEEP="start:id,id,...": from start (game seconds), every 2 seconds mounts the next
// attachment on Conker and takes it off again 1.6 seconds later (snap them at start + 2k + 1).
static void attach_call(uint8_t* rdram, recomp_context* ctx, bool mount, int id) {
    wear(rdram, ctx, mount, id);
}

static void test_attach_sweep(uint8_t* rdram, recomp_context* ctx) {
    static double start = -1.0; static std::vector<int> ids; static bool read = false; static size_t k = 0; static bool on = false;
    if (!read) {
        read = true;
        if (const char* p = std::getenv("CONKER_TEST_ATTACH_SWEEP")) {
            start = std::atof(p);
            for (const char* c = std::strchr(p, ':'); c && *c; ) { ids.push_back(std::atoi(c + 1)); c = std::strchr(c + 1, ','); }
        }
    }
    if (start < 0.0 || k >= ids.size()) return;
    const double t = conker::testing::game_seconds() - start - 2.0 * k;
    if (!on && t >= 0.0) { attach_call(rdram, ctx, true, ids[k]); on = true; std::printf("[sweep] %d on\n", ids[k]); }
    if (on && t >= 1.6) { attach_call(rdram, ctx, false, ids[k]); on = false; k++; }
}

extern "C" int conker_photo_frame(uint8_t* rdram, recomp_context* ctx) {
    test_attach(rdram, ctx);
    test_attach_sweep(rdram, ctx);
    const double t = conker::testing::game_seconds();
    frame_presses = new_presses.exchange(0);
    // (A press of P counts for half a second: one in a menu or on the pause screen is dropped.)
    const int64_t key_at = key_toggle_at.exchange(0);
    const bool key_fresh = key_at != 0 &&
        std::chrono::steady_clock::now().time_since_epoch().count() - key_at < std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(500)).count();
    bool toggle = key_fresh || toggle_request;
    toggle_request = false;
    if (next_toggle < test_toggles.size() && t >= test_toggles[next_toggle]) {
        next_toggle++;
        toggle = true;
    }
    if (is_open && pressed(button_b)) {
        toggle = true;
    }
    if (toggle) {
        if (is_open) {
            close(rdram, ctx);
        }
        else if (conker::photo::enabled()) {
            open(rdram);
            // Testing: CONKER_TEST_PHOTO_POSE=n starts with pose n, playing.
            static const int test_pose = [] { const char* p = std::getenv("CONKER_TEST_PHOTO_POSE"); return p ? std::atoi(p) : 0; }();
            if (is_open && test_pose > 0 && test_pose < pose_count) {
                pose = test_pose;
                apply_pose(rdram, ctx);
            }
            // Testing: CONKER_TEST_PHOTO_WEAR="hat,holding" (indices into hats / holdings) on opening.
            if (const char* w = std::getenv("CONKER_TEST_PHOTO_WEAR"); w && is_open) {
                int h = 0, g = 0;
                std::sscanf(w, "%d,%d", &h, &g);
                hat = std::clamp(h, 0, hat_count - 1);
                holding = std::clamp(g, 0, holding_count - 1);
                wear(rdram, ctx, true, hats[hat].id);
                wear(rdram, ctx, true, holdings[holding].id);
            }
        }
    }
    if (!is_open) {
        return false;
    }
    if (pressed(button_a) || (next_shot < test_shots.size() && t >= test_shots[next_shot])) {
        if (next_shot < test_shots.size() && t >= test_shots[next_shot]) next_shot++;
        rt64_conker_capture_request = 1;
        std::printf("[photo] picture asked for\n");
    }
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(std::chrono::duration<float>(now - last_update).count(), 0.1f);
    last_update = now;
    use_panel(rdram, ctx);
    move_camera(dt);
    play_pose(rdram, ctx, dt);
    lens_now = lens;
    filter_now = filter;
    vignette_now = vignette ? 1 : 0;
    rt64_conker_filter = filter;
    rt64_conker_vignette = vignette ? 1 : 0;
    rt64_conker_tilt = tilt;
    tilt_now = tilt;
    update_panel_text(rdram);
    // A hat or item put on (or taken off) only takes its place in the game's update: the world runs for a frame.
    if (world_steps > 0) {
        world_steps--;
        return false;
    }
    const gpr camera = (gpr)(int32_t)MEM_W(0, (gpr)(int32_t)current_camera);
    if (camera != 0) {
        // The level is drawn by zones, those seen from the zone the camera's eye (+0x2F8, its x and z) is in: that's
        // looked up with ours in its place, and the game's put back.
        float eye[3];
        eye_position(eye);
        const int32_t eye_x = MEM_W(0x2F8, camera), eye_z = MEM_W(0x300, camera);
        write_float(rdram, camera, 0x2F8, eye[0]);
        write_float(rdram, camera, 0x300, eye[2]);
        ctx->r4 = 0;
        func_1510FC34(rdram, ctx);
        MEM_W(0x2F8, camera) = eye_x;
        MEM_W(0x300, camera) = eye_z;
        // The lens (the field of view and projection, Field of View's hook putting ours in), the view, then
        // the frustum the world is culled with.
        if (fov_args_kept) {
            recomp_context saved = *ctx;
            const gpr sp = ctx->r29 - 0x80;
            MEM_W(0x10, sp) = fov_arg_stack;
            ctx->r4 = fov_args[0]; ctx->r5 = fov_args[1]; ctx->r6 = fov_args[2]; ctx->r7 = fov_args[3];
            ctx->r29 = sp;
            func_1510B128(rdram, ctx);
            *ctx = saved;
        }
        ctx->r4 = camera;
        func_1512C490(rdram, ctx);
        func_1510B690(rdram, ctx);
    }
    ctx->r2 = 1;
    return true;
}

// From conker_mouse_camera_view (free_camera.cpp): func_1512C490 about to build a camera's view from its copies of
// the eye ($sp+0xEC) and the look-at point ($sp+0xE0). The game's are kept (where Photo Mode starts from); while
// it's open, ours replace them. True when ours were put in.
bool conker::photo::view(uint8_t* rdram, recomp_context* ctx) {
    const gpr camera = ctx->r16;
    if ((uint32_t)camera != (uint32_t)MEM_W(0, (gpr)(int32_t)current_camera)) {
        return false;
    }
    const gpr eye = ctx->r29 + 0xEC, look = ctx->r29 + 0xE0;
    if (!is_open) {
        for (int i = 0; i < 3; i++) {
            last_eye[i] = read_float(rdram, eye, i * 4);
            last_look[i] = read_float(rdram, look, i * 4);
        }
        have_view = true;
        return false;
    }
    float ours[3];
    eye_position(ours);
    for (int i = 0; i < 3; i++) {
        write_float(rdram, eye, i * 4, ours[i]);
    }
    for (int i = 0; i < 3; i++) {
        write_float(rdram, look, i * 4, focus[i]);
    }
    return true;
}

// Hook: the start of the game's field of view step (func_1510B128): player 1's call (camera 0) is kept, while
// Photo Mode isn't open, to be made again while it is.
extern "C" void conker_photo_fov_args(uint8_t* rdram, recomp_context* ctx) {
    if (is_open || (int32_t)ctx->r4 != 0) {
        return;
    }
    fov_args[0] = ctx->r4; fov_args[1] = ctx->r5; fov_args[2] = ctx->r6; fov_args[3] = ctx->r7;
    fov_arg_stack = MEM_W(0x10, ctx->r29);
    fov_args_kept = true;
}

// Hook: the start of the HUD (func_1508FD38, $a0 the display list). True skips it (returning the list as it was).
extern "C" int conker_photo_hide_hud(uint8_t* rdram, recomp_context* ctx) {
    if (!is_open) {
        return false;
    }
    ctx->r2 = ctx->r4;
    return true;
}

// Hook: the start of an object's drawing (func_1502C974, $a0 the display list, $a1 the object's index). True
// skips it (returning the list as it was): Conker and what hangs off him, while he's hidden.
extern "C" int conker_photo_hide_object(uint8_t* rdram, recomp_context* ctx) {
    if (!is_open || conker_shown || !part_of_conker(rdram, (int)(int32_t)ctx->r5)) {
        return false;
    }
    ctx->r2 = ctx->r4;
    return true;
}

// The panel and the hints, drawn over the game by recompui (so never in the pictures), in the settings' style:
// gold names, pill-shaped values, and the player's own buttons or keys (the controller's or the keyboard's,
// following Button Prompts) in the hints.
namespace {
    namespace look = recompui::conker_look;

    bool ui_created = false;
    recompui::ContextId ui_context = recompui::ContextId::null();
    recompui::Element* ui_panel = nullptr;
    recompui::Label* ui_names[RowCount] = {};
    recompui::Label* ui_values[RowCount] = {};
    recompui::Element* ui_hints = nullptr;
    recompui::Element* ui_saved_box = nullptr;
    std::string ui_shown_values[RowCount];
    int ui_shown_selected = -1;
    std::string ui_shown_hints;
    bool ui_panel_visible = true, ui_saved_visible = false;

    recompui::Element* dark_box(recompui::Element* parent) {
        recompui::Element* box = ui_context.create_element<recompui::Element>(parent);
        box->set_background_color(recompui::Color{ 0x14, 0x0E, 0x0A, 0xD8 });
        box->set_border_width(3.0f);
        box->set_border_color(look::steel);
        box->set_border_radius(18.0f);
        return box;
    }

    // What one of the game's inputs is bound to (the first binding), as prompt-font text; empty if none.
    std::string bound(recompinput::GameInput input, bool keyboard) {
        const int profile = keyboard ? recompinput::profiles::get_sp_keyboard_profile_index()
                                     : recompinput::profiles::get_sp_controller_profile_index();
        for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
            const recompinput::InputField& field = recompinput::profiles::get_input_binding(profile, input, i);
            if (!field.is_empty()) {
                return field.to_string();
            }
        }
        return "";
    }

    // The hints: what to press (prompt-font glyphs, or a word for the mouse), then what it does; '|' between hints.
    std::string hints_text(bool keyboard) {
        using GI = recompinput::GameInput;
        auto keys = [&](std::initializer_list<GI> inputs) {
            std::string s;
            for (GI input : inputs) {
                const std::string b = bound(input, keyboard);
                if (!b.empty() && s.find(b) == std::string::npos) s += b;
            }
            return s;
        };
        const std::string turn = keyboard ? std::string("\x01Mouse") :
            recompinput::InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTX).to_string();
        const std::string move = keyboard ? keys({ GI::Y_AXIS_POS, GI::X_AXIS_NEG, GI::Y_AXIS_NEG, GI::X_AXIS_POS }) :
            keys({ GI::Y_AXIS_POS });
        std::string out;
        auto add = [&](const std::string& press, const char* what) {
            if (press.empty()) return;
            if (!out.empty()) out += '|';
            out += press + '\t' + what;
        };
        add(turn, "Turn");
        add(move, "Move");
        add(keys({ GI::L }) + "+" + move, "Up / Down");
        add(keys({ GI::Z, GI::R }) + (keyboard ? std::string("\x01Wheel") : std::string()), "Closer / Further");
        add(keys({ GI::DPAD_UP, GI::DPAD_DOWN }), "Choose");
        add(keys({ GI::DPAD_LEFT, GI::DPAD_RIGHT }), "Change");
        add(keys({ GI::A }), "Picture");
        add(keys({ GI::START }), "Hide Panel");
        add(keys({ GI::B }) + (keyboard ? std::string("\x01P") : std::string()), "Close");
        return out;
    }

    // Glyphs in the prompt font; a part after \x01 is a word (Mouse, Wheel, P) in the game's font.
    void add_press(recompui::Element* parent, const std::string& press) {
        size_t start = 0;
        while (start < press.size()) {
            const bool word = press[start] == '\x01';
            if (word) start++;
            size_t end = press.find('\x01', start);
            if (end == std::string::npos) end = press.size();
            const std::string part = press.substr(start, end - start);
            if (!part.empty()) {
                recompui::Label* label = ui_context.create_element<recompui::Label>(parent, part, recompui::LabelStyle::Small);
                if (word) {
                    look::pill(label, 15.0f);
                    label->set_color(look::brown_lit);
                }
                else {
                    label->set_font_family("promptfont");
                    label->set_font_weight(400);
                    label->set_font_size(26.0f);
                    label->set_color(look::brown_lit);
                }
            }
            start = end;
        }
    }

    void build_hints(const std::string& text) {
        ui_hints->clear_children();
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find('|', start);
            if (end == std::string::npos) end = text.size();
            const std::string hint = text.substr(start, end - start);
            const size_t tab = hint.find('\t');
            if (tab != std::string::npos) {
                recompui::Element* item = ui_context.create_element<recompui::Element>(ui_hints);
                item->set_display(recompui::Display::Flex);
                item->set_flex_direction(recompui::FlexDirection::Row);
                item->set_align_items(recompui::AlignItems::Center);
                item->set_gap(6.0f);
                add_press(item, hint.substr(0, tab));
                recompui::Label* what = ui_context.create_element<recompui::Label>(item, hint.substr(tab + 1), recompui::LabelStyle::Small);
                look::letters(what, 18.0f);
                what->add_class("conker-name");
                what->set_color(look::gold);
            }
            start = end + 1;
        }
    }

    void show_selected(int row, bool selected) {
        ui_names[row]->set_color(selected ? look::orange : look::gold);
        ui_values[row]->set_background_color(selected ? look::fill : recompui::Color{ 0, 0, 0, 0 });
        ui_values[row]->set_border_color(selected ? look::ring : look::steel);
        ui_values[row]->set_color(selected ? look::chosen_text : look::brown);
    }

    void create_ui() {
        ui_context = recompui::create_context();
        ui_context.open();
        ui_context.set_captures_input(false);
        ui_context.set_captures_mouse(false);
        recompui::Element* root = ui_context.get_root_element();

        ui_panel = dark_box(root);
        ui_panel->set_position(recompui::Position::Absolute);
        ui_panel->set_left(24.0f);
        ui_panel->set_top(24.0f);
        ui_panel->set_padding_left(22.0f);
        ui_panel->set_padding_right(22.0f);
        ui_panel->set_padding_top(10.0f);
        ui_panel->set_padding_bottom(16.0f);
        ui_panel->set_display(recompui::Display::Flex);
        ui_panel->set_flex_direction(recompui::FlexDirection::Column);
        ui_panel->set_gap(10.0f);
        recompui::Label* title = ui_context.create_element<recompui::Label>(ui_panel, "Photo Mode", recompui::LabelStyle::Normal);
        look::letters(title, 36.0f);
        title->add_class("conker-title");
        title->set_color(look::purple);
        title->set_margin_bottom(4.0f);
        for (int i = 0; i < RowCount; i++) {
            recompui::Element* row = ui_context.create_element<recompui::Element>(ui_panel);
            row->set_display(recompui::Display::Flex);
            row->set_flex_direction(recompui::FlexDirection::Row);
            row->set_align_items(recompui::AlignItems::Center);
            row->set_justify_content(recompui::JustifyContent::SpaceBetween);
            row->set_gap(24.0f);
            ui_names[i] = ui_context.create_element<recompui::Label>(row, row_names[i], recompui::LabelStyle::Normal);
            look::letters(ui_names[i], 22.0f);
            ui_names[i]->add_class("conker-name");
            ui_values[i] = ui_context.create_element<recompui::Label>(row, "", recompui::LabelStyle::Small);
            look::pill(ui_values[i], 17.0f);
            show_selected(i, false);
        }

        // The hints, along the bottom.
        recompui::Element* bottom = ui_context.create_element<recompui::Element>(root);
        bottom->set_position(recompui::Position::Absolute);
        bottom->set_left(0.0f);
        bottom->set_right(0.0f);
        bottom->set_bottom(20.0f);
        bottom->set_display(recompui::Display::Flex);
        bottom->set_justify_content(recompui::JustifyContent::Center);
        ui_hints = dark_box(bottom);
        ui_hints->set_display(recompui::Display::Flex);
        ui_hints->set_flex_direction(recompui::FlexDirection::Row);
        ui_hints->set_flex_wrap(recompui::FlexWrap::Wrap);
        ui_hints->set_justify_content(recompui::JustifyContent::Center);
        ui_hints->set_align_items(recompui::AlignItems::Center);
        ui_hints->set_gap(22.0f);
        ui_hints->set_max_width(92.0f, recompui::Unit::Percent);
        ui_hints->set_padding_left(20.0f);
        ui_hints->set_padding_right(20.0f);
        ui_hints->set_padding_top(8.0f);
        ui_hints->set_padding_bottom(8.0f);

        // "Picture saved", at the top.
        recompui::Element* top = ui_context.create_element<recompui::Element>(root);
        top->set_position(recompui::Position::Absolute);
        top->set_left(0.0f);
        top->set_right(0.0f);
        top->set_top(28.0f);
        top->set_display(recompui::Display::Flex);
        top->set_justify_content(recompui::JustifyContent::Center);
        recompui::Label* saved = ui_context.create_element<recompui::Label>(top, "Picture saved in the Photos folder", recompui::LabelStyle::Normal);
        look::pill(saved, 22.0f);
        saved->set_background_color(look::fill);
        saved->set_border_color(look::ring);
        saved->set_color(look::chosen_text);
        ui_saved_box = top;
        ui_saved_box->set_display(recompui::Display::None);
        ui_saved_visible = false;

        ui_context.close();
        ui_created = true;
    }
}

void conker::photo::on_ui_ready() {
    ui_ready = true;
}

// Main thread (update_gfx), each frame.
void conker::photo::update_ui() {
    if (!ui_ready || !active_flag) {
        if (ui_created && recompui::is_context_shown(ui_context)) {
            recompui::hide_context(ui_context);
        }
        return;
    }
    if (!ui_created) {
        create_ui();
    }
    Panel now;
    {
        std::lock_guard lock(panel_mutex);
        now = panel;
    }
    const bool saved = std::chrono::steady_clock::now().time_since_epoch().count() < saved_shown_until;
    const std::string hints = hints_text(conker::keyboard_prompts_in_use());
    ui_context.open();
    for (int i = 0; i < RowCount; i++) {
        if (now.values[i] != ui_shown_values[i]) {
            ui_values[i]->set_text(now.values[i]);
            ui_shown_values[i] = now.values[i];
        }
    }
    if (now.selected != ui_shown_selected) {
        if (ui_shown_selected >= 0 && ui_shown_selected < RowCount) show_selected(ui_shown_selected, false);
        show_selected(now.selected, true);
        ui_shown_selected = now.selected;
    }
    if (hints != ui_shown_hints) {
        build_hints(hints);
        ui_shown_hints = hints;
    }
    if (now.shown != ui_panel_visible) {
        ui_panel->set_display(now.shown ? recompui::Display::Flex : recompui::Display::None);
        ui_hints->set_display(now.shown ? recompui::Display::Flex : recompui::Display::None);
        ui_panel_visible = now.shown;
    }
    if (saved != ui_saved_visible) {
        ui_saved_box->set_display(saved ? recompui::Display::Flex : recompui::Display::None);
        ui_saved_visible = saved;
    }
    ui_context.close();
    if (!recompui::is_context_shown(ui_context)) {
        recompui::show_context(ui_context, "");
    }
}

// Testing: CONKER_TRACE_ANIM=1 prints each animation Conker is given and where from (the caller's return address).
extern "C" void conker_trace_set_anim(uint8_t* rdram, recomp_context* ctx) {
    static const bool on = std::getenv("CONKER_TRACE_ANIM") != nullptr;
    if (on && (uint32_t)ctx->r4 == player_object) {
        std::printf("[anim] %X from %08X (%.1f s)\n", (uint32_t)ctx->r5, (uint32_t)ctx->r31, conker::testing::game_seconds());
    }
}

// Testing: CONKER_TRACE_ANIM=1 also prints each new (function, object, attachment) the attachment code is given.
extern "C" void conker_trace_attach(uint8_t* rdram, recomp_context* ctx, int which) {
    static const bool on = std::getenv("CONKER_TRACE_ANIM") != nullptr;
    if (!on) return;
    static std::vector<uint64_t> seen;
    const uint64_t key = ((uint64_t)which << 56) | ((uint64_t)(uint32_t)ctx->r4 << 16) | ((uint32_t)ctx->r5 & 0xFFFF);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
    seen.push_back(key);
    std::printf("[attach] f%d obj %08X id %d a2 %08X a3 %08X (%.1f s)\n", which, (uint32_t)ctx->r4, (int)(int32_t)ctx->r5,
        (uint32_t)ctx->r6, (uint32_t)ctx->r7, conker::testing::game_seconds());
}
