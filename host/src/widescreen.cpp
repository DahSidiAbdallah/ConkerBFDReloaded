// From CBFD-Recompiled (Copyright (c) 2026 Sean Ciaschi, MIT License; see
// recomp/THIRD_PARTY_LICENSE), used unchanged except for this note, the pause
// background's stretch and redraw (conker_pause_background_stretch/_redraw, ours) and the
// pause probe.
// Widescreen fixes, called from hooks in conker.toml.
//
// RT64 widens the 3D view, but 2D texture rectangles are drawn in the N64's
// 320-wide screen space, and the G_TEXRECT command can't hold coordinates left
// of 0. So the game culls and clips its screen-space sprites (bubbles, bees,
// sparkles: func_15130A9C) to the 4:3 screen. These hooks let those sprites
// reach into the widened area: the cull bounds are pushed outwards, and the
// rectangle is emitted as RT64's extended texture rectangle, which takes signed
// coordinates, in the same display list space as the game's own rectangle.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <SDL.h>

#include "recomp.h"
#include "ultramodern/config.hpp"
#include "conker.hpp"

// The game window (frontend.cpp).
extern SDL_Window* window;

namespace {
    // RT64's extended GBI (tools/rt64/include/rt64_extended_gbi.h) for F3DEX2,
    // whose no-op (0xE0) carries RT64's hooks.
    constexpr uint32_t rt64_hook_opcode = 0xE0;
    constexpr uint32_t rt64_hook_magic = 0x525464;
    constexpr uint32_t rt64_hook_op_enable = 0x1;
    constexpr uint32_t rt64_extended_opcode = 0x64;
    constexpr uint32_t g_ex_texrect_v1 = 0x000002;
    constexpr uint32_t g_ex_origin_none = 0x800;

    float stack_float(uint8_t* rdram, gpr sp, int32_t offset) {
        uint32_t word = (uint32_t)MEM_W(offset, sp);
        float value;
        std::memcpy(&value, &word, sizeof(value));
        return value;
    }

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

    // How much wider than the game's 4:3 the window shows: RT64 widens the 3D
    // view to the window's aspect ratio unless the aspect ratio is set to Original.
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

    // TEMP-DEBUG: CONKER_CULL_EXTRA widens the frustum and cull scale this much more than
    // the window, to tell whether a missing piece is theirs.
    float cull_extra() {
        static const float extra = [] {
            const char* value = SDL_getenv("CONKER_CULL_EXTRA");
            return value != nullptr ? std::max(1.0f, (float)std::atof(value)) : 1.0f;
        }();
        return extra;
    }

    // TEMP-DEBUG: CONKER_NO_CULL_WIDEN leaves the frustum and cull scale at 4:3.
    bool no_cull_widen() {
        static const bool off = SDL_getenv("CONKER_NO_CULL_WIDEN") != nullptr;
        return off;
    }

    // How far past each 4:3 edge sprites are kept, in N64 screen pixels: as far as
    // the window reaches past it (half the frame's width, D_800BE620, for each 4:3
    // width more), and a little more. A fixed 160 fell short on 32:9 screens.
    float sprite_cull_margin(uint8_t* rdram) {
        const float half_frame = (float)MEM_W(0, (gpr)(int32_t)0x800BE620) * 0.5f;
        return half_frame * (widescreen_ratio() - 1.0f) + 8.0f;
    }

    void put_command(uint8_t* rdram, gpr& dl, uint32_t w0, uint32_t w1) {
        MEM_W(0, dl) = (int32_t)w0;
        MEM_W(4, dl) = (int32_t)w1;
        dl += 8;
    }
}

// func_15130A9C at 0x15130CEC / 0x15130D08: $f6 and $f10 hold the camera's left
// and right sprite bounds (camera + 0x2C / + 0x30), about to be compared with the
// sprite's right and left edges.
extern "C" void conker_widen_sprite_cull_left(uint8_t* rdram, recomp_context* ctx) {
    ctx->f6.fl -= sprite_cull_margin(rdram);
}

extern "C" void conker_widen_sprite_cull_right(uint8_t* rdram, recomp_context* ctx) {
    ctx->f10.fl += sprite_cull_margin(rdram);
}

// func_15130A9C at 0x15130DA0: the game has just written a G_RDPPIPESYNC at $v0,
// the first command of the sprite. RT64 doesn't need syncs; put the enable of its
// extended GBI there instead (RT64 turns it off at the start of every display
// list), so the extended rectangle below costs no extra display list space. The
// sprites are drawn into display lists allocated to fit what the game writes.
extern "C" void conker_enable_extended_gbi(uint8_t* rdram, recomp_context* ctx) {
    gpr dl = ctx->r2;
    put_command(rdram, dl, (rt64_hook_opcode << 24) | rt64_hook_magic, (rt64_hook_op_enable << 28) | rt64_extended_opcode);
}

// func_15130A9C at 0x15131168: $v0 is where the sprite's G_TEXRECT goes (three
// commands: the rectangle and its two G_RDPHALF words). The game clamps its corners
// to 0 and moves the texture start instead; emit the unclamped rectangle as an
// extended one, which is also three commands. RT64 (rt64.patch) clips a rectangle
// whose scissor spans the frame at the edges of the widened frame. The function
// then continues at its end (L_15131360), which returns sp + 0x100.
extern "C" void conker_emit_sprite_texrect(uint8_t* rdram, recomp_context* ctx) {
    gpr sp = ctx->r29;
    // Corners in 10.2 fixed point, already scaled by 4.
    int32_t ulx = (int32_t)stack_float(rdram, sp, 0xBC);
    int32_t uly = (int32_t)stack_float(rdram, sp, 0xB8);
    int32_t lrx = (int32_t)stack_float(rdram, sp, 0xB4);
    int32_t lry = (int32_t)stack_float(rdram, sp, 0xB0);
    // Texture start (s10.5) and steps (s5.10), including the flips' adjustments.
    uint32_t s = (uint32_t)MEM_W(0x98, sp) & 0xFFFF;
    uint32_t t = (uint32_t)MEM_W(0x94, sp) & 0xFFFF;
    uint32_t dsdx = (uint32_t)MEM_W(0x90, sp) & 0xFFFF;
    uint32_t dtdy = (uint32_t)MEM_W(0x8C, sp) & 0xFFFF;
    const uint32_t tile = 0;

    gpr dl = ctx->r2;
    put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_texrect_v1,
        tile | (g_ex_origin_none << 3) | (g_ex_origin_none << 15));
    put_command(rdram, dl, ((uint32_t)(ulx & 0xFFFF) << 16) | (uint32_t)(uly & 0xFFFF),
        ((uint32_t)(lrx & 0xFFFF) << 16) | (uint32_t)(lry & 0xFFFF));
    put_command(rdram, dl, (s << 16) | t, (dsdx << 16) | dtdy);
    MEM_W(0x100, sp) = (int32_t)dl;
}

// Speech bubbles (issue #59). func_15095D34 draws each piece of a cutscene's speech bubble (its
// halves, mirrored, and its tail) as a G_TEXRECT: the left edge from $f12 (D_800D2C78, truncated
// at 0x15095E78), the right edge that plus a width (its 5th argument and D_800D2C84 less one:
// $f12 is 1.0 by then, 0x15095F18), the texture start from $f12's fraction. Like the sprites, a piece past the
// screen's left edge is clamped to 0 and its texture moved on, so a bubble near the edge was cut at
// the 4:3 edge in widescreen, the widened picture beside it.
//
// In widescreen a piece whose left edge is near or past the screen's is drawn that many whole
// pixels to the right (its left edge, which the right one follows; whole pixels, so the texture
// start is the same), which the game doesn't clamp, and its rectangle is then moved back as RT64's
// extended one, whose coordinates can be negative. The game writes the corners in 12 bits, so on
// very wide screens a piece far to the right has its right edge wrapped past 1024 pixels; that one
// is unwrapped and rewritten the same way.
//
// The extended GBI has to be enabled in the frame before that, and the bubbles write no sync to
// put the enable in. RT64 keeps it on from the enable to the end of the frame, so it's enabled
// early, in place of the pipe sync each camera's pass starts with; the bubbles are only moved in
// frames where that happened.
namespace {
    constexpr float bubble_margin = 8.0f; // pixels kept right of the screen's left edge
    constexpr uint32_t bubble_texrect = 0xE4; // G_TEXRECT
    bool extended_enabled = false;
    gpr bubble_dl_start = 0;
    int32_t bubble_shift = 0;
}

// func_1501878C, as it starts the frame's display list.
extern "C" void conker_frame_dl_begin(uint8_t* rdram, recomp_context* ctx) {
    extended_enabled = false;
    conker::fps_counter::game_frame();
}

// func_15019464, just after func_1501A490 wrote a pipe sync and the camera's scissor ($v0 after them).
extern "C" void conker_camera_pass_sync(uint8_t* rdram, recomp_context* ctx) {
    gpr sync = ctx->r2 - 16;
    if ((uint32_t)MEM_W(0, sync) != 0xE7000000 || MEM_W(4, sync) != 0) {
        return;
    }
    put_command(rdram, sync, (rt64_hook_opcode << 24) | rt64_hook_magic, (rt64_hook_op_enable << 28) | rt64_extended_opcode);
    extended_enabled = true;
}

// func_15095D34, after $f12 (the piece's left edge) is loaded; its display list ($a0) is at $sp + 0x88.
extern "C" void conker_bubble_begin(uint8_t* rdram, recomp_context* ctx) {
    bubble_dl_start = 0;
    bubble_shift = 0;
    if (!extended_enabled || widescreen_ratio() <= 1.0f) {
        return;
    }
    const gpr sp = ctx->r29;
    bubble_dl_start = (gpr)MEM_W(0x88, sp);
    if (ctx->f12.fl < bubble_margin) {
        bubble_shift = (int32_t)std::ceil(bubble_margin - ctx->f12.fl);
        ctx->f12.fl += (float)bubble_shift;
    }
}

// At its return: $v0 is the end of what it wrote.
extern "C" void conker_bubble_end(uint8_t* rdram, recomp_context* ctx) {
    const gpr start = bubble_dl_start;
    const int32_t shift = bubble_shift;
    bubble_dl_start = 0;
    bubble_shift = 0;
    if (start == 0) {
        return;
    }
    const gpr end = ctx->r2;
    for (gpr cmd = start; cmd + 24 <= end && end - start <= 0x40; cmd += 8) {
        const uint32_t w0 = (uint32_t)MEM_W(0, cmd);
        if ((w0 >> 24) != bubble_texrect || ((uint32_t)MEM_W(8, cmd) >> 24) != 0xE1 || ((uint32_t)MEM_W(16, cmd) >> 24) != 0xF1) {
            continue;
        }
        const uint32_t w1 = (uint32_t)MEM_W(4, cmd);
        int32_t lrx = (int32_t)((w0 >> 12) & 0xFFF), lry = (int32_t)(w0 & 0xFFF);
        int32_t ulx = (int32_t)((w1 >> 12) & 0xFFF), uly = (int32_t)(w1 & 0xFFF);
        const uint32_t tile = (w1 >> 24) & 0x7;
        if (lrx < ulx) {
            lrx += 0x1000; // past the 12 bits
        } else if (shift == 0) {
            break; // drawn as the game wrote it
        }
        ulx -= shift * 4;
        lrx -= shift * 4;
        const uint32_t st = (uint32_t)MEM_W(12, cmd), steps = (uint32_t)MEM_W(20, cmd);
        gpr dl = cmd;
        put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_texrect_v1, tile | (g_ex_origin_none << 3) | (g_ex_origin_none << 15));
        put_command(rdram, dl, ((uint32_t)(ulx & 0xFFFF) << 16) | (uint32_t)(uly & 0xFFFF), ((uint32_t)(lrx & 0xFFFF) << 16) | (uint32_t)(lry & 0xFFFF));
        put_command(rdram, dl, st, steps);
        break;
    }
}

// func_151D5E90 (and func_151D6418) draw a saved copy of the frame, such as the
// pause menu's blurred background, as 42 textured tiles. The copy only holds the
// 4:3 frame, so RT64 draws the tiles in the 4:3 area and the widened sides show
// the game frozen behind them. Scale the tiles up to the whole width instead,
// and as much vertically, so the frame keeps its proportions and loses some of
// its top and bottom, with RT64's rect aspect (zoom, from rt64.patch). The tiles
// are full of load and pipe syncs, which RT64 doesn't need: before the first
// rectangle, the first sync becomes the enable of RT64's extended GBI and the
// second the zoom; the last sync, after the last rectangle, returns to the
// automatic aspect. The display list doesn't grow.
//
// Not for the motion blur (func_151D6778, e.g. Conker drunk at the start of the game), which
// uses both every frame on its own copy of the frame (D_800BE570): it draws the copy over the
// frame, then copies the frame into it. That copy is RT64's widened frame and is drawn back
// across the whole width already, and zooming it each frame zoomed the last frame's ghost
// again on every pass, dragging the picture into streaks.
namespace {
    constexpr uint32_t motion_blur_copy = 0x800BE570; // D_800BE570
    constexpr uint32_t g_ex_setrectaspect_v1 = 0x000033;
    constexpr uint32_t g_ex_aspect_auto = 0x0;
    constexpr uint32_t g_ex_aspect_stretch = 0x1;
    constexpr uint32_t g_ex_aspect_zoom = 0x3;
    constexpr uint32_t g_texrect = 0xE4;

    gpr frame_copy_dl_start = 0;

    bool is_sync(uint8_t* rdram, gpr cmd) {
        uint32_t w0 = (uint32_t)MEM_W(0, cmd);
        return (w0 == 0xE6000000 || w0 == 0xE7000000 || w0 == 0xE8000000) && MEM_W(4, cmd) == 0;
    }
}

// At the start of the function: $a0 is where it writes its first command, $a1 the image it
// draws from.
extern "C" void conker_probe_pause(uint8_t* rdram, recomp_context* ctx, int tag); // testing.cpp
extern "C" bool conker_pause_background_stretch(); // pause_background.cpp (ours)
extern "C" bool conker_pause_background_redraw(uint8_t* rdram, gpr start, gpr end); // pause_background.cpp (ours)

extern "C" void conker_frame_copy_begin(uint8_t* rdram, recomp_context* ctx) {
    conker_probe_pause(rdram, ctx, 2);
    frame_copy_dl_start = ctx->r4;
    const uint32_t blur_copy = (uint32_t)MEM_W(0, (gpr)(int32_t)motion_blur_copy);
    if (blur_copy == 0) {
        return;
    }
    // The motion blur draws its copy (func_151D6418 from D_800BE570), or copies the frame into
    // it (func_151D5E90 just after its colour image was set to D_800BE570).
    const bool draws_copy = (uint32_t)ctx->r5 == blur_copy;
    const bool fills_copy = ((uint32_t)MEM_W(-16, ctx->r4) >> 24) == 0xFF && (uint32_t)MEM_W(-12, ctx->r4) == blur_copy;
    if (draws_copy || fills_copy) {
        frame_copy_dl_start = 0;
    }
}

// At its return: $v0 is the end of what it wrote.
extern "C" void conker_frame_copy_end(uint8_t* rdram, recomp_context* ctx) {
    gpr start = frame_copy_dl_start;
    gpr end = ctx->r2;
    frame_copy_dl_start = 0;
    if (start == 0 || end <= start || end - start > 0x10000) {
        return;
    }
    if (conker_pause_background_redraw(rdram, start, end)) {
        return;
    }
    gpr syncs_before[2] = { 0, 0 };
    int before_count = 0;
    gpr last_rect = 0;
    gpr last_sync = 0;
    for (gpr cmd = start; cmd < end; cmd += 8) {
        if (((uint32_t)MEM_W(0, cmd) >> 24) == g_texrect) {
            last_rect = cmd;
            cmd += 16; // its two RDPHALF words
        }
        else if (is_sync(rdram, cmd)) {
            if (last_rect == 0 && before_count < 2) {
                syncs_before[before_count++] = cmd;
            }
            last_sync = cmd;
        }
    }
    if (before_count < 2 || last_rect == 0 || last_sync < last_rect) {
        return;
    }
    gpr dl = syncs_before[0];
    put_command(rdram, dl, (rt64_hook_opcode << 24) | rt64_hook_magic, (rt64_hook_op_enable << 28) | rt64_extended_opcode);
    dl = syncs_before[1];
    put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_setrectaspect_v1, conker_pause_background_stretch() ? g_ex_aspect_stretch : g_ex_aspect_zoom);
    dl = last_sync;
    put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_setrectaspect_v1, g_ex_aspect_auto);
}

// func_15180580 draws the circle wipe of spawning, dying and changing levels: black
// rectangles around the circle, clipped to the camera, and the circle's four
// quarters as texture rectangles, whose left edges are clamped to 0. RT64 widens
// the rectangles that touch one side of the frame, but when the circle is wider
// than the 4:3 frame, the side rectangles are empty and aren't drawn, and the left
// quarters are cut at 0, so the widened sides show the game through the wipe.
//
// The wipe is rewritten into a display list of our own: the black rectangles
// become RT64's extended ones, with signed coordinates reaching past the frame's
// edges, and the left quarters extended texture rectangles starting where the
// circle does. The display list the game wrote into can't grow, so its first
// command becomes a branch to ours, which branches back to where the game's
// continues. Ours are in RDRAM past the game's 8 MB, where RT64 can still read
// them (it masks addresses to 16 MB) and mods don't load (from 0x81000000).
namespace {
    constexpr uint32_t g_ex_fillrect_v1 = 0x000003;
    constexpr uint32_t g_dl_branch = 0xDE010000;
    constexpr uint32_t wide_dl_start = 0x00F00000;
    // A ring of display lists: RT64 has long finished with one when it comes round again.
    constexpr uint32_t wide_dl_size = 0x10000;
    uint32_t wide_dl_offset = 0;

    // Beyond the frame's sides, in N64 screen pixels. The scissor clips the rest.
    constexpr int32_t wide_reach = 1024;
    // The cameras (D_800BE628, 0x180 bytes each) and their clip bounds.
    constexpr gpr cameras_pointer = (gpr)(int32_t)0x800BE628;
    constexpr uint32_t camera_size = 0x180;
    constexpr int32_t camera_top = 0x24, camera_bottom = 0x28, camera_left = 0x2C, camera_right = 0x30;

    gpr iris_dl_start = 0;
    int32_t iris_camera = 0;

    struct Command {
        uint32_t w0, w1;
        uint32_t op() const { return w0 >> 24; }
    };

    int32_t camera_bound(uint8_t* rdram, int32_t camera, int32_t field) {
        gpr base = (gpr)MEM_W(0, cameras_pointer) + camera * camera_size;
        uint32_t word = (uint32_t)MEM_W(field, base);
        float value;
        std::memcpy(&value, &word, sizeof(value));
        return (int32_t)value;
    }

    void put_fill(uint8_t* rdram, gpr& dl, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
        if (lrx <= ulx || lry <= uly) {
            return;
        }
        put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_fillrect_v1, g_ex_origin_none | (g_ex_origin_none << 12));
        put_command(rdram, dl, ((uint32_t)(ulx * 4) << 16) | ((uint32_t)(uly * 4) & 0xFFFF),
            ((uint32_t)(lrx * 4) << 16) | ((uint32_t)(lry * 4) & 0xFFFF));
    }
}

// At the start of the function: $a0 is where it writes its first command, $a1 the camera.
extern "C" void conker_iris_begin(uint8_t* rdram, recomp_context* ctx) {
    iris_dl_start = ctx->r4;
    iris_camera = (int32_t)ctx->r5;
}

// At its return: $v0 is the end of what it wrote.
extern "C" void conker_iris_end(uint8_t* rdram, recomp_context* ctx) {
    gpr start = iris_dl_start;
    gpr end = ctx->r2;
    iris_dl_start = 0;
    constexpr size_t max_commands = 64;
    if (start == 0 || end <= start || end - start > max_commands * 8) {
        return;
    }
    Command cmds[max_commands];
    size_t count = (size_t)(end - start) / 8;
    for (size_t i = 0; i < count; i++) {
        cmds[i] = { (uint32_t)MEM_W(i * 8, start), (uint32_t)MEM_W(i * 8 + 4, start) };
    }

    // The wipe in progress: 8 commands of setup (a display list call first), the
    // black rectangles, the texture's setup, the four quarters (a G_TEXRECT and its
    // two G_RDPHALF words each: below right, above left, above right, below left)
    // and the geometry mode's restore. Anything else is left as the game drew it.
    constexpr size_t setup_count = 8;
    if (count < setup_count || cmds[0].op() != 0xDE) {
        return;
    }
    size_t texture_setup = setup_count;
    while (texture_setup < count && cmds[texture_setup].op() == 0xF6) {
        texture_setup++;
    }
    constexpr size_t quarter_count = 4;
    if (count < texture_setup + quarter_count * 3 + 2) {
        return;
    }
    size_t quarters = count - 1 - quarter_count * 3;
    if (cmds[count - 1].op() != 0xD9) {
        return;
    }
    for (size_t i = texture_setup; i < quarters; i++) {
        if (cmds[i].op() == 0xE4 || cmds[i].op() == 0xF6) {
            return;
        }
    }
    for (size_t q = 0; q < quarter_count; q++) {
        const Command* rect = &cmds[quarters + q * 3];
        if (rect[0].op() != 0xE4 || rect[1].op() != 0xE1 || rect[2].op() != 0xF1) {
            return;
        }
    }
    auto ulx = [&](size_t q) { return (int32_t)((cmds[quarters + q * 3].w1 >> 12) & 0xFFF); };
    auto uly = [&](size_t q) { return (int32_t)(cmds[quarters + q * 3].w1 & 0xFFF); };
    auto lrx = [&](size_t q) { return (int32_t)((cmds[quarters + q * 3].w0 >> 12) & 0xFFF); };
    auto lry = [&](size_t q) { return (int32_t)(cmds[quarters + q * 3].w0 & 0xFFF); };
    // The circle, in 10.2 fixed point, from the unclamped quarter below right.
    int32_t cx4 = ulx(0);
    int32_t cy4 = uly(0);
    int32_t r4 = lrx(0) - cx4;
    if (r4 <= 0 || lry(0) - cy4 != r4 || lrx(1) != cx4 || lrx(3) != cx4) {
        return;
    }

    uint32_t needed = (1 + (uint32_t)count + quarter_count * 2 + 1) * 8;
    if (wide_dl_offset + needed > wide_dl_size) {
        wide_dl_offset = 0;
    }
    uint32_t wide_dl = wide_dl_start + wide_dl_offset;
    wide_dl_offset += (needed + 15) & ~15u;
    gpr dl = (gpr)(int32_t)(0x80000000u | wide_dl);

    put_command(rdram, dl, (rt64_hook_opcode << 24) | rt64_hook_magic, (rt64_hook_op_enable << 28) | rt64_extended_opcode);
    for (size_t i = 0; i < setup_count; i++) {
        put_command(rdram, dl, cmds[i].w0, cmds[i].w1);
    }

    // The black around the circle: bands above and below it, and its sides, which
    // reach past the frame's edges where the camera touches them.
    int32_t top = camera_bound(rdram, iris_camera, camera_top);
    int32_t bottom = camera_bound(rdram, iris_camera, camera_bottom);
    int32_t left = camera_bound(rdram, iris_camera, camera_left);
    int32_t right = camera_bound(rdram, iris_camera, camera_right);
    int32_t cx = cx4 / 4, cy = cy4 / 4, r = r4 / 4;
    // A full-screen camera is clipped to 2..290; split-screen ones stop in the middle.
    int32_t wide_left = left <= 4 ? left - wide_reach : left;
    int32_t wide_right = right >= 286 ? right + wide_reach : right;
    int32_t band_top = std::max(top, cy - r);
    int32_t band_bottom = std::min(bottom, cy + r);
    put_fill(rdram, dl, wide_left, top, wide_right, band_top);
    put_fill(rdram, dl, wide_left, band_bottom, wide_right, bottom);
    put_fill(rdram, dl, wide_left, band_top, std::min(cx - r, wide_right), band_bottom);
    put_fill(rdram, dl, std::max(cx + r, wide_left), band_top, wide_right, band_bottom);

    for (size_t i = texture_setup; i < quarters; i++) {
        put_command(rdram, dl, cmds[i].w0, cmds[i].w1);
    }
    for (size_t q = 0; q < quarter_count; q++) {
        const Command* rect = &cmds[quarters + q * 3];
        bool left_quarter = q == 1 || q == 3;
        if (!left_quarter || cx4 - r4 >= 0) {
            put_command(rdram, dl, rect[0].w0, rect[0].w1);
            put_command(rdram, dl, rect[1].w0, rect[1].w1);
            put_command(rdram, dl, rect[2].w0, rect[2].w1);
            continue;
        }
        // Undo the clamp: move the texture start back to where the circle starts.
        int32_t new_ulx = cx4 - r4;
        int32_t dsdx = (int16_t)(rect[2].w1 >> 16);
        int32_t s = (int16_t)(rect[1].w1 >> 16) + ((new_ulx * dsdx) >> 7);
        uint32_t t = rect[1].w1 & 0xFFFF;
        const uint32_t tile = (rect[0].w1 >> 24) & 0x7;
        put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_texrect_v1,
            tile | (g_ex_origin_none << 3) | (g_ex_origin_none << 15));
        put_command(rdram, dl, ((uint32_t)new_ulx << 16) | (uint32_t)uly(q),
            ((uint32_t)lrx(q) << 16) | (uint32_t)lry(q));
        put_command(rdram, dl, ((uint32_t)s << 16) | t, rect[2].w1);
    }
    put_command(rdram, dl, cmds[count - 1].w0, cmds[count - 1].w1);
    put_command(rdram, dl, g_dl_branch, (uint32_t)end & 0x00FFFFFF);

    // The game's first command branches to ours; the rest of its wipe is skipped.
    gpr game_dl = start;
    put_command(rdram, game_dl, g_dl_branch, wide_dl);
}

// func_1501B22C builds a camera's frustum from its fields of view: the view-space
// normals of its four side planes, which the game culls the world and its objects
// against. RT64 widens the picture, but those planes still hold the 4:3 view, so
// the level's pieces past the 4:3 edges weren't drawn (holes at the sides, and
// geometry popping in and out as the camera turned). At its return ($s0 is the
// camera), rebuild the left and right planes from the horizontal field of view
// as wide as the window shows it: tan(half angle) grows with the aspect ratio.
extern "C" void conker_widen_frustum(uint8_t* rdram, recomp_context* ctx) {
    const float ratio = widescreen_ratio();
    if (ratio <= 1.0f || no_cull_widen()) {
        return;
    }
    const gpr camera = ctx->r16;
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    const float half_x = read_float(rdram, camera, 0x74) * 0.5f * degrees_to_radians;
    const float wide_half_x = std::min(std::atan(std::tan(half_x) * ratio * cull_extra()), 1.5f);
    const float c = std::cos(wide_half_x);
    const float s = std::sin(wide_half_x);
    // Left (cos, 0, -sin) and right (-cos, 0, -sin), as the game writes them.
    write_float(rdram, camera, 0x88, c);
    write_float(rdram, camera, 0x8C, 0.0f);
    write_float(rdram, camera, 0x90, -s);
    write_float(rdram, camera, 0x94, -c);
    write_float(rdram, camera, 0x98, 0.0f);
    write_float(rdram, camera, 0x9C, -s);
}

// updateCullScales_1510B958 sets the scale that the game's other culls (the level's
// pieces among them: func_150A5378, func_150A6210, func_1510AEE0) multiply a
// view-space x by before comparing it with the depth: a point is kept while
// |x| * scale <= depth, the 4:3 view. At its return, divide it by how much wider
// the window is, so they keep what the widened view shows.
extern "C" void conker_widen_cull_scale(uint8_t* rdram, recomp_context* ctx) {
    const float ratio = widescreen_ratio();
    if (ratio <= 1.0f || no_cull_widen()) {
        return;
    }
    const gpr cull_scale_x = (gpr)(int32_t)0x800D35E0; // cullScaleX_800D35E0
    write_float(rdram, cull_scale_x, 0, read_float(rdram, cull_scale_x, 0) / (ratio * cull_extra()));
}

// Some levels' backdrop (sky and distant scenery) is a grid of cells: func_15110CFC splits
// it down to the cells in view (sphere tests, func_150A6210, with the widened cull scale)
// and writes 4 vertices per cell into a buffer func_15000AD0 allocates per camera, two
// halves of 448 vertices, one per frame in flight. It never checks the end: Rare's 4:3
// view can't hold more cells than that, but a wide one can, and the extra vertices
// overran the next heap block (a crash in the heap walk, func_10004250, looking at the
// sky on ultrawide screens and in the duct tape and exploding mouse cutscenes). At
// 0x15000B80 $s1 holds the block's size and $v1 the second half's offset: make both
// four times as big, enough for a 32:9 view with room to spare (43 KB more a camera).
namespace {
    constexpr uint32_t backdrop_buffer_scale = 4;
}

extern "C" void conker_widen_backdrop_buffers(uint8_t* rdram, recomp_context* ctx) {
    ctx->r17 = (gpr)(int64_t)(int32_t)((uint32_t)ctx->r17 * backdrop_buffer_scale);
    ctx->r3 = (gpr)(int64_t)(int32_t)((uint32_t)ctx->r3 * backdrop_buffer_scale);
}

// func_15111AF4 draws the backdrop (sky and distant scenery) in sectors around the
// camera, and only those whose middle is within $f0 degrees of the way it looks: 95 (80
// in split screen), room for Rare's 4:3 view (30 degrees each side), half a sector (45)
// and some to spare. A wider view reaches further each side, and further still when
// it looks up: its top corners sweep across more directions, and looking steeply up
// it takes in nearly all of them. So the outermost sectors weren't drawn, and the sky's
// edges showed at the sides. At 0x15111BC4, just after $f0 is set, let in every sector
// the view's corners can reach: from the window's horizontal field of view, the
// camera's vertical one and how far it looks up or down (its view matrix, D_800D9C10).
extern "C" void conker_widen_backdrop_sectors(uint8_t* rdram, recomp_context* ctx) {
    const float ratio = widescreen_ratio();
    if (ratio <= 1.0f) {
        return;
    }
    constexpr float degrees_to_radians = 3.14159265358979f / 180.0f;
    constexpr float half_sector = 45.0f, spare = 10.0f;
    const uint32_t camera_index = (uint32_t)MEM_W(0, (gpr)(int32_t)0x80082FA4); // D_80082FA4
    const gpr camera = (gpr)(int32_t)MEM_W(0, (gpr)(int32_t)0x800BE628) + (gpr)(int32_t)(camera_index * 0x180);
    const gpr view = (gpr)(int32_t)(0x800D9C10 + camera_index * 0x40);
    const float half_x = std::atan(std::tan(read_float(rdram, camera, 0x74) * 0.5f * degrees_to_radians) * ratio);
    const float half_y = read_float(rdram, camera, 0x78) * 0.5f * degrees_to_radians;
    // The camera looks along -z of its view: the y part of that is its pitch.
    const float pitch = std::asin(std::clamp(-read_float(rdram, view, 0x18), -1.0f, 1.0f));
    const float corner_pitch = std::fabs(pitch) + half_y;
    float reach = 180.0f;
    if (corner_pitch < 89.0f * degrees_to_radians) {
        reach = std::atan(std::tan(half_x) / std::cos(corner_pitch)) / degrees_to_radians + half_sector + spare;
    }
    ctx->f0.fl = std::max(ctx->f0.fl, std::min(reach, 180.0f));
}
