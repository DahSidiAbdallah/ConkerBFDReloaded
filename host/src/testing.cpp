// Testing aids, all off unless their environment variable is set (run_host.bat):
//   CONKER_SPEED=N       runs the game N times faster (its clock and screen refreshes),
//                        with the sound muted, so test runs get through the intro quickly.
//   CONKER_SNAP_ROOM=hex    snap times are that room's timer (in refreshes; see on_vi).
//   CONKER_SNAP_AT=s+... at each of these game seconds, prints "[snap] s" and holds the
//                        game still until snaps\t<s>.png exists (snap.ps1 -Log takes it),
//                        so screenshots land on the same moment at any speed.
//   CONKER_RDRAM_DUMP=1  also saves the game's memory (8 MB, as RT64 sees it) at each of them,
//                        as snaps\t<s>.rdram (tools/textures/scan_rdram.py finds pictures in it).
// Times here are game time: screen refreshes (60 a second) since the game started.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"

namespace {
    std::atomic<uint32_t> vis{0};
    uint32_t speed = 1;
    struct Snap { uint32_t vi; std::string name; };
    std::vector<Snap> snap_at;
    size_t next_snap = 0;
    std::filesystem::path snap_dir;
}

void conker::testing::init() {
    if (const char* s = std::getenv("CONKER_SPEED")) {
        speed = (uint32_t)std::max(1, std::atoi(s));
        ultramodern::set_speed_multiplier(speed);
        std::printf("[testing] running %ux faster, sound muted\n", speed);
    }
    if (const char* s = std::getenv("CONKER_SNAP_AT")) {
        // Seconds, decimals allowed (e.g. 64.05), any other separator (the .bat gets them
        // joined with '+': cmd splits arguments at commas).
        for (const char* p = s; *p != '\0';) {
            if ((*p >= '0' && *p <= '9') || *p == '.') {
                const char* start = p;
                while ((*p >= '0' && *p <= '9') || *p == '.') p++;
                std::string token(start, p);
                // (With CONKER_SNAP_ROOM the numbers are that room's timer, not seconds.)
                double scale = std::getenv("CONKER_SNAP_ROOM") != nullptr ? 1.0 : 60.0;
                snap_at.push_back({ (uint32_t)(std::atof(token.c_str()) * scale + 0.5), token });
            } else {
                p++;
            }
        }
        const char* dir = std::getenv("CONKER_SNAP_DIR");
        snap_dir = dir != nullptr ? dir : "C:\\ConkerRecompWin\\snaps";
    }
}

uint32_t conker::testing::speed_multiplier() {
    return speed;
}

double conker::testing::game_seconds() {
    return vis.load() / 60.0;
}

// Called on every screen refresh (VI).
extern "C" volatile int rt64_cbfd_log_window; // rt64_rsp.cpp (testing)

void conker::testing::on_vi(uint8_t* rdram) {
    uint32_t now = ++vis;
    // CONKER_TEST_POKE=seconds:hexaddress:delta: adds delta to the byte at that address then (e.g. player 1's
    // lives, 0x800D2144), to make the game show its HUD (finding the HUD's timers).
    static double poke_at = -1.0; static uint32_t poke_addr = 0; static int poke_delta = 0;
    static const bool poke_parsed = [] {
        if (const char* p = std::getenv("CONKER_TEST_POKE")) { std::sscanf(p, "%lf:%x:%d", &poke_at, &poke_addr, &poke_delta); }
        return true;
    }();
    (void)poke_parsed;
    if (poke_at > 0 && rdram != nullptr && now == (uint32_t)(poke_at * 60.0)) {
        MEM_B(0, (gpr)(int32_t)poke_addr) = (int8_t)(MEM_B(0, (gpr)(int32_t)poke_addr) + poke_delta);
        std::printf("[testing] poke %08X += %d\n", poke_addr, poke_delta);
    }
    // Testing aid: CONKER_TEST_TEXT=seconds:findhex:replacehex: from then on, once a
    // second until found, look for that text in memory and replace it (same length).
    static double text_at = -1.0; static std::string text_find, text_repl; static bool text_done = false;
    static const bool text_parsed = [] {
        if (const char* p = std::getenv("CONKER_TEST_TEXT")) {
            char f[256] = {}, r[256] = {};
            if (std::sscanf(p, "%lf:%255[0-9a-fA-F]:%255[0-9a-fA-F]", &text_at, f, r) == 3) {
                auto unhex = [](const char* h) { std::string o; for (size_t i = 0; h[i] && h[i + 1]; i += 2) { unsigned v; std::sscanf(h + i, "%2x", &v); o.push_back((char)v); } return o; };
                text_find = unhex(f); text_repl = unhex(r);
            }
        }
        return true;
    }();
    (void)text_parsed;
    if (text_at > 0 && !text_done && rdram != nullptr && now >= (uint32_t)(text_at * 60.0) && now % 60 == 0 && !text_find.empty()) {
        const size_t n = text_find.size();
        for (uint32_t a = 0x80000000; a < 0x80800000 - n; a++) {
            bool match = true;
            for (size_t i = 0; i < n && match; i++) match = (uint8_t)MEM_B(0, (gpr)(int32_t)(a + i)) == (uint8_t)text_find[i];
            if (match) {
                for (size_t i = 0; i < text_repl.size(); i++) MEM_B(0, (gpr)(int32_t)(a + i)) = (int8_t)text_repl[i];
                std::printf("[testing] text replaced at %08X\n", a);
                text_done = true;
            }
        }
    }
    // CONKER_LOG_WINDOW=room:from:to (hex room, room timer): turn on RT64's lighting record
    // (RT64_CBFD_VTXLIGHT_LOG) for those frames only.
    static int log_room = -1, log_from = 0, log_to = 0;
    static const bool log_parsed = [] {
        if (const char* s = std::getenv("CONKER_LOG_WINDOW")) {
            std::sscanf(s, "%x:%d:%d", &log_room, &log_from, &log_to);
        }
        return true;
    }();
    (void)log_parsed;
    if (log_room >= 0 && rdram != nullptr) {
        const int32_t timer = MEM_W(0, (int32_t)0x800E0A90);
        rt64_cbfd_log_window = (MEM_W(0, (int32_t)0x800BE9F0) == log_room && timer >= log_from && timer <= log_to) ? 1 : 0;
    }
    // CONKER_SNAP_ROOM=hex: the snap times count the room timer (0x800E0A90, refreshes
    // since the room began) in that room instead, to match emulator pictures exactly.
    static const int snap_room = std::getenv("CONKER_SNAP_ROOM") ? (int)std::strtol(std::getenv("CONKER_SNAP_ROOM"), nullptr, 16) : -1;
    if (snap_room >= 0) {
        if (rdram == nullptr || MEM_W(0, (int32_t)0x800BE9F0) != snap_room) {
            return;
        }
        now = (uint32_t)MEM_W(0, (int32_t)0x800E0A90);
    }
    if (now % 60 == 0 && !snap_at.empty()) {
        std::printf("[time] %u\n", now / 60);
    }
    if (next_snap < snap_at.size() && now >= snap_at[next_snap].vi) {
        const Snap& snap = snap_at[next_snap++];
        std::filesystem::path png = snap_dir / ("t" + snap.name + ".png");
        std::printf("[snap] %s\n", snap.name.c_str());
        if (std::getenv("CONKER_RDRAM_DUMP") != nullptr && rdram != nullptr) {
            if (FILE* f = std::fopen((snap_dir / ("t" + snap.name + ".rdram")).string().c_str(), "wb")) {
                std::fwrite(rdram, 1, 8 * 1024 * 1024, f);
                std::fclose(f);
            }
        }
        // Hold the game (it waits for this refresh) until the picture is taken.
        auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!std::filesystem::exists(png) && std::chrono::steady_clock::now() < give_up) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

// CONKER_PROBE=1: how often the game reads the controller and finishes a frame, and how
// old the newest controller read is when a frame is finished (for input delay work).
namespace {
    std::atomic<int64_t> last_poll_us{0};
    std::atomic<uint32_t> polls{0};
    int64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

void conker::testing::on_controller_read() {
    last_poll_us = now_us();
    polls++;
}

extern "C" void conker_probe_pause(uint8_t* rdram, recomp_context* ctx, int tag);

extern "C" void conker_pause_background_tick(uint8_t* rdram); // pause_background.cpp

// Also the pause background's clock (not only testing).
extern "C" void func_1501D348(uint8_t* rdram, recomp_context* ctx); // the game: go to a room

namespace {
    float read_real(uint8_t* rdram, int32_t address) {
        const int32_t bits = MEM_W(0, (gpr)address);
        float value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }

    void write_real(uint8_t* rdram, int32_t address, float value) {
        int32_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        MEM_W(0, (gpr)address) = bits;
    }
}

// Testing aids, for starting a test anywhere in seconds instead of playing up to it:
// CONKER_TEST_WARP=seconds:room:entrance (room in hex) sends Conker to that room and entrance at that
// time, as the game's own exits do (func_1501D348(room, entrance, 0, 0, 0)); several can follow,
// separated by ';'. Then
// CONKER_TEST_PLACE=seconds:x:y:z:facing (facing in 65536ths of a turn) puts him at a spot.
// Several places can follow, separated by ';'. Run at the start of the game's frame.
static void test_travel(uint8_t* rdram, recomp_context* ctx) {
    struct Place { double at; float x, y, z; int facing; bool done; };
    struct Warp { double at; int room, entrance; bool done; };
    static std::vector<Warp> warps;
    static std::vector<Place> places;
    static const bool parsed = [] {
        if (const char* w = std::getenv("CONKER_TEST_WARP")) {
            std::string all(w);
            size_t pos = 0;
            while (pos < all.size()) {
                size_t end = all.find(';', pos);
                Warp warp{ -1.0, 0, 0, false };
                if (std::sscanf(all.substr(pos, end - pos).c_str(), "%lf:%x:%d", &warp.at, &warp.room, &warp.entrance) == 3) {
                    warps.push_back(warp);
                }
                if (end == std::string::npos) break;
                pos = end + 1;
            }
        }
        if (const char* p = std::getenv("CONKER_TEST_PLACE")) {
            std::string all(p);
            size_t pos = 0;
            while (pos < all.size()) {
                size_t end = all.find(';', pos);
                Place place{ -1.0, 0, 0, 0, 0, false };
                if (std::sscanf(all.substr(pos, end - pos).c_str(), "%lf:%f:%f:%f:%d", &place.at, &place.x, &place.y, &place.z, &place.facing) == 5) {
                    places.push_back(place);
                }
                if (end == std::string::npos) break;
                pos = end + 1;
            }
        }
        return true;
    }();
    (void)parsed;
    const double t = conker::testing::game_seconds();
    for (Warp& warp : warps) {
        if (warp.done || t < warp.at) {
            continue;
        }
        warp.done = true;
        recomp_context c = *ctx;
        c.r29 = ctx->r29 - 0x40; // stack room below the frame's
        MEM_W(0x10, c.r29) = 0;
        c.r4 = warp.room; c.r5 = warp.entrance; c.r6 = 0; c.r7 = 0;
        func_1501D348(rdram, &c);
        std::printf("[testing] warp to room %02X entrance %d\n", warp.room, warp.entrance);
        break;
    }
    constexpr int32_t player = (int32_t)0x800CC2D0;
    for (Place& place : places) {
        if (place.done || t < place.at) {
            continue;
        }
        place.done = true;
        write_real(rdram, player + 0x14, place.x);
        write_real(rdram, player + 0x18, place.y);
        write_real(rdram, player + 0x1C, place.z);
        for (int field : { 0x20, 0x3C, 0x44, 0x1F4 }) { // no speed left over
            write_real(rdram, player + field, 0.0f);
        }
        MEM_H(0x76, (gpr)player) = (int16_t)place.facing;
        std::printf("[testing] placed at %.1f %.1f %.1f facing %04X (room %02X)\n", place.x, place.y, place.z,
            place.facing & 0xFFFF, (int)MEM_W(0, (gpr)(int32_t)0x800BE9F0));
    }
}

// Testing aid: CONKER_TEST_CAMERA_FLAGS=seconds:hexbits ORs those bits into the camera's +0x5F0 as
// func_1512D070 (its sway) starts, from then on (0x8: the sway Conker's drunkenness turns on).
extern "C" void conker_test_camera_flags(uint8_t* rdram, recomp_context* ctx) {
    static double at = -1.0; static unsigned bits = 0;
    static const bool parsed = [] {
        if (const char* c = std::getenv("CONKER_TEST_CAMERA_FLAGS")) std::sscanf(c, "%lf:%x", &at, &bits);
        return true;
    }();
    (void)parsed;
    if (at < 0 || conker::testing::game_seconds() < at) {
        return;
    }
    MEM_W(0x5F0, ctx->r4) = MEM_W(0x5F0, ctx->r4) | (int32_t)bits;
}

extern "C" void conker_ledge_grab_watchdog(uint8_t* rdram, recomp_context* ctx); // ledge_grab.cpp

// Testing aid: CONKER_TEST_SINK=seconds:units moves Conker down by that much every game frame from
// then on while he's hanging, climbing up or hopping up (stands in for getting stuck on a ledge, for
// Ledge Grab's safety net).
static void test_sink(uint8_t* rdram) {
    static double at = -1.0; static float units = 0.0f;
    static const bool parsed = [] {
        if (const char* c = std::getenv("CONKER_TEST_SINK")) std::sscanf(c, "%lf:%f", &at, &units);
        return true;
    }();
    (void)parsed;
    if (at < 0 || conker::testing::game_seconds() < at) {
        return;
    }
    constexpr int32_t player = (int32_t)0x800CC2D0;
    const uint16_t anim = (uint16_t)((uint32_t)MEM_W(0, (gpr)(player + 0x84)) >> 16);
    if (anim == 0x42 || anim == 0x40 || anim == 0xEA) {
        write_real(rdram, player + 0x18, read_real(rdram, player + 0x18) - units);
    }
}

// Testing aid: CONKER_TEST_TRACK=1 logs Conker's position and animation, and the camera's eye and
// mode, twice a second.
static void test_track(uint8_t* rdram) {
    static const bool track = std::getenv("CONKER_TEST_TRACK") != nullptr;
    static double next = 0.0;
    const double t = conker::testing::game_seconds();
    if (!track || t < next) {
        return;
    }
    static const double every = std::getenv("CONKER_TEST_TRACK_FAST") != nullptr ? 0.03 : 0.5;
    next = t + every;
    constexpr int32_t player = (int32_t)0x800CC2D0;
    const uint32_t camera = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
    std::printf("[track] %.2fs at %.1f %.1f %.1f anim %04X", t, read_real(rdram, player + 0x14), read_real(rdram, player + 0x18),
        read_real(rdram, player + 0x1C), (unsigned)((uint32_t)MEM_W(0, (gpr)(player + 0x84)) >> 16));
    if ((camera & 0xFF000000u) == 0x80000000u) {
        const int32_t c = (int32_t)camera;
        std::printf(" eye %.1f %.1f %.1f look %.1f %.1f %.1f mode %d", read_real(rdram, c + 0x2F8), read_real(rdram, c + 0x2FC), read_real(rdram, c + 0x300),
            read_real(rdram, c + 0x2BC), read_real(rdram, c + 0x2C0), read_real(rdram, c + 0x2C4), (int)MEM_W(0x2C, (gpr)c));
    }
    std::printf("\n");
}

extern "C" void conker_probe_frame_end(uint8_t* rdram, recomp_context* ctx) {
    test_track(rdram);
    test_travel(rdram, ctx);
    test_sink(rdram);
    conker_ledge_grab_watchdog(rdram, ctx);
    conker_pause_background_tick(rdram);
    conker_probe_pause(rdram, ctx, 0);
    static const bool probe = std::getenv("CONKER_PROBE") != nullptr;
    if (!probe) {
        return;
    }
    static int64_t window_start = now_us();
    static uint32_t frames = 0;
    static int64_t age_sum = 0;
    int64_t now = now_us();
    frames++;
    age_sum += now - last_poll_us.load();
    if (now - window_start >= 2000000) {
        double secs = (now - window_start) / 1e6;
        std::printf("[timing] frames/s %.1f  controller reads/s %.1f  newest read's age at frame end %.1f ms\n",
            frames / secs, polls.exchange(0) / secs, age_sum / 1000.0 / frames);
        window_start = now;
        frames = 0;
        age_sum = 0;
    }
}

// CONKER_PROBE=1: func_1501C730(a0, room, ...) sends the game to a room; func_151E30C4 gets
// the room a save continues in from func_1509CA30.
extern "C" void conker_probe_goto_room(uint8_t* rdram, recomp_context* ctx) {
    if (std::getenv("CONKER_PROBE") != nullptr) {
        std::printf("[goto] a0=%08X a1=%08X a2=%08X a3=%08X\n", (uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7);
    }
}

extern "C" void conker_probe_continue_room(uint8_t* rdram, recomp_context* ctx) {
    if (std::getenv("CONKER_PROBE") != nullptr) {
        std::printf("[continue] room=%08X (asked with a0=%08X)\n", (uint32_t)ctx->r2, (uint32_t)MEM_W(0x28, ctx->r29));
    }
}

// A scripted right stick (frontend.cpp's input script), for test runs of the Free Camera.
namespace {
    std::atomic<float> scripted_rx{0.0f}, scripted_ry{0.0f};
}

void conker::testing::set_right_stick(float x, float y) {
    scripted_rx = x;
    scripted_ry = y;
}

bool conker::testing::right_stick(float* x, float* y) {
    *x = scripted_rx;
    *y = scripted_ry;
    return *x != 0.0f || *y != 0.0f;
}

// CONKER_PROBE_PAUSE=1: what the game does around a pause, frame by frame (which picture the
// pause menu's blurred background is taken from). tag: 0 frame start (func_1501878C),
// 1 func_150198FC (draws the background), 2 func_151D5E90/func_151D6418 (draws a copy of a
// frame, $a1 = the picture), 3 func_151D61B0 (blurs a picture in place, $a0), 4 the scene's
// projection (func_1510B654).
extern "C" void conker_probe_pause(uint8_t* rdram, recomp_context* ctx, int tag) {
    static const bool probe = std::getenv("CONKER_PROBE_PAUSE") != nullptr;
    if (!probe) {
        return;
    }
    static uint32_t frame = 0;
    static int scenes = 0;
    switch (tag) {
    case 0:
        if (MEM_BU(0, (int32_t)0x80082F90) != 0 || MEM_BU(0, (int32_t)0x800BEAC0) != 0 || scenes == 0) {
            std::printf("[pause] frame %u end: scenes %d counter %u flag %u idx %u buf %08X room %X\n", frame, scenes,
                MEM_BU(0, (int32_t)0x80082F90), MEM_BU(0, (int32_t)0x800BEAC0), MEM_BU(0, (int32_t)0x800BE9C0),
                (uint32_t)MEM_W(0, (int32_t)0x800BE9C4), (uint32_t)MEM_W(0, (int32_t)0x800BE9F0));
        }
        frame++;
        scenes = 0;
        break;
    case 1:
        std::printf("[pause] frame %u background: counter %u flag %u byte575 %u idx %u buf %08X\n", frame,
            MEM_BU(0, (int32_t)0x80082F90), MEM_BU(0, (int32_t)0x800BEAC0), MEM_BU(0, (int32_t)0x800BE575),
            MEM_BU(0, (int32_t)0x800BE9C0), (uint32_t)MEM_W(0, (int32_t)0x800BE9C4));
        break;
    case 2:
        std::printf("[pause] frame %u copy drawn from %08X (dl %08X)\n", frame, (uint32_t)ctx->r5, (uint32_t)ctx->r4);
        break;
    case 3:
        std::printf("[pause] frame %u blur %08X\n", frame, (uint32_t)ctx->r4);
        break;
    case 5:
        std::printf("[pause] frame %u Start newly pressed (scenes so far %d)\n", frame, scenes);
        break;
    case 4:
        scenes++;
        if (MEM_BU(0, (int32_t)0x800BEAC0) != 0 || MEM_BU(0, (int32_t)0x80082F90) != 0) {
            std::printf("[pause] frame %u scene drawn: counter %u flag %u idx %u fbs %08X %08X\n", frame,
                MEM_BU(0, (int32_t)0x80082F90), MEM_BU(0, (int32_t)0x800BEAC0), MEM_BU(0, (int32_t)0x800BE9C0),
                (uint32_t)MEM_W(0, (int32_t)0x800BAAEC), (uint32_t)MEM_W(4, (int32_t)0x800BAAEC));
        }
        break;
    }
}

extern "C" void conker_probe_pause_start() {
    conker_probe_pause(nullptr, nullptr, 5);
}
