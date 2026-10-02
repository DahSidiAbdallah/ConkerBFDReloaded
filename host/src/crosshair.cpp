// Aiming Crosshair (Conker tab): a red dot in the middle of the screen while Conker aims, so it's
// clear where a throw goes: in the game's second aiming mode (func_15126378: throwables, the
// magnum; look_aim.cpp's conker_aim_stick) without zoom (the sniper scope draws its own), and in the
// look mode (func_15120158, conker_look_targets) when the game put Conker in it to throw (the
// knives in the barn) rather than the player holding R to look around. Single player only.
//
// Like the FPS counter (fps_counter.cpp) it's a recompui context of its own that takes no input,
// drawn over the game: a red dot, made of a plain element. It's shown while the aiming
// mode ran in the last tenth of a second (the game runs at 30 frames a second).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>

#include "ultramodern/ultramodern.hpp"
#include "recompui/recompui.h"
#include "recompinput/players.h"
#include "elements/ui_element.h"

#include "conker.hpp"

namespace {
    constexpr uint16_t button_r = 0x0010, button_l = 0x0020; // R_TRIG, L_TRIG
    // How far above the centre the dot goes, as a part of the half height (0: the centre).
    std::atomic<float> dot_raise{0.0f};
    float shown_raise = -1.0f;
    recompui::Element* dot_element = nullptr;
    std::atomic<uint16_t> buttons_held{0};
    // When R or L was last held, and when the look mode started running without them (clock ticks).
    std::atomic<int64_t> look_button_at{0}, look_alone_since{0}, look_last_ran{0};
}

namespace {
    using clock = std::chrono::steady_clock;
    constexpr auto still_aiming = std::chrono::milliseconds(100);

    std::atomic<int64_t> aimed_at{0}; // when the aiming mode last ran, unzoomed (clock ticks)
    std::atomic<bool> ui_ready = false; // recompui's UI exists (made on the render thread as RT64 starts)
    bool created = false;
    recompui::ContextId context = recompui::ContextId::null();

    void create() {
        context = recompui::create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        recompui::Element* root = context.get_root_element();
        // A red dot, centred on the window (the game's picture is centred in it at any aspect ratio),
        // with a thin dark edge so it shows on red and bright scenery too.
        recompui::Element* dot = context.create_element<recompui::Element>(root);
        dot->set_position(recompui::Position::Absolute);
        dot->set_left(50.0f, recompui::Unit::Percent);
        dot->set_top(50.0f, recompui::Unit::Percent);
        dot->set_width(18.0f);
        dot->set_height(18.0f);
        dot->set_translate_2D(-50.0f, -50.0f, recompui::Unit::Percent);
        dot->set_border_width(2.0f);
        dot->set_border_radius(9.0f);
        dot->set_border_color({ 0x20, 0x00, 0x00, 0xC0 });
        dot->set_background_color({ 0xFF, 0x20, 0x20, 0xF0 });
        dot_element = dot;
        context.close();
        created = true;
    }
}


void conker::crosshair::aiming(bool zoomed) {
    dot_raise = 0.0f;
    if (!zoomed) {
        aimed_at = clock::now().time_since_epoch().count();
    }
}

void conker::crosshair::look_mode(float vertical_fov) {
    // Only when the game put Conker in the look mode to throw (the knives in the barn), not when the
    // player looks around with R or L: not while either is held, nor for a moment after (the camera
    // swings back for a few frames once let go), and only once the look mode has run that way for a
    // little while.
    const int64_t now = clock::now().time_since_epoch().count();
    const auto ticks = [](auto d) { return (int64_t)std::chrono::duration_cast<clock::duration>(d).count(); };
    if (now - look_last_ran.load() > ticks(std::chrono::milliseconds(200))) {
        look_alone_since = now; // (a new spell of the look mode)
    }
    look_last_ran = now;
    if ((buttons_held.load() & (button_r | button_l)) != 0 || now - look_button_at.load() < ticks(std::chrono::milliseconds(700))) {
        look_alone_since = now;
        return;
    }
    if (now - look_alone_since.load() >= ticks(std::chrono::milliseconds(250))) {
        aimed_at = now;
        // Throws from the look mode fly above the view's centre: measured on the knives in the barn
        // (the knife lands 0.41 of the way from the centre to the top, at the game's 50 degrees),
        // a fixed angle above it, so it's placed by the field of view in use.
        constexpr float throw_angle = 10.8f * 3.14159265f / 180.0f;
        float raise = 0.0f;
        if (vertical_fov > 1.0f && vertical_fov < 170.0f) {
            raise = std::tan(throw_angle) / std::tan(vertical_fov * 0.5f * 3.14159265f / 180.0f);
        }
        dot_raise = std::clamp(raise, 0.0f, 0.9f);
    }
}

void conker::crosshair::set_buttons(uint16_t buttons) {
    buttons_held = buttons;
    if ((buttons & (button_r | button_l)) != 0) {
        look_button_at = clock::now().time_since_epoch().count();
    }
}

void conker::crosshair::on_ui_ready() {
    ui_ready = true;
}

void conker::crosshair::update() {
    // Testing aid: CONKER_TEST_CROSSHAIR=1 shows it all the time (test_crosshair.flag, run_host.bat).
    static const bool test_always = std::getenv("CONKER_TEST_CROSSHAIR") != nullptr;
    if (test_always) {
        aimed_at = clock::now().time_since_epoch().count();
    }
    const auto last = clock::time_point(clock::duration(aimed_at.load()));
    // Single player only: in split screen the middle of the window isn't player 1's view.
    // Not while a cutscene or a talk plays: the game uses the look mode for some (Conker reading
    // "What To Do"), though nothing can be thrown then.
    const bool wanted = ui_ready && conker::crosshair::enabled() && ultramodern::is_game_started() &&
        recompinput::players::is_single_player_mode() && clock::now() - last < still_aiming &&
        !conker::qol::cutscene_playing();
    if (!wanted) {
        if (created && recompui::is_context_shown(context)) {
            recompui::hide_context(context);
        }
        return;
    }
    if (!created) {
        create();
    }
    if (!recompui::is_context_shown(context)) {
        recompui::show_context(context, "");
    }
    const float raise = dot_raise.load();
    if (raise != shown_raise && dot_element != nullptr) {
        context.open();
        dot_element->set_top(50.0f * (1.0f - raise), recompui::Unit::Percent);
        context.close();
        shown_raise = raise;
    }
}
