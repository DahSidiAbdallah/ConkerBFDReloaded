// libultra pieces the recompiled game needs that neither the translation nor the
// runtime provides, and the helpers recomp/conker.toml's hooks call. The hook list
// comes from CBFD-Recompiled (MIT, Sean Ciaschi).

#include <chrono>
#include <csetjmp>
#include <cstdio>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/error_handling.hpp"
#include "ultramodern/ultra64.h"

#include "conker.hpp"

// Big-endian word of cartridge ROM at a physical address, or false if outside it.
static bool rom_word(uint32_t phys, int32_t& out) {
    auto rom = recomp::get_rom();
    uint64_t off = (uint64_t)phys - recomp::rom_base;
    if (phys < recomp::rom_base || off + 4 > rom.size()) {
        return false;
    }
    out = (int32_t)(((uint32_t)rom[off] << 24) | ((uint32_t)rom[off + 1] << 16) | ((uint32_t)rom[off + 2] << 8) | rom[off + 3]);
    return true;
}

// Word reads through KSEG1 (0xA0000000 | phys): cartridge ROM (Rare's anti-piracy
// checks) or uncached RDRAM.
extern "C" int32_t conker_kseg1_read32(uint8_t* rdram, uint32_t vaddr) {
    uint32_t phys = vaddr & 0x1FFFFFFFu;
    int32_t word = 0;
    if (phys >= recomp::rom_base) {
        if (!rom_word(phys, word)) {
            std::fprintf(stderr, "[extras] KSEG1 read past the ROM: %08X\n", vaddr);
        }
        return word;
    }
    if (phys < 0x00800000u) {
        return MEM_W(0, (gpr)(int32_t)(0x80000000u | phys));
    }
    std::fprintf(stderr, "[extras] unhandled KSEG1 read: %08X\n", vaddr);
    return 0;
}

// Music (CBFD-Recompiled V0.1.5, issue #66; hooks in recomp/conker.toml).
// A pass of a busy-wait loop: func_10008CE8, which starts a song on a sequence player, stops the
// player and then counts up to 2,000,000 (then 4,000,000) while it waits for the audio thread to
// report it stopped. On the N64 the audio thread preempts the loop; here game threads switch only
// when one waits or yields, so the loop ran out without the audio thread running, and the new song
// went to a player still playing the old one (the bar's music played on after loading a save from
// the menu the game over leads to). This yields for up to 1 ms, letting the audio thread run, and
// counts that as the passes the N64 would have made in the time (about 3,000), so the loop still
// gives up after about as long as it would there. The count ($s0) stays at or under the loop's
// bound ($s1), which both loops end on.
extern "C" void yield_self_1ms(uint8_t* rdram);
extern "C" void conker_spin_wait_pass(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t passes_per_ms = 3000;
    yield_self_1ms(rdram);
    const uint32_t count = (uint32_t)ctx->r16;
    const uint32_t bound = (uint32_t)ctx->r17;
    ctx->r16 = (count < bound && bound - count > passes_per_ms) ? count + passes_per_ms : bound;
}

// A song just started still reads as stopped. Starting a song (func_10008CE8) only queues an event
// for the audio thread, and the player says it's stopped (its state, +0x2C, AL_STOPPED) until the
// audio thread has handled it, which here can be a frame or more later. The music manager
// (func_1000D2F8) asked the player then (func_1000853C), took the song for finished and freed its
// player while it played on: the outside ambience went on inside the bar, later songs landed on the
// wrong players, and in the stone dragon's mouth the wrong one faded while the level's music played
// on very loud. So until the audio thread has it, the player reads as playing: marked when
// func_10008CE8 starts its song, the mark cleared once it plays, when the game stops it, or after
// 500 ms (should the song never start).
namespace {
    constexpr int sequence_players = 3; // D_8003C900
    std::chrono::steady_clock::time_point song_started_at[sequence_players];
    bool song_just_started[sequence_players] = {};
}

// func_10008CE8 at 0x10008EC4, just after it starts the song: its player number is its first
// argument, the byte at $sp + 0x43.
extern "C" void conker_song_started(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t player = MEM_BU(0x43, ctx->r29);
    if (player >= sequence_players) {
        return;
    }
    song_just_started[player] = true;
    song_started_at[player] = std::chrono::steady_clock::now();
}

// func_10008F24 (stop a player) at its start: $a0 the player number.
extern "C" void conker_song_stopped(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t player = (uint32_t)ctx->r4 & 0xFF;
    if (player < sequence_players) {
        song_just_started[player] = false;
    }
}

// func_1000853C (a player's state) at 0x10008560, after reading it: $v0 the state, $a1 the player.
extern "C" void conker_song_state(uint8_t* rdram, recomp_context* ctx) {
    constexpr auto start_limit = std::chrono::milliseconds(500);
    const uint32_t player = (uint32_t)ctx->r5 & 0xFF;
    if (player >= sequence_players || !song_just_started[player]) {
        return;
    }
    if ((int32_t)ctx->r2 != 0 || std::chrono::steady_clock::now() - song_started_at[player] > start_limit) {
        song_just_started[player] = false;
        return;
    }
    ctx->r2 = 1; // AL_PLAYING
}

// Rare's script interpreter (func_150ADAF0) leaves nested calls with a longjmp-like
// jump (func_150AE280); the hooks use this buffer. Only the main thread runs scripts.
extern "C" { jmp_buf conker_interpreter_exit; }

// libultra's osContInit creates __osEepromTimerQ (0x80042A78, one slot at
// 0x80042A90), which Rare's EEPROM code waits on; the runtime's doesn't.
extern "C" void conker_create_eeprom_timer_queue(uint8_t* rdram) {
    const int32_t queue = (int32_t)0x80042A78, msgs = (int32_t)0x80042A90;
    if (TO_PTR(OSMesgQueue, queue)->msgCount == 0) {
        osCreateMesgQueue(rdram, queue, msgs, 1);
    }
}

// s32 osPiRawReadIo(u32 devAddr, u32 *data) / osPiReadIo: one ROM word.
static void pi_read_io(uint8_t* rdram, recomp_context* ctx) {
    uint32_t phys = (0xB0000000u | (uint32_t)ctx->r4) & 0x1FFFFFFFu;
    int32_t word;
    if (!rom_word(phys, word)) {
        std::fprintf(stderr, "[extras] PI read outside the ROM: %08X\n", (uint32_t)ctx->r4);
        ctx->r2 = -1;
        return;
    }
    MEM_W(0, ctx->r5) = word;
    ctx->r2 = 0;
}
extern "C" void osPiRawReadIo_recomp(uint8_t* rdram, recomp_context* ctx) { pi_read_io(rdram, ctx); }
extern "C" void osPiReadIo_recomp(uint8_t* rdram, recomp_context* ctx) { pi_read_io(rdram, ctx); }

// s32 osPfsInit(OSMesgQueue*, OSPfs*, int channel): Conker only asks what pak is
// in (func_15006234), then tries a Rumble Pak. No Controller Pak support, so say
// "some other device" when a pak is in, else "no pak".
extern "C" void osPfsInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    constexpr int32_t PFS_ERR_NOPACK = 1, PFS_ERR_DEVICE = 11;
    auto info = conker::frontend::device_info((int)ctx->r6);
    bool pak = info.connected_device == ultramodern::input::Device::Controller && info.connected_pak != ultramodern::input::Pak::None;
    ctx->r2 = pak ? PFS_ERR_DEVICE : PFS_ERR_NOPACK;
}

// Conker stops on fatal errors with a bare `syscall` (func_10007DA0, func_150AD770).
extern "C" void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t vram) {
    char msg[128];
    std::snprintf(msg, sizeof(msg), "The game stopped with an error (syscall at 0x%08X, called from 0x%08X).",
        (uint32_t)vram, (uint32_t)ctx->r31);
    std::fprintf(stderr, "[extras] %s\n", msg);
    ultramodern::error_handling::message_box(msg);
    ULTRAMODERN_QUICK_EXIT();
}
