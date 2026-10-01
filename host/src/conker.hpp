// Conker BFD: Reloaded -- the host program around the recompiled game.
// Structure follows CBFD-Recompiled's host (MIT, Copyright (c) 2026 Sean Ciaschi).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "librecomp/game.hpp"
#include "ultramodern/input.hpp"

namespace recomp::config {
    class Config;
}

struct _SDL_GameController;

namespace conker {
    inline constexpr const char* program_name = "Conker BFD: Reloaded";
    inline constexpr uint64_t us_rom_hash = 0x23FBBA2DBCF2FD8EULL; // XXH3-64 of the US ROM (.z64)

    // segments.cpp: the game's code sections and its TLB-mapped pages.
    void register_code_sections();
    void register_tlb_code();
    void map_tlb_pages(uint8_t* rdram);
    std::vector<uint8_t> unpacked_rom(std::span<const uint8_t> rom);

    // frontend.cpp: window, launcher and menus, input.
    namespace frontend {
        void init(recomp::GameEntry& game);
        void set_callbacks(recomp::Configuration& cfg);
        void on_vi();
        ultramodern::input::connected_device_info_t device_info(int controller);
        // The controllers holding ports 1-4, in port order; returns how many hold one.
        int port_controllers(std::array<_SDL_GameController*, 4>& out);
    }

    // rumble.cpp (CBFD-Recompiled): the Rumble Pak, sent to each port's controller.
    namespace rumble {
        void add_options(recomp::config::Config& config);
        void set(int port, bool on);
        void update();
    }

    // look_aim.cpp (CBFD-Recompiled): gyro and mouse in the look mode (hold R), how each input
    // moves the view, and the camera's turning speed and inversion.
    namespace look_aim {
        void add_options(recomp::config::Config& config);
        void on_input_poll();
    }

    // settings.cpp: the settings tabs.
    void init_settings();
    bool camera_inverted();
    float camera_turn_speed();
    bool skip_intro();
    bool free_camera_enabled();
    bool camera_auto_follow();
    // free_camera.cpp: the right stick for the Free Camera (x right, y up, past the dead zone).
    void free_camera_stick(float* x, float* y);
    // scene_fixes.cpp: Skip Intro's steps.
    void skip_intro_on_vi(uint8_t* rdram);
    bool skip_intro_pressing_start();

    // testing.cpp: fast-forward, game-time screenshots (environment variables).
    namespace testing {
        void init();
        void on_vi(uint8_t* rdram);
        uint32_t speed_multiplier();
        double game_seconds();
        void on_controller_read();
        // A scripted right stick for test runs (CONKER_INPUT_SCRIPT's rx= and ry=); false when none.
        bool right_stick(float* x, float* y);
        void set_right_stick(float x, float y);
    }

    // sound.cpp: SDL sound output.
    namespace sound {
        void queue_samples(int16_t* samples, size_t count);
        size_t frames_remaining();
        void set_frequency(uint32_t frequency);

        // sound_mix.cpp: the Sound tab's Music, Sound Effects and Speech volumes.
        inline const std::string music_volume = "conker_music_volume";
        inline const std::string effects_volume = "conker_effects_volume";
        inline const std::string speech_volume = "conker_speech_volume";
        void add_volume_options();
    }
}
