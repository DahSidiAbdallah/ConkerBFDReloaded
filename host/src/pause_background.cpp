// Ours: the pause menu's blurred background in widescreen.
//
// When the game pauses, it keeps the last frame it drew (func_150198FC copies it from RDRAM
// into its own picture, then blurs that in place over the next frames, func_151D61B0) and
// draws it behind the menu as tiles (func_151D5E90). That frame in RDRAM is RT64's native
// 320x240 picture, which only holds the 4:3 view, so the widened sides aren't in it.
//
// The game reads Start a frame or two before it draws the frame it keeps. So on a new Start
// press, RT64 is asked (rt64_conker_native_aspect_scale, patches/rt64_conker.patch) to write
// the next frames to RDRAM with the whole widened view squeezed into the 320 pixels. When the
// pause then copies one of them, its tiles are stretched across the window
// (widescreen.cpp) and the picture has its proportions back. If no pause comes, RT64 goes
// back to the normal native picture after a few frames.

#include <algorithm>
#include <atomic>
#include <cstdint>

#include <SDL.h>

#include "recomp.h"
#include "ultramodern/config.hpp"

extern SDL_Window* window; // frontend.cpp

extern "C" volatile float rt64_conker_native_aspect_scale; // RT64, rt64_state.cpp
extern "C" volatile int rt64_conker_open_scissor_border; // RT64, rt64_rdp.cpp
extern "C" void conker_probe_pause(uint8_t* rdram, recomp_context* ctx, int tag); // testing.cpp

namespace {
    // How much wider than 4:3 RT64 draws the 3D view (1 with the aspect ratio set to Original).
    float widescreen_ratio() {
        if (ultramodern::renderer::get_graphics_config().ar_option == ultramodern::renderer::AspectRatio::Original || window == nullptr) {
            return 1.0f;
        }
        int width = 0, height = 0;
        SDL_GetWindowSize(window, &width, &height);
        if (width <= 0 || height <= 0) {
            return 1.0f;
        }
        return std::max(1.0f, (float)width / (float)height / (4.0f / 3.0f));
    }

    constexpr int32_t pause_flag = (int32_t)0x800BEAC0;    // byte: paused
    constexpr int32_t pause_counter = (int32_t)0x80082F90; // byte: 0 = copies the last frame next, 1-5 = blurs it
    constexpr int wide_calls = 12; // func_1501878C calls (two a frame) the squeezed picture waits for a pause

    std::atomic<bool> paused{false};
    std::atomic<bool> wide_asked{false};
    std::atomic<int> wide_calls_left{0};
    std::atomic<bool> kept_frame_wide{false};

    void stop_wide() {
        wide_asked = false;
        rt64_conker_native_aspect_scale = 0.0f;
    }
}

// A new press of Start on the first controller (frontend.cpp).
void conker_pause_background_start_pressed() {
    const float ratio = widescreen_ratio();
    if (paused || ratio <= 1.0f) {
        return;
    }
    wide_calls_left = wide_calls;
    wide_asked = true;
    rt64_conker_native_aspect_scale = ratio;
}

// Each start of func_1501878C (testing.cpp's hook).
extern "C" void conker_pause_background_tick(uint8_t* rdram) {
    paused = MEM_BU(0, pause_flag) != 0;
    if (!paused) {
        kept_frame_wide = false;
    }
    if (wide_asked && --wide_calls_left <= 0 && !paused) {
        stop_wide();
    }
}

// The start of func_150198FC, which draws the pause background.
extern "C" void conker_pause_background(uint8_t* rdram, recomp_context* ctx) {
    conker_probe_pause(rdram, ctx, 1);
    if (MEM_BU(0, pause_flag) == 0) {
        return;
    }
    if (MEM_BU(0, pause_counter) == 0) {
        // This frame copies the last one: squeezed if RT64 was asked before it was drawn.
        kept_frame_wide = wide_asked.load();
    }
    else if (wide_asked) {
        stop_wide();
    }
}

// The start of func_151D61B0, which blurs a picture in place ($a0), as the pause does to the frame
// it kept, once in each of the four frames after the copy.
extern "C" void conker_pause_background_blur(uint8_t* rdram, recomp_context* ctx) {
    conker_probe_pause(rdram, ctx, 3);
    const gpr picture = ctx->r4;
    // Only while the blank columns are there: RT64 draws them when it opens the scissors' border
    // (rt64_conker_open_scissor_border, scene_fixes.cpp).
    if (!kept_frame_wide || rt64_conker_open_scissor_border || (uint32_t)picture != (uint32_t)MEM_W(0, (int32_t)0x800BE9C4)) {
        return;
    }
    const int32_t width = MEM_W(0, (int32_t)0x800BE620), height = MEM_W(0, (int32_t)0x800BE624); // D_800BE620/4
    constexpr int32_t blank = 2;
    if (width <= 4 * blank || height <= 0 || width > 640 || height > 480) {
        return;
    }
    for (int32_t y = 0; y < height; y++) {
        const gpr row = picture + y * width * 2; // 16-bit pixels
        const int16_t left = MEM_H(blank * 2, row), right = MEM_H((width - 1 - blank) * 2, row);
        for (int32_t x = 0; x < blank; x++) {
            MEM_H(x * 2, row) = left;
            MEM_H((width - 1 - x) * 2, row) = right;
        }
    }
}

namespace {
    constexpr uint32_t rt64_hook = (0xE0u << 24) | 0x525464; // RT64's hook on F3DEX2's no-op
    constexpr uint32_t rt64_enable_extended = (0x1u << 28) | 0x64;
    constexpr uint32_t g_ex_setrectaspect = (0x64u << 24) | 0x000033;
    constexpr uint32_t g_ex_aspect_auto = 0x0, g_ex_aspect_stretch = 0x1, g_ex_aspect_zoom = 0x3;
    // Our display lists: past the game's 8 MB, where RT64 still reads (it masks addresses to 16
    // MB), after widescreen.cpp's ring (0x00F00000, 64 KB). A ring: RT64 has long finished with one
    // by the time it comes round.
    constexpr uint32_t redraw_ring_start = 0x00F10000, redraw_ring_size = 0x10000;
    uint32_t redraw_ring_offset = 0;
    // Tiles drawn of the picture, and loaded with a 1-pixel border: (62 + 2) x (28 + 2) 16-bit texels
    // fill 3840 of texture memory's 4096 bytes.
    constexpr int32_t tile_width = 62, tile_height = 28;

    void put(uint8_t* rdram, gpr& dl, uint32_t w0, uint32_t w1) {
        MEM_W(0, dl) = (int32_t)w0;
        MEM_W(4, dl) = (int32_t)w1;
        dl += 8;
    }
}

// widescreen.cpp's func_151D5E90/func_151D6418 hook, with the display list the function wrote:
// returns true when it was replaced by ours: the pause's kept frame, stretched across the window
// when it was squeezed, zoomed to fill it when it wasn't (as widescreen.cpp does), as it is in 4:3.
extern "C" bool conker_pause_background_redraw(uint8_t* rdram, gpr start, gpr end) {
    const uint32_t picture = (uint32_t)MEM_W(0, (int32_t)0x800BE9C4);
    if (picture == 0) {
        return false;
    }
    const uint32_t aspect = kept_frame_wide ? g_ex_aspect_stretch : (widescreen_ratio() > 1.0f ? g_ex_aspect_zoom : g_ex_aspect_auto);
    // The game's list: its setup (syncs, combiner, other modes...), then per tile a G_SETTIMG of the
    // picture, tile setup, load and a G_TEXRECT at the tile's own place (the picture drawn 1:1 at 0,0).
    gpr settimg = 0, othermode = 0;
    for (gpr cmd = start; cmd < end; cmd += 8) {
        const uint32_t op = (uint32_t)MEM_W(0, cmd) >> 24;
        if (op == 0xFD) { settimg = cmd; break; }
        if (op == 0xEF) { othermode = cmd; }
        if (op == 0xE4 || op == 0xF4) { return false; }
    }
    if (settimg == 0 || othermode == 0) {
        return false;
    }
    const uint32_t timg_w0 = (uint32_t)MEM_W(0, settimg), timg_w1 = (uint32_t)MEM_W(4, settimg);
    const int32_t width = (int32_t)(timg_w0 & 0xFFF) + 1;
    const int32_t height = MEM_W(0, (int32_t)0x800BE624); // D_800BE624
    const bool rgba16 = ((timg_w0 >> 19) & 0x3) == 2 && ((timg_w0 >> 21) & 0x7) == 0;
    if ((timg_w1 & 0x00FFFFFF) != (picture & 0x00FFFFFF) || !rgba16 || width != MEM_W(0, (int32_t)0x800BE620) || height <= 0 || height > 480) {
        return false;
    }

    const int32_t columns = (width + tile_width - 1) / tile_width, rows = (height + tile_height - 1) / tile_height;
    const uint32_t setup = (uint32_t)(settimg - start) / 8 + 1;
    const uint32_t needed = (4 + setup + (uint32_t)(columns * rows) * 10 + 2) * 8;
    if (needed > redraw_ring_size) {
        return false;
    }
    if (redraw_ring_offset + needed > redraw_ring_size) {
        redraw_ring_offset = 0;
    }
    const uint32_t ours = redraw_ring_start + redraw_ring_offset;
    redraw_ring_offset += (needed + 15) & ~15u;
    gpr dl = (gpr)(int32_t)(0x80000000u | ours);

    put(rdram, dl, rt64_hook, rt64_enable_extended);
    put(rdram, dl, g_ex_setrectaspect, aspect);
    // The game's setup up to its G_SETTIMG, smoothed (G_TF_BILERP) instead of point sampled.
    for (gpr cmd = start; cmd <= settimg; cmd += 8) {
        uint32_t w0 = (uint32_t)MEM_W(0, cmd);
        if (cmd == othermode) {
            w0 = (w0 & ~(0x3u << 12)) | (0x2u << 12);
        }
        put(rdram, dl, w0, (uint32_t)MEM_W(4, cmd));
    }
    for (int32_t row = 0; row < rows; row++) {
        for (int32_t column = 0; column < columns; column++) {
            const int32_t x = column * tile_width, y = row * tile_height;
            const int32_t w = std::min(tile_width, width - x), h = std::min(tile_height, height - y);
            // The texels loaded: the tile and a pixel round it, within the picture.
            const int32_t x0 = std::max(x - 1, 0), y0 = std::max(y - 1, 0);
            const int32_t x1 = std::min(x + w, width - 1), y1 = std::min(y + h, height - 1);
            const uint32_t line = (uint32_t)(((x1 - x0 + 1) * 2 + 7) / 8); // 64-bit words a row
            const uint32_t tile_format = (0u << 21) | (2u << 19) | (line << 9); // RGBA 16-bit
            const uint32_t clamp = (0x2u << 18) | (0x2u << 8); // cmt, cms: clamp
            put(rdram, dl, 0xF5000000 | tile_format, (7u << 24) | clamp);
            put(rdram, dl, 0xE6000000, 0);
            put(rdram, dl, 0xF4000000 | ((uint32_t)(x0 * 4) << 12) | (uint32_t)(y0 * 4), (7u << 24) | ((uint32_t)(x1 * 4) << 12) | (uint32_t)(y1 * 4));
            put(rdram, dl, 0xE7000000, 0);
            put(rdram, dl, 0xF5000000 | tile_format, (0u << 24) | clamp);
            put(rdram, dl, 0xF2000000 | ((uint32_t)(x0 * 4) << 12) | (uint32_t)(y0 * 4), (0u << 24) | ((uint32_t)(x1 * 4) << 12) | (uint32_t)(y1 * 4));
            // The rectangle at the tile's place; its first texel is (x, y), s10.5 in the loaded area.
            put(rdram, dl, 0xE4000000 | ((uint32_t)((x + w) * 4) << 12) | (uint32_t)((y + h) * 4), (0u << 24) | ((uint32_t)(x * 4) << 12) | (uint32_t)(y * 4));
            put(rdram, dl, 0xE1000000, ((uint32_t)(x * 32) << 16) | (uint32_t)(y * 32));
            put(rdram, dl, 0xF1000000, (0x400u << 16) | 0x400u);
            put(rdram, dl, 0xE7000000, 0);
        }
    }
    put(rdram, dl, g_ex_setrectaspect, g_ex_aspect_auto);
    put(rdram, dl, 0xDE010000, (uint32_t)end & 0x00FFFFFF); // on to what follows the game's list

    // The game's list now starts by branching to ours.
    gpr game_dl = start;
    put(rdram, game_dl, 0xDE010000, ours);
    return true;
}

// The rect aspect for the tiles of a saved frame (widescreen.cpp): stretched when the
// pause kept a squeezed frame, zoomed otherwise.
extern "C" bool conker_pause_background_stretch() {
    return kept_frame_wide;
}
