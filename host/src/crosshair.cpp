// Aiming Crosshair (Conker tab): a red dot in the middle of the screen while Conker aims, so it's
// clear where a shot or a throw goes. Single player only.
//
// Overhauled with what CBFD-Recompiled V0.1.6's reticle (MIT) does better: it shows only for what's
// known to aim something, by the look mode's kind (crosshair::look_mode: the shotgun) and the second aiming
// mode (func_15126378; look_aim.cpp's conker_aim_stick) without zoom (the sniper scope draws its own).
// Only where it's exact, for shots that fly straight to the middle: thrown things (the barn's knives, the
// slingshot) fly in an arc that drops more the further they go, so no one spot on the screen is right at
// every distance (raised for a middle distance, it was a little off nearer and further: a player found it
// finicky and asked for it gone there). It's hidden during cutscenes and while a speech or thought bubble is up.
//
// Like the FPS counter (fps_counter.cpp) it's a recompui context of its own that takes no input,
// drawn over the game: a red dot, made of a plain element. It's shown while the aiming
// mode ran in the last tenth of a second (the game runs at 30 frames a second).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"
#include "recompui/recompui.h"
#include "recompinput/players.h"
#include "elements/ui_element.h"

#include "conker.hpp"

namespace {
    constexpr uint16_t button_r = 0x0010, button_l = 0x0020; // R_TRIG, L_TRIG
    std::atomic<uint16_t> buttons_held{0};
    std::atomic<int64_t> look_button_at{0}; // when R or L was last held (clock ticks)
}

namespace {
    using clock = std::chrono::steady_clock;
    constexpr auto still_aiming = std::chrono::milliseconds(100);

    std::atomic<int64_t> aimed_at{0}; // when the aiming mode last ran, unzoomed (clock ticks)
    std::atomic<int64_t> bubble_at{0}; // when a speech or thought bubble's text was last drawn
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
        context.close();
        created = true;
    }
}


namespace {
    // Testing (CONKER_TEST_TRACK): which way the dot was asked for, at most twice a second.
    void track(const char* how) {
        static const bool on = std::getenv("CONKER_TEST_TRACK") != nullptr;
        static int64_t last = 0;
        const int64_t now = clock::now().time_since_epoch().count();
        if (on && now - last > (int64_t)std::chrono::duration_cast<clock::duration>(std::chrono::milliseconds(500)).count()) {
            last = now;
            std::printf("[crosshair] %s\n", how);
        }
    }
}

void conker::crosshair::aiming(bool zoomed) {
    track(zoomed ? "aiming, zoomed" : "aiming");
    if (!zoomed) {
        aimed_at = clock::now().time_since_epoch().count();
    }
}

void conker::crosshair::look_mode(uint8_t kind) {
    {
        char how[32];
        std::snprintf(how, sizeof how, "look mode runs, kind %02X", kind);
        track(how);
    }
    // Shown only for the look mode's kinds that aim a straight shot (the camera's +0x23E, which the look mode picks
    // its settings by), as CBFD-Recompiled V0.1.6's reticle does: the rest (plain R-Look 0x00, hypnotising the raptor,
    // reading "What To Do"...) aren't aiming anything, and the thrown things (the barn's knives 0x0A, the slingshot
    // 0x02) arc (see the top).
    if (kind != 0x3B) { // the shotgun (Spooky), aimed holding R
        return;
    }
    aimed_at = clock::now().time_since_epoch().count();
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
    // Not while a cutscene plays, nor while a speech or thought bubble is up: the game uses the look
    // mode for some of those (Conker reading "What To Do", whose pages wait for a button and aren't
    // cutscenes), though nothing can be thrown then.
    const bool bubble = clock::now() - clock::time_point(clock::duration(bubble_at.load())) < std::chrono::milliseconds(250);
    const bool wanted = ui_ready && conker::crosshair::enabled() && ultramodern::is_game_started() &&
        recompinput::players::is_single_player_mode() && clock::now() - last < still_aiming &&
        !conker::qol::cutscene_playing() && !bubble;
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
}

// func_150417AC starting: a speech or thought bubble's text is being drawn this frame.
extern "C" void conker_bubble_text_drawn(uint8_t*, recomp_context*) {
    bubble_at = clock::now().time_since_epoch().count();
}
