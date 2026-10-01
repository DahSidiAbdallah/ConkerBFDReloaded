// Show FPS (Graphics tab), from CBFD-Recompiled V0.1.5 (MIT, Sean Ciaschi).
// A small counter in the top-right corner while the game runs, to spot
// slowdowns. It shows two rates, each averaged over half a second:
// - FPS: frames RT64 presented to the screen (RT64::presentedFrameCount, added by patches/rt64_conker.patch),
//   interpolated ones included. A drop here is the PC falling behind.
// - Game: frames the game itself made (a display list started, conker_frame_dl_begin in
//   widescreen.cpp). Conker runs at up to 30; a drop here with FPS steady is the game's own
//   slowdown, as it would be on the N64.
//
// It's a recompui context of its own that takes no input and no mouse, so the game and the
// menus work as before, shown only once the game has started (the launcher comes back when
// nothing is shown before then). recompui's UI is made on the render thread as RT64 starts, and
// update_gfx can run before that (the game started from the command line): the context is only
// made once the launcher's init has run (on_ui_ready), or creating it read the UI before it existed.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "ultramodern/ultramodern.hpp"
#include "recompui/recompui.h"
#include "elements/ui_label.h"

#include "conker.hpp"

namespace RT64 {
    uint64_t presentedFrameCount();
}

namespace {
    using clock = std::chrono::steady_clock;
    constexpr auto interval = std::chrono::milliseconds(500);

    std::atomic<uint64_t> game_frames = 0;
    std::atomic<bool> ui_ready = false;

    bool created = false;
    recompui::ContextId context = recompui::ContextId::null();
    recompui::Label* label = nullptr;

    clock::time_point last_time;
    uint64_t last_presented = 0;
    uint64_t last_game = 0;

    void create() {
        context = recompui::create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        recompui::Element* box = context.create_element<recompui::Element>(context.get_root_element());
        box->set_position(recompui::Position::Absolute);
        box->set_top(8.0f);
        box->set_right(8.0f);
        box->set_padding_left(8.0f);
        box->set_padding_right(8.0f);
        box->set_padding_top(2.0f);
        box->set_padding_bottom(2.0f);
        box->set_border_radius(6.0f);
        box->set_background_color(recompui::theme::color::BGOverlay);
        label = context.create_element<recompui::Label>(box, "", recompui::LabelStyle::Small);
        context.close();
        created = true;
    }
}

void conker::fps_counter::game_frame() {
    game_frames.fetch_add(1, std::memory_order_relaxed);
}

void conker::fps_counter::on_ui_ready() {
    ui_ready = true;
}

void conker::fps_counter::update() {
    const bool wanted = ui_ready && conker::fps_counter::enabled() && ultramodern::is_game_started();
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
        last_time = clock::time_point{};
    }

    const clock::time_point now = clock::now();
    const uint64_t presented = RT64::presentedFrameCount();
    const uint64_t game = game_frames.load(std::memory_order_relaxed);
    if (last_time == clock::time_point{}) {
        last_time = now;
        last_presented = presented;
        last_game = game;
        return;
    }
    if (now - last_time < interval) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - last_time).count();
    char text[64];
    std::snprintf(text, sizeof(text), "%.0f FPS  |  Game %.0f", (presented - last_presented) / seconds,
        (game - last_game) / seconds);
    last_time = now;
    last_presented = presented;
    last_game = game;

    context.open();
    label->set_text(text);
    context.close();
}
