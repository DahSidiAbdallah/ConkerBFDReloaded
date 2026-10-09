// Quality-of-life options (Conker tab):
//
// - Autosave Icon: Conker's head, pulsing and rocking, in the bottom-right corner while the game saves
//   (the save file in the settings folder changes; the runtime writes it as the game writes its
//   EEPROM, at checkpoints and on the save menu).
// - Pause When Unfocused: while the window doesn't have the keyboard (alt-tab, another window
//   clicked), the game holds still: the VI callback waits, so game time, sound and input stop.
// - Skip Any Cutscene: holding L skips any cutscene, even the first time (the game only lets watched
//   ones be skipped), including the ones it never lets you skip (the opening, the hangover scene), as
//   CBFD-Recompiled's Skip Any Cutscene mod allows (MIT). Held, not pressed, so it can't happen by
//   accident: "Hold to Skip" shows in the corner with a ring filling as L is held. A hook where
//   func_1501E05C (the game's "skip the playing cutscene?" check) returns, recomp/conker.toml.
// - Reduce Motion Effects: no motion blur (the drunk ghosting) and no drunken camera sway, for players
//   who get motion sick.
// - Always Show Health: health (chocolate) stays on screen instead of sliding away.
// - Longer Tail Spin: the spin after a jump (A twice) floats and glides for longer.
// - Toggle R-Look and Toggle Crouch: a press of R or Z holds it until the next press, for players
//   who can't hold a button down.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <thread>

#include <SDL.h>

extern SDL_Window* window; // frontend.cpp

#include "recomp.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "recompui/recompui.h"
#include "elements/ui_element.h"
#include "elements/ui_image.h"
#include "elements/ui_label.h"
#include "util/file.h"

#include "conker.hpp"

extern std::atomic_bool exited; // librecomp's recomp.cpp: set by ultramodern::quit

namespace {
    using clock = std::chrono::steady_clock;

    // Pause When Unfocused.
    std::atomic<bool> window_focused{ true };


    // Testing aid, only with a file named capture_enabled in the settings folder: F9 saves a snapshot
    // of the game's memory there (memory_<n>.rdram), to find things only a real moment of play shows
    // (e.g. the cash display's timer for Always Show HUD).
    std::atomic<int> capture_requests{ 0 };

    int SDLCALL watch_focus(void*, SDL_Event* event) {
        if (event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_SCANCODE_F9 && !event->key.repeat) {
            capture_requests++;
        }
        if (event->type == SDL_WINDOWEVENT) {
            if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                window_focused = true;
            } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                window_focused = false;
            }
        }
        return 1;
    }

    // Test runs play in the background, without the keyboard.
    bool test_run() {
        static const bool testing = std::getenv("CONKER_SNAP_AT") != nullptr || std::getenv("CONKER_INPUT_SCRIPT") != nullptr;
        return testing;
    }

    // Autosave Icon.
    constexpr auto icon_time = std::chrono::milliseconds(2000);
    constexpr auto check_every = std::chrono::milliseconds(250);
    std::atomic<bool> ui_ready = false;
    bool icon_created = false;
    recompui::ContextId icon_context = recompui::ContextId::null();
    clock::time_point last_check{}, shown_until{};
    std::filesystem::file_time_type last_save{};
    bool have_last_save = false;

    // The newest save file's time (the runtime keeps them in the settings folder's saves).
    bool newest_save(std::filesystem::file_time_type& time) {
        std::error_code ec;
        const std::filesystem::path folder = recomp::get_config_path() / "saves";
        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".bin") {
                continue;
            }
            const auto t = entry.last_write_time(ec);
            if (!ec && (!found || t > time)) {
                time = t;
                found = true;
            }
        }
        return found;
    }

    recompui::Image* icon_image = nullptr;

    void create_icon() {
        std::ifstream file(recompui::file::get_asset_path("save_icon.png"), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        icon_context = recompui::create_context();
        icon_context.open();
        icon_context.set_captures_input(false);
        icon_context.set_captures_mouse(false);
        if (!bytes.empty()) {
            recompui::queue_image_from_bytes_file("?/conker/save_icon", bytes);
            icon_image = icon_context.create_element<recompui::Image>(icon_context.get_root_element(), "?/conker/save_icon");
            icon_image->set_position(recompui::Position::Absolute);
            icon_image->set_right(30.0f);
            icon_image->set_bottom(30.0f);
            icon_image->set_width(64.0f);
            icon_image->set_height(64.0f);
        }
        icon_context.close();
        icon_created = true;
    }

    // Skip Any Cutscene's hold.
    constexpr uint16_t button_l = 0x0020, button_start = 0x1000;
    constexpr double hold_seconds = 1.2;
    std::atomic<uint16_t> player_buttons{ 0 };     // player 1's own buttons (before the toggles)
    std::atomic<int64_t> cutscene_seen{ 0 };       // the check last ran (a cutscene is playing)
    std::atomic<int64_t> hold_started{ 0 };        // 0: not holding
    std::atomic<bool> wait_for_release{ false };   // after a skip, until L is let go
    std::atomic<float> hold_progress{ 0.0f };

    int64_t ticks() { return clock::now().time_since_epoch().count(); }
    double seconds_since(int64_t t) { return std::chrono::duration<double>(clock::now() - clock::time_point(clock::duration(t))).count(); }

    constexpr int ring_dots = 24;
    constexpr float ring_radius = 28.0f, ring_right = 66.0f, ring_bottom = 66.0f, dot_size = 8.0f;
    bool skip_created = false;
    recompui::ContextId skip_context = recompui::ContextId::null();
    recompui::Element* dots[ring_dots] = {};
    int lit_shown = -1;
    const recompui::Color dot_off{ 0xFF, 0xF4, 0xD6, 0x50 }, dot_on{ 0xF0, 0x7A, 0x1E, 0xFF };

    void create_skip() {
        skip_context = recompui::create_context();
        skip_context.open();
        skip_context.set_captures_input(false);
        skip_context.set_captures_mouse(false);
        recompui::Element* root = skip_context.get_root_element();
        for (int k = 0; k < ring_dots; k++) {
            const float a = (-90.0f + k * 360.0f / ring_dots) * 3.14159265f / 180.0f; // from the top, clockwise
            recompui::Element* d = skip_context.create_element<recompui::Element>(root);
            d->set_position(recompui::Position::Absolute);
            d->set_right(ring_right - ring_radius * std::cos(a) - dot_size / 2);
            d->set_bottom(ring_bottom - ring_radius * std::sin(a) - dot_size / 2);
            d->set_width(dot_size);
            d->set_height(dot_size);
            d->set_border_radius(dot_size / 2);
            d->set_background_color(dot_off);
            dots[k] = d;
        }
        recompui::Label* button = skip_context.create_element<recompui::Label>(root, "L", recompui::LabelStyle::Large);
        button->set_position(recompui::Position::Absolute);
        button->set_right(ring_right - 9.0f);
        button->set_bottom(ring_bottom - 16.0f);
        button->set_color({ 0xFF, 0xF4, 0xD6, 0xFF });
        recompui::Label* text = skip_context.create_element<recompui::Label>(root, "Hold to Skip", recompui::LabelStyle::Large);
        text->set_position(recompui::Position::Absolute);
        text->set_right(ring_right + ring_radius + 18.0f);
        text->set_bottom(ring_bottom - 16.0f);
        text->set_color({ 0xFF, 0xE9, 0xC2, 0xFF });
        skip_context.close();
        skip_created = true;
    }

    void update_skip() {
        const bool cutscene = seconds_since(cutscene_seen.load()) < 0.25;
        const bool holding = (player_buttons.load() & button_l) != 0 && !wait_for_release.load();
        const bool wanted = ui_ready && conker::qol::skip_any_cutscene() && cutscene && holding;
        if (!wanted) {
            if (skip_created && recompui::is_context_shown(skip_context)) {
                recompui::hide_context(skip_context);
            }
            return;
        }
        if (!skip_created) {
            create_skip();
        }
        if (!recompui::is_context_shown(skip_context)) {
            recompui::show_context(skip_context, "");
            lit_shown = -1;
        }
        const int lit = std::clamp((int)std::lround(hold_progress.load() * ring_dots), 0, ring_dots);
        if (lit != lit_shown) {
            skip_context.open();
            for (int k = 0; k < ring_dots; k++) {
                dots[k]->set_background_color(k < lit ? dot_on : dot_off);
            }
            skip_context.close();
            lit_shown = lit;
        }
    }

    // Air Meter (Accessibility): a bar under the game's air face (top right, drawn in the 4:3 picture's
    // space: its middle at x 276, its bottom at y 115 of 320 x 240; measured), emptying as Conker's
    // time underwater (+0xB2) goes from 0 to 1561, where he starts losing health. Shown only with the face
    // (and after an area's opening circle), fading in and out with it. Out of air, each hit takes 60
    // off the count (back to 1501), so from 1561 on it stays empty until he's had air again.
    std::atomic<uint16_t> air_used{ 0 };
    std::atomic<bool> air_paused{ false };
    std::atomic<uint8_t> air_face{ 0 };
    std::atomic<int32_t> air_face_drawn{ -1 }; // the area's timer when func_150911F4 last drew the face
    std::atomic<int32_t> air_area_timer{ 0 };
    std::atomic<bool> drowning{ false }; // the game's drowning scene (animations 0x28, 0x24): the screen closes in
    constexpr int32_t area_opening = 190;
    float meter_fade = 0.0f; // fading in once wanted
    float meter_opacity_shown = -1.0f;
    constexpr float air_limit = 1561.0f;
    constexpr float meter_x = 276.0f, meter_y = 121.0f, meter_w = 78.0f, meter_h = 8.0f; // N64 units
    bool meter_created = false;
    recompui::ContextId meter_context = recompui::ContextId::null();
    recompui::Element* meter_frame = nullptr;
    recompui::Element* meter_fill = nullptr;
    float meter_shown = -1.0f;
    float meter_aspect = 0.0f;
    float meter_smooth = 1.0f;
    bool out_of_air = false;
    clock::time_point meter_last{};

    recompui::Color meter_colour(float air) {
        // green (full) -> yellow -> orange -> red (empty)
        auto mix = [](recompui::Color a, recompui::Color b, float t) {
            auto m = [t](uint8_t x, uint8_t y) { return (uint8_t)std::lround(x + (y - x) * t); };
            return recompui::Color{ m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 0xFF };
        };
        const recompui::Color green{ 0x6E, 0xD2, 0x4A, 0xFF }, yellow{ 0xE6, 0xD2, 0x3C, 0xFF },
            orange{ 0xF0, 0x96, 0x2D, 0xFF }, red{ 0xE6, 0x3C, 0x32, 0xFF };
        if (air > 0.66f) return mix(yellow, green, (air - 0.66f) / 0.34f);
        if (air > 0.33f) return mix(orange, yellow, (air - 0.33f) / 0.33f);
        return mix(red, orange, air / 0.33f);
    }

    void place_meter(float aspect) {
        // In the window, the game's 4:3 picture is centred and as tall as the window.
        const float sx = 100.0f / (aspect * 240.0f); // % of width per N64 unit (the picture is 4:3, as tall as the window)
        const float sy = 100.0f / 240.0f;                                              // % of height per N64 unit
        meter_frame->set_left(50.0f + (meter_x - meter_w / 2 - 160.0f) * sx, recompui::Unit::Percent);
        meter_frame->set_top(meter_y * sy, recompui::Unit::Percent);
        meter_frame->set_width(meter_w * sx, recompui::Unit::Percent);
        meter_frame->set_height(meter_h * sy, recompui::Unit::Percent);
    }

    void create_meter() {
        meter_context = recompui::create_context();
        meter_context.open();
        meter_context.set_captures_input(false);
        meter_context.set_captures_mouse(false);
        recompui::Element* root = meter_context.get_root_element();
        meter_frame = meter_context.create_element<recompui::Element>(root);
        meter_frame->set_position(recompui::Position::Absolute);
        meter_frame->set_background_color({ 0x00, 0x00, 0x00, 0x8C });
        meter_frame->set_border_width(2.0f);
        meter_frame->set_border_color({ 0xFF, 0xFF, 0xFF, 0xE6 });
        meter_frame->set_border_radius(100.0f);
        meter_fill = meter_context.create_element<recompui::Element>(meter_frame);
        meter_fill->set_position(recompui::Position::Absolute);
        meter_fill->set_left(0.0f);
        meter_fill->set_top(0.0f);
        meter_fill->set_height(100.0f, recompui::Unit::Percent);
        meter_fill->set_width(100.0f, recompui::Unit::Percent);
        meter_fill->set_border_radius(100.0f);
        meter_context.close();
        meter_created = true;
    }

    void update_meter() {
        const uint16_t used = air_used.load();
        const bool cutscene = seconds_since(cutscene_seen.load()) < 0.25;
        const uint8_t face = air_face.load();
        // Drawn within the last ~third of a second of play (the area's timer stops with the game, and
        // starts over in a new area).
        const int32_t since_drawn = air_area_timer.load() - air_face_drawn.load();
        const bool drawn = air_face_drawn.load() >= 0 && since_drawn > -20 && since_drawn < 20; // (read a frame apart: either way)
        const bool wanted = ui_ready && conker::qol::air_meter() && face != 0 && drawn && air_area_timer.load() >= area_opening &&
            !air_paused.load() && !cutscene && !drowning.load() && ultramodern::is_game_started();
        if (!wanted) {
            if (meter_created && recompui::is_context_shown(meter_context)) {
                recompui::hide_context(meter_context);
            }
            meter_smooth = 1.0f;
            meter_fade = 0.0f;
            if (air_used.load() < 1501) {
                out_of_air = false;
            }
            return;
        }
        if (!meter_created) {
            create_meter();
        }
        if (!recompui::is_context_shown(meter_context)) {
            recompui::show_context(meter_context, "");
            meter_shown = -1.0f;
            meter_aspect = 0.0f;
            meter_opacity_shown = -1.0f;
            meter_last = clock::now();
        }
        // Follow the air smoothly (the counter steps a frame at a time).
        if (used >= air_limit) {
            out_of_air = true;
        } else if (used < 1501) {
            out_of_air = false;
        }
        const float air = out_of_air ? 0.0f : std::clamp(1.0f - used / air_limit, 0.0f, 1.0f);
        const clock::time_point now = clock::now();
        const float dt = std::min(0.1f, std::chrono::duration<float>(now - meter_last).count());
        meter_last = now;
        meter_smooth += (air - meter_smooth) * (1.0f - std::exp(-dt / 0.15f));
        if (air == 0.0f && meter_smooth < 0.01f) {
            meter_smooth = 0.0f; // all the way empty, not a sliver
        }
        meter_fade = std::min(1.0f, meter_fade + dt / 0.3f);
        int w = 0, h = 0;
        if (window != nullptr) SDL_GetWindowSize(window, &w, &h);
        const float aspect = (w > 0 && h > 0) ? (float)w / (float)h : 16.0f / 9.0f;
        const float opacity = std::round(face / 255.0f * meter_fade * 50.0f) / 50.0f;
        if (std::abs(meter_smooth - meter_shown) > 0.002f || aspect != meter_aspect || opacity != meter_opacity_shown) {
            meter_context.open();
            if (opacity != meter_opacity_shown) {
                meter_frame->set_opacity(opacity);
                meter_opacity_shown = opacity;
            }
            if (aspect != meter_aspect) {
                place_meter(aspect);
                meter_aspect = aspect;
            }
            meter_fill->set_width(std::max(0.0f, meter_smooth) * 100.0f, recompui::Unit::Percent);
            meter_fill->set_background_color(meter_colour(meter_smooth));
            meter_context.close();
            meter_shown = meter_smooth;
        }
    }

    // Toggle R-Look / Toggle Crouch.
    struct Toggle {
        bool was_down = false;
        bool held = false;
        uint16_t apply(uint16_t buttons, uint16_t button, bool enabled) {
            const bool down = (buttons & button) != 0;
            if (!enabled) {
                held = false;
                was_down = down;
                return buttons;
            }
            if (down && !was_down) {
                held = !held;
            }
            was_down = down;
            return held ? (buttons | button) : (buttons & (uint16_t)~button);
        }
    };
    Toggle toggle_r, toggle_z;
}

void conker::qol::init() {
    SDL_AddEventWatch(watch_focus, nullptr);
}

void conker::qol::on_ui_ready() {
    ui_ready = true;
}

void conker::qol::on_vi() {
    if (!conker::qol::pause_unfocused() || test_run()) {
        return;
    }
    // Hold the game still until the window has the keyboard again, or the game is closing (else
    // quitting while unfocused, e.g. from the taskbar, would wait here forever).
    while (!window_focused.load() && !exited.load() && ultramodern::is_game_started() && conker::qol::pause_unfocused()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void conker::qol::update() {
    update_skip();
    update_meter();
    const clock::time_point now = clock::now();
    if (now - last_check >= check_every) {
        last_check = now;
        // Button Prompts: the key pictures follow the bindings (key_prompts.cpp).
        static int prompt_checks = 0;
        if (prompt_checks++ % 4 == 0) {
            conker::key_prompts_update();
            static const char* pictures = std::getenv("CONKER_PROMPT_PICTURES"); // testing: the pictures as files
            static bool written = false;
            if (pictures != nullptr && !written) {
                written = true;
                conker::key_prompts_write_pictures(std::filesystem::u8path(pictures));
            }
        }
        std::filesystem::file_time_type t{};
        if (newest_save(t)) {
            if (have_last_save && t != last_save && ultramodern::is_game_started()) {
                shown_until = now + icon_time;
            }
            last_save = t;
            have_last_save = true;
        }
    }
    const bool wanted = ui_ready && conker::qol::saving_icon() && now < shown_until;
    if (!wanted) {
        if (icon_created && recompui::is_context_shown(icon_context)) {
            recompui::hide_context(icon_context);
        }
        return;
    }
    if (!icon_created) {
        create_icon();
    }
    if (!recompui::is_context_shown(icon_context)) {
        recompui::show_context(icon_context, "");
    }
    // Pulsing and rocking a little.
    if (icon_image != nullptr) {
        const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
        icon_context.open();
        icon_image->set_opacity((float)(0.85 + 0.15 * std::sin(seconds * 2.0 * 3.14159265 * 1.5)));
        icon_image->set_rotation((float)(14.0 * std::sin(seconds * 2.0 * 3.14159265 * 0.9)));
        icon_context.close();
    }
}

void conker::qol::set_player_buttons(uint16_t buttons) {
    player_buttons = buttons;
}

uint16_t conker::qol::apply_toggles(uint16_t buttons) {
    constexpr uint16_t button_r = 0x0010, button_z = 0x2000;
    buttons = toggle_r.apply(buttons, button_r, conker::qol::toggle_r_look());
    buttons = toggle_z.apply(buttons, button_z, conker::qol::toggle_crouch());
    return buttons;
}

// Walk Button (Accessibility): while L is held (or after a press, with Toggle), the stick is turned
// down to a gentle push, so the game itself plays Conker's walk (animation 0x01, about a fifth of
// his run's speed; from about half a push the game runs). Steering is the stick's; pushing less goes
// slower still. L does nothing in play (it's only Skip Any Cutscene's hold, in cutscenes); Toggle
// L toggles on a short tap, so holding L to skip never does.
namespace {
    constexpr float walk_stick = 0.3f;
    constexpr double walk_tap_seconds = 0.3; // Toggle L: only a tap this short toggles, on release
    bool walk_toggled = false;
    bool walk_l_was_held = false;
    int64_t walk_l_pressed = 0;     // when L went down
    bool walk_l_in_cutscene = false; // a cutscene played while L was down (Skip Any Cutscene's hold)
    // Only on foot: standing on the ground (flags + 0x100 top byte 0x01) in one of his on-foot
    // animations. Swimming (0x27, also "on the ground") and the air (jumps, the tail spin) keep the
    // whole stick. Set each VI from his object (on_vi_memory), read on the input thread.
    std::atomic<bool> on_foot{ false };
    // Invert Swimming: swimming underwater, where the stick steers like a plane (up dives). Underwater
    // is where the game itself counts his air (below): func_1504B0FC adds to it (Longer Breath's hook) only
    // while he's under, carrying something (a cog) or not; on the surface it doesn't, and his
    // underwater byte (+0xAD) stays set there after a dive, so it can't tell. The area timer (60 a
    // second) when the air was last counted; set with on_foot.
    std::atomic<int32_t> air_counted_at{ -1000 };
    std::atomic<int32_t> air_counted_since{ 0 }; // when the counting last started (after a break)
    bool surface_swimming_animation(uint16_t animation) {
        switch (animation) {
            case 0x22: case 0x27: case 0x6C: // swimming on the surface
            case 0xCC:                       // treading water
            case 0x8B:                       // wading
                return true;
            default:
                return false;
        }
    }
    std::atomic<bool> underwater{ false };
    bool on_foot_animation(uint16_t animation) {
        switch (animation) {
            case 0x0F: case 0x49: // standing
            case 0x01: case 0x02: // walking, running
            case 0x21: case 0x36: // turning, landing
            case 0x7A:            // pressed against a wall
            case 0x64: case 0x65: case 0x66: // on a beam: walking, balancing
                return true;
            default:
                return false;
        }
    }
}

static float real_at(uint8_t* rdram, int32_t address) {
    const int32_t bits = MEM_W(0, (gpr)address);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

static void update_on_foot(uint8_t* rdram) {
    constexpr int32_t player = (int32_t)0x800CC2D0;
    const uint8_t flags = (uint8_t)((uint32_t)MEM_W(0, (gpr)(player + 0x100)) >> 24);
    const uint16_t animation = (uint16_t)((uint32_t)MEM_W(0, (gpr)(player + 0x84)) >> 16);
    on_foot = flags == 0x01 && on_foot_animation(animation);
    // Air Meter: Conker's time underwater (+0xB2, see Longer Breath), while the game isn't paused.
    air_used = (uint16_t)MEM_HU(0, (gpr)(player + 0xB2));
    air_paused = MEM_BU(0, (gpr)(int32_t)0x800BEAC0) != 0;
    // The air face's own visibility (D_800D24C8 + 0xBA: 0 hidden, up to 255 as it fades in; func_150911F4
    // only draws it when it isn't 0): the bar comes, fades and goes with it.
    air_face = (uint8_t)MEM_BU(0, (gpr)(int32_t)(0x800D24C8 + 0xBA));
    // The area's timer (refreshes since it started): its opening circle covers the first ~180.
    air_area_timer = MEM_W(0, (gpr)(int32_t)0x800E0A90);
    // Out of air for good, the game plays his drowning (0x28, then 0x24) and closes the screen down to a
    // circle around him, hiding the face: the bar goes with it.
    drowning = animation == 0x28 || animation == 0x24;
    // Under: his air counted within the last few game frames (it's counted once a frame, at 30 a
    // second), not in a surface swimming animation, not walking along the bottom, and either well
    // below the water's surface (+0x118; swimming on it his middle is about 41 below, bobbing to 45)
    // or counting for over half a second: on the surface the game counts air for the moment a bob
    // or a stroke dips him under, and those mustn't flip the stick back and forth.
    const int32_t now_timer = air_area_timer.load();
    const int32_t since_counted = now_timer - air_counted_at.load();
    const bool counting = since_counted >= 0 && since_counted <= 8;
    const bool deep = real_at(rdram, player + 0x18) < real_at(rdram, player + 0x118) - 55.0f;
    const bool a_while = now_timer - air_counted_since.load() >= 30;
    underwater = counting && (deep || a_while) && !surface_swimming_animation(animation) && !on_foot.load();
    static const bool swim_debug = std::getenv("CONKER_SWIM_DEBUG") != nullptr; // testing: when it changes
    static bool was_underwater = false;
    if (swim_debug && underwater.load() != was_underwater) {
        was_underwater = underwater.load();
        std::printf("[swim] %.2fs %s: anim %02X y %.0f water %.0f air %u\n", conker::testing::game_seconds(), was_underwater ? "underwater" : "not underwater",
            animation, real_at(rdram, player + 0x18), real_at(rdram, player + 0x118), (unsigned)air_used.load());
    }
}

// Longer Breath: func_1504B0FC adds the frame's step (D_800BE9E4) to Conker's time underwater
// (+0xB2) at 0x1504B4E8 ($t4 + $t5); at 721 he gasps and his face starts turning purple, at 1501
// he gasps again, from 1561 he loses health each second (about 43 seconds in, measured). The step
// is divided here, with the remainder carried over, so all of it comes later.
extern "C" void conker_longer_breath(uint8_t* rdram, recomp_context* ctx) {
    constexpr int32_t player = (int32_t)0x800CC2D0;
    if ((int32_t)ctx->r16 != player) {
        return;
    }
    const int32_t timer = MEM_W(0, (gpr)(int32_t)0x800E0A90); // he's underwater (Invert Swimming)
    const int32_t gap = timer - air_counted_at.load();
    if (gap < 0 || gap > 8) {
        air_counted_since = timer;
    }
    air_counted_at = timer;
    const int mode = conker::qol::longer_breath();
    if (mode == 0) {
        return;
    }
    static double carried = 0.0;
    const double divisor = mode == 1 ? 2.0 : 4.0;
    carried += (double)(int32_t)ctx->r13 / divisor;
    const int32_t step = (int32_t)carried;
    carried -= step;
    ctx->r13 = step;
}

// func_150911F4 (draws the air face), past its "hidden" check (0x1509122C): it's on screen this frame.
extern "C" void conker_air_face_drawn(uint8_t* rdram, recomp_context* ctx) {
    air_face_drawn = MEM_W(0, (gpr)(int32_t)0x800E0A90);
}

void conker::qol::apply_swim(float* y) {
    if (conker::qol::invert_swimming() && underwater.load()) {
        *y = -*y;
    }
}

void conker::qol::apply_walk(uint16_t buttons, float* x, float* y) {
    const int mode = conker::qol::walk_button(); // 0 off, 1 hold L, 2 toggle L
    const bool l_held = (buttons & button_l) != 0;
    // A tap toggles when L is let go; a hold never does (holding L to skip a cutscene, even one
    // that starts or ends during the hold).
    if (l_held && !walk_l_was_held) {
        walk_l_pressed = ticks();
        walk_l_in_cutscene = false;
    }
    if (l_held && conker::qol::cutscene_playing()) {
        walk_l_in_cutscene = true;
    }
    if (mode == 2 && !l_held && walk_l_was_held && !walk_l_in_cutscene &&
        !conker::qol::cutscene_playing() && seconds_since(walk_l_pressed) < walk_tap_seconds) {
        walk_toggled = !walk_toggled;
    }
    walk_l_was_held = l_held;
    if (mode != 2) {
        walk_toggled = false;
    }
    const bool walking = (mode == 1) ? l_held : walk_toggled;
    if (walking && on_foot.load()) {
        // Pushed diagonally, a controller reports up to about 1.4 (both axes at their ends), which
        // turned down was still enough for the game to run: the push is capped at a full one first.
        const float push = std::sqrt(*x * *x + *y * *y);
        const float scale = walk_stick / std::max(1.0f, push);
        *x *= scale;
        *y *= scale;
    }
}

// Skip Any Cutscene: where func_1501E05C returns ($v0: skip the playing cutscene now). It runs each
// frame while a cutscene plays. Holding L for hold_seconds skips; a skip the game decided by itself
// (not from a button: the bar's walk-in after Skip Intro) is kept; a press alone does nothing.
extern "C" void conker_skip_cutscene_result(uint8_t* rdram, recomp_context* ctx) {
    cutscene_seen = ticks(); // (the cash counter hides during cutscenes too, cash_hud.cpp)
    if (!conker::qol::skip_any_cutscene()) {
        return;
    }
    const uint16_t pressed = (uint16_t)MEM_HU(0, (gpr)(int32_t)0x800BE710); // this frame's new presses
    const bool by_itself = ctx->r2 != 0 && (pressed & (button_l | button_start)) == 0;
    const bool held = (player_buttons.load() & button_l) != 0;
    if (!held) {
        hold_started = 0;
        wait_for_release = false;
        hold_progress = 0.0f;
    } else if (!wait_for_release && hold_started.load() == 0) {
        hold_started = ticks();
    }
    const int64_t started = hold_started.load();
    const float progress = started != 0 ? (float)std::min(1.0, seconds_since(started) / hold_seconds) : 0.0f;
    hold_progress = progress;
    if (progress >= 1.0f) {
        hold_started = 0;
        wait_for_release = true; // not the next cutscene too
        hold_progress = 0.0f;
        ctx->r2 = 1;
        return;
    }
    ctx->r2 = by_itself ? 1 : 0;
}

bool conker::qol::cutscene_playing() {
    return seconds_since(cutscene_seen.load()) < 0.25;
}

// Reduce Motion Effects: func_151D6778 draws the motion blur (Conker drunk at the start of the game:
// the last frames left as ghosts over the picture) when its strength, D_800BE574 ($v1 at 0x151D6798),
// isn't 0. Taken as 0 there, it draws none; the game's own value is left as it is.
extern "C" void conker_motion_blur(uint8_t* rdram, recomp_context* ctx) {
    if (conker::qol::reduce_motion()) {
        ctx->r3 = 0;
    }
}

// Reduce Motion Effects: the camera's sway. func_1512D070 sways the camera's look-at point slowly from
// side to side (Conker drunk: the camera's +0x5F0 has 0x8 set; also a couple of other camera modes),
// easing its strength (+0x7DC) toward the one it picks, $f2 here (0 when there's none). Taken as 0,
// the sway eases out and stays out; the game's own state is left as it is.
extern "C" void conker_camera_sway(uint8_t* rdram, recomp_context* ctx) {
    if (conker::qol::reduce_motion()) {
        ctx->f2.u32l = 0;
    }
}

// Always Show Health: the chocolate (health) display slides in when health changes and away again when
// its timer, D_800D2444 (frames, set to 240 by func_1508F0D4 on a change), runs out. Kept topped up,
// it stays on screen.
void conker::qol::on_vi_memory(uint8_t* rdram) {
    if (rdram == nullptr) {
        return;
    }
    update_on_foot(rdram);
    if (capture_requests.load() > 0) {
        capture_requests--;
        static const bool enabled = std::filesystem::exists(recomp::get_config_path() / "capture_enabled");
        static int count = 0;
        if (enabled) {
            const std::filesystem::path path = recomp::get_config_path() / ("memory_" + std::to_string(++count) + ".rdram");
            if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
                std::fwrite(rdram, 1, 8 * 1024 * 1024, f);
                std::fclose(f);
            }
        }
    }
    conker::qol::longer_spin_on_vi(rdram);
    if (!conker::qol::always_show_hud()) {
        return;
    }
    MEM_W(0, (gpr)(int32_t)0x800D2444) = 240;
}


// Longer Tail Spin: pressing A again in a jump makes Conker spin his tail: his upward speed (his
// object's + 0x20, D_800CC2D0 is player 1's) is set to 7 and his gravity (+ 0x24) to 1.0, which stays
// until he lands (a jump's is 6.2, standing 5.0). He floats up and glides down. With the option on,
// while he spins (gravity exactly 1.0 or ours, in the air) gravity is lower and he falls more slowly,
// so the spin lasts longer. Set once per VI: the game keeps the value until he lands.
namespace {
    constexpr uint32_t player_object = 0x800CC2D0;
    constexpr float spin_gravity = 1.0f, longer_gravity = 0.55f, longer_fall_speed = -6.0f;
}

void conker::qol::longer_spin_on_vi(uint8_t* rdram) {
    if (!conker::qol::longer_spin()) {
        return;
    }
    const gpr object = (gpr)(int32_t)player_object;
    auto read = [&](int offset) { uint32_t w = (uint32_t)MEM_W(offset, object); float f; std::memcpy(&f, &w, 4); return f; };
    auto write = [&](int offset, float f) { uint32_t w; std::memcpy(&w, &f, 4); MEM_W(offset, object) = (int32_t)w; };
    const float gravity = read(0x24), height = read(0x28);
    if (height <= 0.0f || (gravity != spin_gravity && gravity != longer_gravity)) {
        return;
    }
    write(0x24, longer_gravity);
    if (read(0x20) < longer_fall_speed) {
        write(0x20, longer_fall_speed);
    }
}
