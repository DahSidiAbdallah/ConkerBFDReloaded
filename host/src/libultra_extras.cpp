// libultra pieces the recompiled game needs that neither the translation nor the
// runtime provides, and the helpers recomp/conker.toml's hooks call. The hook list
// comes from CBFD-Recompiled (MIT, Sean Ciaschi).

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
