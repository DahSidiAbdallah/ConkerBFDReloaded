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
    // CONKER_TEST_CASH=seconds: adds $1 to player 1's cash then (0x800D2148, as the CBFD cheats
    // mod), to make the game show its HUD (finding the HUD's timer).
    static const double test_cash_at = std::getenv("CONKER_TEST_CASH") ? std::atof(std::getenv("CONKER_TEST_CASH")) : -1.0;
    if (test_cash_at > 0 && rdram != nullptr && now == (uint32_t)(test_cash_at * 60.0)) {
        MEM_W(0, (int32_t)0x800D2148) = MEM_W(0, (int32_t)0x800D2148) + 1;
        std::printf("[testing] cash +1\n");
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
extern "C" void conker_probe_frame_end(uint8_t* rdram, recomp_context* ctx) {
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
