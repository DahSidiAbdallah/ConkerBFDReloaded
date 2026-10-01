// Per-scene widescreen fixes, called from hooks in recomp/conker.toml.
//
// The game skips drawing whatever lies outside its 4:3 view. For widescreen,
// widescreen.cpp (from CBFD-Recompiled) widens those checks so the sides of the
// picture are filled in. Some scenes, though, were staged for the 4:3 picture with
// things waiting just past its sides: in the opening's Nintendo logo scene (room
// 0x21) Conker, his hands and chainsaw wait off to the side, and the N logo is
// kicked off to land out of view. In those rooms the camera keeps the N64's exact
// side-to-side view: it zooms in evenly by as much as the window is wider than 4:3,
// so the picture fills the window, shows exactly what the N64 showed across, and
// loses a strip at the top and bottom. The game's own 4:3 checks for what to draw
// stay as they were, since nothing past the 4:3 sides is shown.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include <SDL.h>

#include "recomp.h"
#include "ultramodern/config.hpp"
#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"

extern SDL_Window* window; // frontend.cpp

extern "C" void conker_widen_frustum(uint8_t* rdram, recomp_context* ctx);
extern "C" void conker_widen_cull_scale(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr int32_t current_room = (int32_t)0x800BE9F0;
    constexpr uint32_t rooms_staged_for_4_3[] = { 0x21 };

    bool room_staged_for_4_3(uint8_t* rdram) {
        uint32_t room = (uint32_t)MEM_W(0, current_room);
        return std::find(std::begin(rooms_staged_for_4_3), std::end(rooms_staged_for_4_3), room) !=
            std::end(rooms_staged_for_4_3);
    }

    // How much wider than 4:3 the picture is (1 when the aspect ratio is Original).
    float widescreen_ratio() {
        if (ultramodern::renderer::get_graphics_config().ar_option == ultramodern::renderer::AspectRatio::Original ||
            window == nullptr) {
            return 1.0f;
        }
        int width = 0, height = 0;
        SDL_GetWindowSize(window, &width, &height);
        if (width <= 0 || height <= 0) {
            return 1.0f;
        }
        return std::max(1.0f, (float)width / (float)height / (4.0f / 3.0f));
    }
}

// func_1510B5F8 at 0x1510B64C, its call to Rare's perspective function (0x15047F00):
// $a2 and $a3 hold the vertical and horizontal fields of view in degrees (floats'
// bits). Both are narrowed alike, so the picture zooms evenly.
extern "C" void conker_scene_fov(uint8_t* rdram, recomp_context* ctx) {
    const float ratio = widescreen_ratio();
    if (ratio <= 1.0f || !room_staged_for_4_3(rdram)) {
        return;
    }
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    auto narrow = [ratio](gpr& reg) {
        uint32_t bits = (uint32_t)reg;
        float fov;
        std::memcpy(&fov, &bits, sizeof(fov));
        fov = 2.0f * std::atan(std::tan(fov * 0.5f * degrees_to_radians) / ratio) / degrees_to_radians;
        std::memcpy(&bits, &fov, sizeof(bits));
        reg = (int32_t)bits;
    };
    narrow(ctx->r6);
    narrow(ctx->r7);
}

// The zoom above keeps the middle of the N64's picture, cutting as much off its top as
// off its bottom. The scene's first shot (the N logo dropping under a lamp, then
// bouncing up and landing, until the screen goes black at about 4.3 s) has the lamp
// shade and the bouncing logo at the very top and bare floor at the bottom, so there
// the whole cut comes off the bottom instead. After the black screen (room timer 260)
// the cut is even again, which can't be seen happen.
// func_1510B5F8 at 0x1510B654, after the call: the camera's projection is a float
// matrix at the camera (0x800BE628's cameras, $sp+0x2C in) +0xBC, used as
// clip = v * m. Moving the picture down by dy (in -1..1 screen units) adds dy * w to
// clip y: m[i][1] += dy * m[i][3].
extern "C" void conker_probe_pause(uint8_t* rdram, recomp_context* ctx, int tag); // testing.cpp

extern "C" void conker_scene_projection(uint8_t* rdram, recomp_context* ctx) {
    conker_probe_pause(rdram, ctx, 4);
    constexpr int32_t room_timer = (int32_t)0x800E0A90;
    constexpr int32_t first_shot_end = 260;
    const float ratio = widescreen_ratio();
    if (ratio <= 1.0f || !room_staged_for_4_3(rdram) || MEM_W(0, room_timer) >= first_shot_end) {
        return;
    }
    const int32_t matrix = MEM_W(0, (int32_t)0x800BE628) + MEM_W(0x2C, ctx->r29) + 0xBC;
    // The zoomed picture shows 1/ratio of the N64's height; keeping its top edge moves
    // the picture down by ratio - 1.
    const float dy = -(ratio - 1.0f);
    auto get = [&](int i, int j) {
        uint32_t bits = (uint32_t)MEM_W((i * 4 + j) * 4, matrix);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    };
    for (int i = 0; i < 4; i++) {
        float value = get(i, 1) + dy * get(i, 3);
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        MEM_W((i * 4 + 1) * 4, matrix) = (int32_t)bits;
    }
}

extern "C" volatile int rt64_conker_open_scissor_border; // RT64 (rt64_rdp.cpp, patches/rt64_conker.patch)

// In place of widescreen.cpp's hooks of the same names (without "scene_"). The first also tells
// RT64 whether to open the 2 pixels Conker's scissors leave out at each side of the frame, which
// made thin black strips at the edges of a wide window (widescreen only; 4:3 stays as it was).
extern "C" void conker_scene_widen_frustum(uint8_t* rdram, recomp_context* ctx) {
    rt64_conker_open_scissor_border = widescreen_ratio() > 1.0f ? 1 : 0;
    static const bool test_original_culling = std::getenv("CONKER_TEST_43_CULLING") != nullptr; // testing
    if (!room_staged_for_4_3(rdram) && !test_original_culling) {
        conker_widen_frustum(rdram, ctx);
    }
}

extern "C" void conker_scene_widen_cull_scale(uint8_t* rdram, recomp_context* ctx) {
    // Camera: Field of View first ($v0 is the camera), in 4:3 too (field_of_view.cpp).
    conker::field_of_view::adjust_cull_scales(rdram, ctx->r2);
    static const bool test_original_culling = std::getenv("CONKER_TEST_43_CULLING") != nullptr; // testing
    if (!room_staged_for_4_3(rdram) && !test_original_culling) {
        conker_widen_cull_scale(rdram, ctx);
    }
}

// Skip Intro (Conker tab), behind a black screen until the save menu shows.
//
// The game boots through three rooms: 0x25 (the notices and the Nintendo logo), 0x21 (the
// chainsaw opening) and 0x1D (the bar, where Conker walks in before the menu; a button
// press skips that). A boot phase (0x800E0B94) picks what runs each frame, and each part
// asks for the next room itself through func_1501C730. As CBFD-Recompiled's Skip Intro mod
// does (Christopher Conley, MIT; mods/skip_intro there): once the logos have run 10
// frames, ask for the bar instead of the opening, and once the bar has loaded, leave
// behind what the opening's end would have (without that the bar is empty and Start does
// nothing). Ours then presses Start for the player, which opens the save menu, and shows
// the screen: the menu is up about a second after Start Game.
namespace {
    enum class Skip { Off, Logos, Bar, PressingStart, Done };
    std::atomic<Skip> skip_state{ Skip::Off };
    constexpr uint32_t legal_screens = 0x25, bar = 0x1D;
    constexpr int32_t room_timer = (int32_t)0x800E0A90; // refreshes since the room began
    constexpr int32_t logo_frames_before_skip = 10;     // earlier, the bar never loads (the mod's measurement)
    constexpr int32_t press_start_at = 20, show_menu_at = 60; // bar room timer

    void finish_skip() {
        skip_state = Skip::Done;
        ultramodern::set_screen_blanked(false);
    }
}

extern "C" void func_1501C730(uint8_t* rdram, recomp_context* ctx);

// func_15007A70(a, b, room)'s entry: the game changing rooms.
extern "C" void conker_room_change(uint8_t* rdram, recomp_context* ctx) {
    uint32_t room = (uint32_t)(ctx->r6 & 0xFFFF);
    static const bool probe = std::getenv("CONKER_PROBE") != nullptr;
    if (probe) {
        std::printf("[room] %02X -> %02X\n", (uint32_t)MEM_W(0, current_room), room);
    }
    Skip state = skip_state;
    if (state == Skip::Off && room == legal_screens && conker::skip_intro()) {
        skip_state = Skip::Logos;
        ultramodern::set_screen_blanked(true);
    } else if (state != Skip::Off && state != Skip::Done && room != legal_screens && room != bar) {
        finish_skip(); // somewhere unexpected: just show it
    }
}

// func_151DE6D4 (the logos' frame, boot phase 0) at its return: after 10 frames, ask for
// the bar in place of the opening, as the logos' own end asks for the opening.
extern "C" void conker_skip_intro_logos(uint8_t* rdram, recomp_context* ctx) {
    if (skip_state != Skip::Logos || MEM_W(0, room_timer) < logo_frames_before_skip) {
        return;
    }
    skip_state = Skip::Bar;
    MEM_B(0, (int32_t)0x8008FE28) = 2;
    MEM_B(0, (int32_t)0x800E0B94) = 1; // boot phase
    MEM_B(0, (int32_t)0x800D2E40) = 0;
    // func_1501C730(6, bar, 0, 0, 1), from a hook: registers saved and put back, the fifth
    // argument in this function's outgoing argument space.
    recomp_context saved = *ctx;
    int32_t saved_arg = MEM_W(0x10, ctx->r29);
    ctx->r4 = 6;
    ctx->r5 = (int32_t)bar;
    ctx->r6 = 0;
    ctx->r7 = 0;
    MEM_W(0x10, ctx->r29) = 1;
    func_1501C730(rdram, ctx);
    MEM_W(0x10, saved.r29) = saved_arg;
    *ctx = saved;
    MEM_B(0, (int32_t)0x800E0B96) = 0xFF;
}

// func_15007A70 at its return, the room loaded: in the bar, what the opening's end
// (func_151DE85C) would have left behind.
extern "C" void conker_skip_intro_room_loaded(uint8_t* rdram, recomp_context* ctx) {
    if (skip_state != Skip::Bar || (uint32_t)MEM_W(0, current_room) != bar || MEM_W(0, room_timer) > 5) {
        return;
    }
    MEM_B(0, (int32_t)0x800D2E40) = 0;
    MEM_B(0, (int32_t)0x800E0B94) = 3; // boot phase
    MEM_B(0, (int32_t)0x8008FD80) = 1;
    MEM_B(0, (int32_t)0x8008FE28) = 2;
    MEM_B(0, (int32_t)0x8008FDA4) = 0;
    int32_t game_state = MEM_W(0, (int32_t)0x8008FDD4);
    if (game_state != 0) {
        MEM_B(0x3E, game_state) = 0;
        MEM_B(0x2B, game_state) = 5;
        MEM_B(0x2C, game_state) = 5;
    }
}

// Every screen refresh: in the bar, press Start once it's running, then show the menu.
void conker::skip_intro_on_vi(uint8_t* rdram) {
    Skip state = skip_state;
    if ((state != Skip::Bar && state != Skip::PressingStart) || (uint32_t)MEM_W(0, current_room) != bar) {
        return;
    }
    int32_t timer = MEM_W(0, room_timer);
    if (state == Skip::Bar && timer >= press_start_at) {
        skip_state = Skip::PressingStart;
    } else if (state == Skip::PressingStart && timer >= show_menu_at) {
        finish_skip();
    }
}

bool conker::skip_intro_pressing_start() {
    return skip_state == Skip::PressingStart;
}
