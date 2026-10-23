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

#if defined(CONKER_RT64)
namespace RT64 {
    // rt64_cbfd_016.patch: the framebuffer the frames the game makes from now on keep for the next (0: none).
    void setHeldFramebuffer(uint32_t address);
}
#endif

namespace {
    // The motion blur was drawn in the frame being made (conker::motion_blur_drawn).
    bool motion_blur_this_frame = false;
    // Where the motion blur's copy of the last frame is (D_800BE570).
    constexpr uint32_t motion_blur_copy = 0x800BE570;
}

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
// Light glows (func_151408A4): before drawing one, the game checks its light's screen position
// against the camera's picture (camera + 0x2C to + 0x30 across, the 4:3 frame), and drops the glow
// outside it. In widescreen the picture is wider, so lights in the sides lost their glows. The
// left bound ($f10, at 0x15140988) and the right one ($f4, at 0x151409A0) move out as the sprites'.
extern "C" void conker_widen_glow_left(uint8_t* rdram, recomp_context* ctx) {
    ctx->f10.fl -= sprite_cull_margin(rdram) - 8.0f;
}

extern "C" void conker_widen_glow_right(uint8_t* rdram, recomp_context* ctx) {
    ctx->f4.fl += sprite_cull_margin(rdram) - 8.0f;
}

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
    conker::shadows::game_frame();
#if defined(CONKER_RT64)
    // The motion blur was drawn in this frame: its copy of the frame is kept for the next (motion_blur_drawn).
    RT64::setHeldFramebuffer(motion_blur_this_frame ? (uint32_t)MEM_W(0, (gpr)(int32_t)motion_blur_copy) : 0);
#endif
    motion_blur_this_frame = false;
    conker::fps_counter::game_frame();
}

// The motion blur draws its copy of the last frame over each frame, then copies the frame into it.
// RT64's frames between the game's (above 30 frames a second) copied themselves into it too, leaving
// a copy a fraction of a frame old: the ghosts all but covered the picture and the blur was nearly
// gone (Conker drunk). Pausing those frames instead dropped the game to 30 frames a second while it
// was drawn (issue #28). RT64 is told the copy's address (rt64_cbfd_016.patch, RT64::setHeldFramebuffer):
// frames between the game's draw with the copy the game's last frame left, and only its own frame
// updates it, as on the N64, while the picture still moves at the full rate.
// From qol.cpp's conker_motion_blur, where func_151D6778 is about to draw the blur (not when Reduce
// Motion Effects takes it out).
void conker::motion_blur_drawn() {
    motion_blur_this_frame = true;
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

// The bullet holes and cracks on the screen in It's War. func_150E76D0 makes each against a side of
// the screen: on the left its x is its half width less 145 (the screen's middle is 146 across), so its left edge
// is at 1, the 4:3 screen's; on the right 145 less its half width (mirrored), its right edge at 291. In
// widescreen they hung in the picture beside the 4:3 frame. Each is moved out as far as the window reaches past
// the 4:3 screen: the left one at 0x150E789C ($sp + 0x54, its x), the right one at 0x150E7880 ($f18, its x,
// stored to $sp + 0x54 in the branch's delay slot).
namespace {
    float hole_widening(uint8_t* rdram) {
        const float ratio = widescreen_ratio();
        if (ratio <= 1.0f) {
            return 0.0f;
        }
        const float half_frame = (float)MEM_W(0, (gpr)(int32_t)0x800BE620) * 0.5f; // D_800BE620
        return half_frame * (ratio - 1.0f);
    }
}

extern "C" void conker_widen_left_hole(uint8_t* rdram, recomp_context* ctx) {
    const gpr sp = ctx->r29;
    write_float(rdram, sp, 0x54, stack_float(rdram, sp, 0x54) - hole_widening(rdram));
}

extern "C" void conker_widen_right_hole(uint8_t* rdram, recomp_context* ctx) {
    ctx->f18.fl += hole_widening(rdram);
}

// Overlay pieces against a side of the screen: the wasp hive turret's frame is func_15154D80's
// pieces in the 292-wide 4:3 screen, its top and bottom bars four tiles each from x 3 to 289, the outer ones the
// bars' rounded ends. In widescreen the frame stopped at the 4:3 screen's edges, the picture going on beside it.
// A wide piece (a fifth of the screen or more) whose left edge is at or past the 4:3 screen's (within 4 pixels),
// or its right edge at or past its right, is stretched out as far as the window reaches past it: the bars' ends
// reach the window's edges, and the pieces inside them (the bars' middle, the gun mounts, the sights) stay as
// they are. The holes and cracks (above) are moved out instead, and are narrower, so they don't count. At
// 0x15155100 $f14 is the piece's middle and $f18 its half width, in the screen's pixels, before its rectangle is
// worked out from them (its texture's steps from its width, so the texture is stretched with it).
extern "C" void conker_widen_overlay_piece(uint8_t* rdram, recomp_context* ctx) {
    const float extra = hole_widening(rdram);
    if (extra <= 0.0f) {
        return;
    }
    constexpr float edge_slack = 4.0f;
    const float frame_width = (float)MEM_W(0, (gpr)(int32_t)0x800BE620); // D_800BE620
    float left = ctx->f14.fl - ctx->f18.fl;
    float right = ctx->f14.fl + ctx->f18.fl;
    if (right - left < frame_width * 0.2f) {
        return;
    }
    const bool at_left = left <= edge_slack;
    const bool at_right = right >= frame_width - edge_slack;
    if (!at_left && !at_right) {
        return;
    }
    if (at_left) {
        left -= extra;
    }
    if (at_right) {
        right += extra;
    }
    ctx->f14.fl = (left + right) * 0.5f;
    ctx->f18.fl = (right - left) * 0.5f;
}

// Screen overlays drawn with func_15154D80: the bullet holes (above), and others like them. Like the sprites,
// its G_TEXRECT has its corners clamped to 0 and its texture start moved on to match, so a hole moved past the
// 4:3 frame's left edge would be cut there. At 0x15155470 the rectangle and its two G_RDPHALF words are
// written ($t4 is the first half, 8 past the rectangle). The corners as worked out, in 10.2 fixed point, are at $sp + 0x60 (left),
// 0x5C (top), 0x58 (right) and 0x54 (bottom); the texture start before the clamp's change in $t5 and $ra, the
// steps in $t3 and $t2. A clamped one is rewritten as RT64's extended rectangle, three commands as well, in
// frames whose camera pass enabled the extended GBI (conker_camera_pass_sync).
extern "C" void conker_emit_overlay_texrect(uint8_t* rdram, recomp_context* ctx) {
    if (!extended_enabled || widescreen_ratio() <= 1.0f) {
        return;
    }
    const gpr sp = ctx->r29;
    const int32_t ulx = (int32_t)stack_float(rdram, sp, 0x60);
    const int32_t uly = (int32_t)stack_float(rdram, sp, 0x5C);
    const int32_t lrx = (int32_t)stack_float(rdram, sp, 0x58);
    const int32_t lry = (int32_t)stack_float(rdram, sp, 0x54);
    if (ulx >= 0 && uly >= 0) {
        return; // drawn as the game wrote it
    }
    gpr dl = ctx->r12 - 8; // $t4
    if (((uint32_t)MEM_W(0, dl) >> 24) != 0xE4 || ((uint32_t)MEM_W(8, dl) >> 24) != 0xE1 || ((uint32_t)MEM_W(16, dl) >> 24) != 0xF1) {
        return;
    }
    const uint32_t s = (uint32_t)ctx->r13 & 0xFFFF;    // $t5
    const uint32_t t = (uint32_t)ctx->r31 & 0xFFFF;    // $ra
    const uint32_t dsdx = (uint32_t)ctx->r11 & 0xFFFF; // $t3
    const uint32_t dtdy = (uint32_t)ctx->r10 & 0xFFFF; // $t2
    const uint32_t tile = 0;
    put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_texrect_v1, tile | (g_ex_origin_none << 3) | (g_ex_origin_none << 15));
    put_command(rdram, dl, ((uint32_t)(ulx & 0xFFFF) << 16) | (uint32_t)(uly & 0xFFFF), ((uint32_t)(lrx & 0xFFFF) << 16) | (uint32_t)(lry & 0xFFFF));
    put_command(rdram, dl, (s << 16) | t, (dsdx << 16) | dtdy);
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
//
// The Haybot's red view (Barn Boys, issue #33) is func_151A5170 drawing the frame back over itself, tinted, in
// strips of up to 256 pixels read straight from the colour image: only across the camera's 4:3 bounds, so in
// widescreen the right of the window wasn't tinted (the left strip, touching the frame's side, was widened by
// RT64). It's stretched as the pause's copy is: across the window, each strip from its part of the widened frame.
namespace {
    constexpr uint32_t g_ex_setrectaspect_v1 = 0x000033;
    constexpr uint32_t g_ex_aspect_auto = 0x0;
    constexpr uint32_t g_ex_aspect_stretch = 0x1;
    constexpr uint32_t g_ex_aspect_zoom = 0x3;
    constexpr uint32_t g_texrect = 0xE4;

    gpr frame_copy_dl_start = 0;
    // The function running now draws the frame back over itself, tinted (func_151A5170).
    bool frame_copy_tint = false;

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

// The tinted copy's strips: two columns, x 2 to 258 in rows of 8 and 258 to 294 in rows of 54 (each strip a
// G_SETTIMG of the colour image, G_SETTILE, G_LOADTILE, G_SETTILE, G_SETTILESIZE and a G_TEXRECT, after a sync).
// RT64 drew the first column from its copy of the frame, but not the second, which it read from RDRAM, an old
// picture. Two things kept the copy from it:
// - The tile's size (G_SETTILESIZE) is a texel more each way than what's loaded (a strip of 54 rows is given 0 to
//   54, inclusive), so the texture memory RT64 takes it to sample runs past what was loaded (0 to 495 words of a
//   486-word load) and no copy matches. On the N64 that row and column are never drawn. (The first column's does
//   too, but its 8 rows fill all of texture memory, which RT64 then checks whole.)
// - The camera is x 2 to 294 and the colour image 292 wide, so RT64's copy of the second column stops at the
//   image's edge, 34 pixels, and its rectangle samples 36.
// So each strip's tile is given the size it was loaded, and a strip reaching past the image's last column stops
// there (its rectangle, tile and load: the camera's 2-pixel border at the right, past the picture).
namespace {
    void fit_tint_copy(uint8_t* rdram, gpr start, gpr end) {
        constexpr int strip_commands = 8; // from the G_SETTIMG to the G_TEXRECT's second RDPHALF
        constexpr uint8_t strip_ops[strip_commands] = { 0xFD, 0xF5, 0xF4, 0xF5, 0xF2, 0xE4, 0xE1, 0xF1 };
        for (gpr cmd = start; cmd + strip_commands * 8 <= end; cmd += 8) {
            bool strip = true;
            for (int i = 0; i < strip_commands && strip; i++) {
                strip = ((uint32_t)MEM_W(i * 8, cmd) >> 24) == strip_ops[i];
            }
            if (!strip) {
                continue;
            }
            const gpr image = cmd, load = cmd + 2 * 8, size = cmd + 4 * 8, rect = cmd + 5 * 8;
            cmd += (strip_commands - 1) * 8;
            const uint32_t edge = (((uint32_t)MEM_W(0, image) & 0xFFF) + 1) << 2; // the image's width, 10.2
            const uint32_t rect_w0 = (uint32_t)MEM_W(0, rect), rect_w1 = (uint32_t)MEM_W(4, rect);
            const uint32_t ulx = (rect_w1 >> 12) & 0xFFF, uly = rect_w1 & 0xFFF;
            uint32_t lrx = (rect_w0 >> 12) & 0xFFF;
            const uint32_t lry = rect_w0 & 0xFFF;
            if (ulx >= edge || lrx <= ulx + 4 || lry <= uly + 4) {
                continue;
            }
            if (lrx > edge) {
                lrx = edge;
                MEM_W(0, rect) = (int32_t)((rect_w0 & ~(0xFFFu << 12)) | (lrx << 12));
                const uint32_t load_w1 = (uint32_t)MEM_W(4, load);
                if (((load_w1 >> 12) & 0xFFF) > edge - 4) {
                    MEM_W(4, load) = (int32_t)((load_w1 & ~(0xFFFu << 12)) | ((edge - 4) << 12));
                }
            }
            // The tile: from 0 to its last texel, inclusive, as wide and tall as the rectangle draws it.
            const uint32_t size_w1 = (uint32_t)MEM_W(4, size);
            MEM_W(4, size) = (int32_t)((size_w1 & 0x07000000u) | ((lrx - ulx - 4) << 12) | (lry - uly - 4));
        }
    }
}

// func_151A5170, at its first instruction: $a0 is where it writes its first command (a pipe sync).
extern "C" void conker_tint_copy_begin(uint8_t* rdram, recomp_context* ctx) {
    frame_copy_dl_start = ctx->r4;
    frame_copy_tint = true;
}

// At its return: $v0 is the end of what it wrote.
extern "C" void conker_frame_copy_end(uint8_t* rdram, recomp_context* ctx) {
    gpr start = frame_copy_dl_start;
    gpr end = ctx->r2;
    const bool tint = frame_copy_tint;
    frame_copy_dl_start = 0;
    frame_copy_tint = false;
    if (start == 0 || end <= start || end - start > 0x10000) {
        return;
    }
    if (tint) {
        fit_tint_copy(rdram, start, end);
    } else if (conker_pause_background_redraw(rdram, start, end)) {
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
    put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_setrectaspect_v1, (tint || conker_pause_background_stretch()) ? g_ex_aspect_stretch : g_ex_aspect_zoom);
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

// func_15181EE0 draws eyelids over the picture (waking up on the boat at the start of It's War, opening and
// blinking; D_800DDE18 points at how shut they are): black fill rectangles above and below (func_1517F9F4), and
// a textured strip with the lid's ragged edge along each, all clipped to the camera's sides, the 4:3 frame. RT64
// doesn't widen those, so in widescreen the sides showed the game past the lids' ends. As with the circle wipe,
// what it wrote is redrawn in a display list of ours: the pieces that reach the frame's sides reach past them,
// the strips' texture carried along (it repeats across).
namespace {
    gpr eyelids_dl_start = 0;
}

extern "C" void conker_eyelids_begin(uint8_t* rdram, recomp_context* ctx) {
    eyelids_dl_start = ctx->r4;
}

// At its return: $v0 is the end of what it wrote (the start, when the eyes are open).
extern "C" void conker_eyelids_end(uint8_t* rdram, recomp_context* ctx) {
    const gpr start = eyelids_dl_start;
    const gpr end = ctx->r2;
    eyelids_dl_start = 0;
    constexpr size_t max_commands = 96;
    if (start == 0 || end <= start || end - start > max_commands * 8) {
        return;
    }
    Command cmds[max_commands];
    const size_t count = (size_t)(end - start) / 8;
    for (size_t i = 0; i < count; i++) {
        cmds[i] = { (uint32_t)MEM_W(i * 8, start), (uint32_t)MEM_W(i * 8 + 4, start) };
    }
    const int32_t left = camera_bound(rdram, 0, camera_left);
    const int32_t right = camera_bound(rdram, 0, camera_right);
    const int32_t wide_left = left <= 4 ? left - wide_reach : left;
    const int32_t wide_right = right >= 286 ? right + wide_reach : right;
    if (wide_left == left && wide_right == right) {
        return;
    }
    const uint32_t needed = (2 + (uint32_t)count * 2) * 8;
    if (wide_dl_offset + needed > wide_dl_size) {
        wide_dl_offset = 0;
    }
    const uint32_t wide_dl = wide_dl_start + wide_dl_offset;
    wide_dl_offset += (needed + 15) & ~15u;
    gpr dl = (gpr)(int32_t)(0x80000000u | wide_dl);
    put_command(rdram, dl, (rt64_hook_opcode << 24) | rt64_hook_magic, (rt64_hook_op_enable << 28) | rt64_extended_opcode);
    for (size_t i = 0; i < count; i++) {
        const Command& c = cmds[i];
        if (c.op() == 0xF6) {
            // G_FILLRECT: lower right in w0, upper left in w1 (10 bits each, then 2 of fraction).
            int32_t ulx = (int32_t)((c.w1 >> 14) & 0x3FF), uly = (int32_t)((c.w1 >> 2) & 0x3FF);
            int32_t lrx = (int32_t)((c.w0 >> 14) & 0x3FF), lry = (int32_t)((c.w0 >> 2) & 0x3FF);
            if (ulx <= left + 1) ulx = wide_left;
            if (lrx >= right - 1) lrx = wide_right;
            put_fill(rdram, dl, ulx, uly, lrx, lry);
            continue;
        }
        if (c.op() == 0xE4 && i + 2 < count && cmds[i + 1].op() == 0xE1 && cmds[i + 2].op() == 0xF1) {
            // G_TEXRECT and its two halves: 10.2 corners, then s and t (10.5), then the steps (5.10).
            int32_t ulx4 = (int32_t)((c.w1 >> 12) & 0xFFF), uly4 = (int32_t)(c.w1 & 0xFFF);
            int32_t lrx4 = (int32_t)((c.w0 >> 12) & 0xFFF), lry4 = (int32_t)(c.w0 & 0xFFF);
            const uint32_t tile = (c.w1 >> 24) & 0x7;
            const int32_t dsdx = (int16_t)(cmds[i + 2].w1 >> 16);
            int32_t s = (int16_t)(cmds[i + 1].w1 >> 16);
            const uint32_t t = cmds[i + 1].w1 & 0xFFFF;
            if (ulx4 <= (left + 1) * 4) {
                s += ((wide_left * 4 - ulx4) * dsdx) >> 7;
                ulx4 = wide_left * 4;
            }
            if (lrx4 >= (right - 1) * 4) {
                lrx4 = wide_right * 4;
            }
            put_command(rdram, dl, (rt64_extended_opcode << 24) | g_ex_texrect_v1,
                tile | (g_ex_origin_none << 3) | (g_ex_origin_none << 15));
            put_command(rdram, dl, ((uint32_t)(ulx4 & 0xFFFF) << 16) | (uint32_t)(uly4 & 0xFFFF),
                ((uint32_t)(lrx4 & 0xFFFF) << 16) | (uint32_t)(lry4 & 0xFFFF));
            put_command(rdram, dl, ((uint32_t)(s & 0xFFFF) << 16) | t, cmds[i + 2].w1);
            i += 2;
            continue;
        }
        put_command(rdram, dl, c.w0, c.w1);
    }
    put_command(rdram, dl, g_dl_branch, (uint32_t)end & 0x00FFFFFF);
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
