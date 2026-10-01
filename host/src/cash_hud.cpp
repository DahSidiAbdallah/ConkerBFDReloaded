// Show Cash (Accessibility tab): Conker's cash on screen during play, drawn by the game's own
// pause-screen code so it looks exactly as it does there: the wad of bills with blinking eyes (a 3D
// model), the green "$" and the gold numbers. The original only shows cash on the pause screen.
//
// The pause screen (func_151EB06C) draws it with:
//   func_151ED90C(0xA4, 0x17, 0, 1.0f)  makes the wad's model (func_151EDB58 frees it);
//   func_151EF954(&D_800E0C38, ...)     the flat projection the pause's models are drawn with;
//   func_151EDBDC(gfx, model, x, y, s)  draws a model there (the wad blinks when it's D_80090058);
//   func_151ED430(gfx, &D_80090060, x, y, 1, 1, s, 0)  a picture ("$" is picture 0x7F6), centred;
//   func_151EADFC(gfx, x, y, value)    the gold numbers.
// Here the same calls are made from a hook at the end of the game's HUD (func_1508FD38, only while
// playing: not paused, not in multiplayer), adding to the frame's display list after the chocolate
// and the tail.
//
// When the cash changes, the numbers count to the new amount while the wad hops and the "$" pulses
// (a loss shakes the wad instead). With When It Changes it slides in from above for that and slides
// away a few seconds later, like the chocolate; with Always it stays. Hidden during cutscenes, and
// with no cash, as on the pause screen.
//
// The wad's model lives in the game's memory. A level load starts the HUD afresh (func_15017640:
// the chocolate's flying pieces are dropped there, not freed), so the model is dropped there too.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "recomp.h"
#include "conker.hpp"

extern "C" void func_151ED90C(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151EDB58(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151EDBDC(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151ED430(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151EADFC(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151EF954(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1509BE40(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1509B570(uint8_t* rdram, recomp_context* ctx);

namespace {
    using Func = void (*)(uint8_t*, recomp_context*);

    constexpr int32_t cash_stat = (int32_t)0x800D2148;      // player 1's cash (func_150859AC(0, 6))
    constexpr int32_t frame_ticks = (int32_t)0x800BE9E4;    // VIs the last game frame took
    constexpr int32_t camera = (int32_t)0x800BE628;         // pointer: the view (+4 width, +8 height)
    constexpr int32_t pause_projection = (int32_t)0x800E0C38;
    constexpr int32_t pause_model_alpha = (int32_t)0x800E0C30; // s16
    constexpr int32_t pause_cash_model = (int32_t)0x80090058;  // the pause's wad (blinks when drawn)
    constexpr int32_t pause_picture = (int32_t)0x80090060;     // the pause's picture info (+0 id)
    constexpr int32_t pause_setup_list = (int32_t)0x80090028;  // display list the pause starts with
    constexpr int32_t projection_far = (int32_t)0x800ABADC;    // float
    constexpr int32_t wad_scale = (int32_t)0x800ABAE0;         // float
    constexpr uint32_t dollar_picture = 0x7F6;

    // Where it goes: the pause screen's layout moved to the top-right corner (the chocolate has the
    // top left), its numbers ending at right_edge, so it grows to the left as the amount gets longer.
    constexpr int right_edge = 296;
    constexpr int move_y = -128; // (room above for the wad's hop)
    constexpr int digit_step = 24, digit_width = 32; // func_151EADFC's numbers

    uint32_t model = 0;          // the wad (game address), 0: none
    bool synced = false;         // shown_value has caught up with the game (no count on a load)
    int target = 0;              // the amount being counted to
    int count_from = 0;          // the amount the count started from
    float count_ticks = 0.0f, count_length = 1.0f;
    float shown_value = 0.0f;
    int shown_digits_value = -1;
    float hold = 0.0f;           // When It Changes: VIs left to stay on screen
    float slide = 0.0f;          // 0 hidden .. 1 in place
    float joy = 0.0f;            // the hop/pulse, VIs left
    float bump = 0.0f;           // the numbers' small hop when they change
    float clock_ticks = 0.0f;
    bool gained = true;

    int32_t word(uint8_t* rdram, int32_t address) { return MEM_W(0, (gpr)address); }
    float real(uint8_t* rdram, int32_t address) {
        int32_t bits = word(rdram, address);
        float value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
    uint32_t bits_of(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        return bits;
    }

    // Calls a game function from the hook: up to four arguments in registers, the rest on the stack
    // below the hooked function's frame; registers put back afterwards. Returns $v0.
    uint32_t call(uint8_t* rdram, recomp_context* ctx, Func func, std::initializer_list<uint32_t> args) {
        recomp_context saved = *ctx;
        const gpr sp = ctx->r29 - 0x40;
        int i = 0;
        for (uint32_t arg : args) {
            const gpr value = (gpr)(int32_t)arg;
            if (i == 0) {
                ctx->r4 = value;
            } else if (i == 1) {
                ctx->r5 = value;
            } else if (i == 2) {
                ctx->r6 = value;
            } else if (i == 3) {
                ctx->r7 = value;
            } else {
                MEM_W(0x10 + 4 * (i - 4), sp) = (int32_t)arg;
            }
            i++;
        }
        ctx->r29 = sp;
        func(rdram, ctx);
        const uint32_t result = (uint32_t)ctx->r2;
        *ctx = saved;
        return result;
    }

    void put(uint8_t* rdram, uint32_t& gfx, uint32_t w0, uint32_t w1) {
        MEM_W(0, (gpr)(int32_t)gfx) = (int32_t)w0;
        MEM_W(4, (gpr)(int32_t)gfx) = (int32_t)w1;
        gfx += 8;
    }

    // The amount as the pause screen shows it (func_151EB06C): 32000 stands for a million, and two
    // story moments show a share of it.
    int shown_amount(uint8_t* rdram, recomp_context* ctx) {
        int value = word(rdram, cash_stat);
        if (value == 32000) {
            value = 1000000;
        }
        if (call(rdram, ctx, func_1509BE40, { 0, 0x5082, 0x1A }) != 0 && call(rdram, ctx, func_1509BE40, { 0, 0x5083, 0x1A }) == 0) {
            const uint32_t thing = call(rdram, ctx, func_1509B570, { 0x83 });
            if (thing == 0) {
                value = 0;
            } else {
                const int share = word(rdram, (int32_t)(thing + 0x64));
                if (share == 0) {
                    value = 0;
                } else if (share < 3) {
                    value = value * share / 3;
                }
            }
        }
        if (MEM_BU(0, (gpr)(int32_t)0x800C35EA) == 1) {
            const int16_t taken = MEM_H(0, (gpr)(int32_t)0x800C3C9E);
            if (taken != -1) {
                value = taken == 0 ? 0 : (taken == 32000 ? 1000000 : value - taken);
            }
        }
        return value;
    }

    void drop_model(uint8_t* rdram, recomp_context* ctx) {
        if (model != 0) {
            call(rdram, ctx, func_151EDB58, { model });
            model = 0;
        }
    }

    float ease(float x) { return 1.0f - (1.0f - x) * (1.0f - x); }
}

// func_15017640: the HUD starts afresh for a new level; the game's memory for the old one is gone.
extern "C" void conker_cash_hud_reset(uint8_t* rdram, recomp_context* ctx) {
    model = 0;
    synced = false;
    slide = 0.0f;
    hold = 0.0f;
}

// func_1508FD38 after its last HUD piece: $v0 is the display list so far.
extern "C" void conker_cash_hud(uint8_t* rdram, recomp_context* ctx) {
    const int mode = conker::qol::cash_counter();
    if (mode == 0) {
        drop_model(rdram, ctx);
        synced = false;
        slide = 0.0f;
        return;
    }
    const float dt = (float)std::clamp(word(rdram, frame_ticks), 1, 6);
    clock_ticks += dt;

    const int amount = shown_amount(rdram, ctx);
    if (!synced) {
        synced = true;
        target = count_from = amount;
        shown_value = (float)amount;
        count_ticks = count_length = 1.0f;
    }
    if (amount != target) {
        // Count from where the numbers are now, taking longer for bigger changes (up to a second).
        count_from = (int)std::lround(shown_value);
        gained = amount > count_from;
        target = amount;
        count_ticks = 0.0f;
        count_length = std::clamp((float)std::abs(amount - count_from) * 1.5f, 24.0f, 60.0f);
        hold = 240.0f; // as long as the chocolate stays after a change
        joy = count_length + 12.0f;
    }
    if (count_ticks < count_length) {
        count_ticks = std::min(count_length, count_ticks + dt);
        shown_value = count_from + (target - count_from) * ease(count_ticks / count_length);
    } else {
        shown_value = (float)target;
        hold = std::max(0.0f, hold - dt);
    }
    joy = std::max(0.0f, joy - dt);
    bump = std::max(0.0f, bump - dt);
    const int digits_value = (int)std::lround(shown_value);
    if (digits_value != shown_digits_value) {
        if (shown_digits_value >= 0 && joy > 0.0f) {
            bump = 4.0f;
        }
        shown_digits_value = digits_value;
    }

    const bool counting = count_ticks < count_length;
    const bool wanted = !conker::qol::cutscene_playing() && (digits_value > 0 || counting) &&
        (mode == 2 || counting || hold > 0.0f);
    slide = std::clamp(slide + (wanted ? dt / 10.0f : -dt / 14.0f), 0.0f, 1.0f);
    if (slide <= 0.0f) {
        drop_model(rdram, ctx);
        return;
    }

    uint32_t gfx = (uint32_t)ctx->r2;
    const float in = ease(slide);
    const float drop = (1.0f - in) * 70.0f; // slides down from above the screen
    const uint32_t alpha = (uint32_t)std::lround(255.0f * in);

    // The hop and pulse while counting up; a shake while counting down.
    float hop = 0.0f, pulse = 0.0f, shake = 0.0f;
    if (joy > 0.0f) {
        const float fade = std::min(1.0f, joy / 12.0f);
        if (gained) {
            hop = std::fabs(std::sin(clock_ticks * 3.14159265f / 14.0f)) * 5.0f * fade;
            pulse = std::fabs(std::sin(clock_ticks * 3.14159265f / 14.0f)) * 0.2f * fade;
        } else {
            shake = std::sin(clock_ticks * 1.1f) * 3.0f * fade;
        }
    }

    // The pause's x for everything (t0 there): the "$" at + 0xA5, the numbers at + 0xAD, the wad at - 10.
    int digits = 1;
    for (int v = digits_value; v >= 10; v /= 10) {
        digits++;
    }
    const int base_x = right_edge - (digit_step * (digits - 1) + digit_width) - 0xAD;
    const int down = move_y - (int)std::lround(drop);

    // The wad: the pause's flat projection, then the model with the pause's own blink.
    if (model == 0) {
        model = call(rdram, ctx, func_151ED90C, { 0xA4, 0x17, 0, bits_of(1.0f) });
    }
    put(rdram, gfx, 0xDE000000, (uint32_t)pause_setup_list);
    put(rdram, gfx, 0xD9FFFFFF, 0x00220404);
    put(rdram, gfx, 0xFB000000, 0xFFFFFF00 | alpha);
    if (model != 0) {
        const int32_t view = word(rdram, camera);
        const float half_w = real(rdram, view + 4) * 0.5f, half_h = real(rdram, view + 8) * 0.5f;
        call(rdram, ctx, func_151EF954, { (uint32_t)pause_projection, bits_of(-half_w), bits_of(half_w), bits_of(-half_h),
            bits_of(half_h), bits_of(1.0f), bits_of(real(rdram, projection_far)), bits_of(1.0f) });
        const int16_t saved_alpha = MEM_H(0, (gpr)pause_model_alpha);
        const int32_t saved_model = word(rdram, pause_cash_model);
        MEM_H(0, (gpr)pause_model_alpha) = (int16_t)alpha;
        MEM_W(0, (gpr)pause_cash_model) = (int32_t)model;
        const float x = (float)base_x - 10.0f + shake;
        const float y = -83.0f - (float)down + hop;
        const float scale = real(rdram, wad_scale) * (1.0f + pulse);
        gfx = call(rdram, ctx, func_151EDBDC, { gfx, model, bits_of(x), bits_of(y), bits_of(scale) });
        MEM_W(0, (gpr)pause_cash_model) = saved_model;
        MEM_H(0, (gpr)pause_model_alpha) = saved_alpha;
    }

    // The "$" and the numbers, set up as the pause draws them.
    put(rdram, gfx, 0xDE000000, (uint32_t)pause_setup_list);
    put(rdram, gfx, 0xEF002C3F, 0x00504244);
    put(rdram, gfx, 0xE7000000, 0);
    put(rdram, gfx, 0xFB000000, 0xFFFFFF00 | alpha);
    const int32_t saved_picture = word(rdram, pause_picture);
    MEM_W(0, (gpr)pause_picture) = (int32_t)dollar_picture;
    gfx = call(rdram, ctx, func_151ED430, { gfx, (uint32_t)pause_picture, (uint32_t)(base_x + 0xA5), (uint32_t)(0xAA + down),
        1, 1, bits_of(1.0f + pulse), 0 });
    MEM_W(0, (gpr)pause_picture) = saved_picture;
    const int lift = (int)std::lround(bump * 0.75f);
    gfx = call(rdram, ctx, func_151EADFC, { gfx, (uint32_t)(base_x + 0xAD), (uint32_t)(0x9B + down - lift), (uint32_t)digits_value });

    ctx->r2 = (gpr)(int32_t)gfx;
}
