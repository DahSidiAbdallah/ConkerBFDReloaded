// Conker BFD: Reloaded -- the host program around the recompiled game.
// Structure follows CBFD-Recompiled's host (MIT, Copyright (c) 2026 Sean Ciaschi).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
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
    // The part of the ROM holding the game's code (.init, .game, .debugger: 0x1000 to the end of
    // .debugger), and its XXH3-64 in the US ROM. ROM hacks that leave it alone (only the game's
    // data changed, like the uncensored speech) work with the recompiled code.
    inline constexpr uint32_t rom_code_start = 0x1000, rom_code_end = 0x25A5D8;
    inline constexpr uint64_t us_code_hash = 0x55ED54C589E9E0D5ULL;
    // Inside that part sits .game's compressed data (0x188328 to 0x19C7D8: its strings, such as the
    // multiplayer "lap"), which a translation can change while the code stays the same. So the code
    // around it must match (XXH3-64 of both sides together), and the block must be the US ROM's or
    // a known translation's: the French patch by Corrigo and Djipi (v1.3), which only changes "lap".
    inline constexpr uint32_t rom_game_data_start = 0x188328, rom_game_data_end = 0x19C7D8;
    inline constexpr uint64_t us_code_around_game_data_hash = 0xC5617F3F5DBB4DF9ULL;
    inline constexpr uint64_t known_game_data_hashes[] = {
        0x7AC0A772526269F5ULL, // the US ROM
        0x4238B3E2AF4E2E79ULL, // the French translation
    };
    inline constexpr uint64_t french_rom_hash = 0xF78F2024E0B8F2A3ULL; // the French translation (v1.3)

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
    bool camera_tilt_inverted();
    float camera_turn_speed();
    float camera_field_of_view();
    // free_camera.cpp: whether player 1's game camera was the normal follow camera last frame (not
    // R-Look, aiming, cutscenes or another special camera).
    bool normal_camera();
    // field_of_view.cpp (CBFD-Recompiled V0.1.5): Camera: Field of View's part of the culls.
    namespace field_of_view {
        void adjust_cull_scales(uint8_t* rdram, uint64_t camera_address);
    }
    bool skip_intro();
    bool free_camera_enabled();
    bool free_camera_mouse(); // Free Camera: Right Stick and Mouse

    // Button Prompts (General tab; key_prompts.cpp): which the speech bubbles' button pictures show.
    enum class ButtonPrompts : uint32_t { Automatic, Controller, Keyboard };
    ButtonPrompts button_prompts();
    void key_prompts_init();   // once SDL is up
    void key_prompts_update(); // now and then, from the UI thread
    void key_prompts_write_pictures(const std::filesystem::path& dir); // testing
    // free_camera.cpp: the right stick for the Free Camera (x right, y up, past the dead zone).
    void free_camera_stick(float* x, float* y);
    // scene_fixes.cpp: Skip Intro's steps.
    void skip_intro_on_vi(uint8_t* rdram);
    bool skip_intro_pressing_start();

    // pad_mappings.cpp: N64 pads whose C-buttons SDL maps as face buttons get them as the right stick
    // (from CBFD-Recompiled V0.1.5, their issue #28).
    namespace pad_mappings {
        // Rewrites the mapping of each such controller connected.
        void fix_all();
        // From SDL's event watch: a controller was connected (fixed at the next update).
        void on_device_added();
        // Every VI: fixes the controllers connected since the last one.
        void update();
    }

    // crosshair.cpp: Aiming Crosshair (Conker tab).
    namespace crosshair {
        // settings.cpp: whether the setting is on.
        bool enabled();
        // look_aim.cpp, each frame of the second aiming mode (player 1): zoomed in (the sniper scope) or not.
        void aiming(bool zoomed);
        // look_aim.cpp, each frame of the look mode (player 1): shown unless the player holds R.
        // vertical_fov: the camera's vertical field of view in use, in degrees.
        void look_mode(float vertical_fov, uint8_t kind, uint32_t room);
        // frontend.cpp: player 1's buttons this frame (R held or not).
        void set_buttons(uint16_t buttons);
        // From the launcher's init (frontend.cpp): recompui's UI exists now.
        void on_ui_ready();
        // On the main thread (update_gfx): shows or hides it.
        void update();
    }

    // qol.cpp: the Conker tab's quality-of-life options.
    namespace qol {
        void init();          // frontend.cpp, once SDL is up
        void on_ui_ready();   // frontend.cpp, from the launcher's init
        void update();        // main thread (update_gfx): the Autosave Icon
        void on_vi();         // every VI: Pause When Unfocused holds the game here
        void set_player_buttons(uint16_t buttons); // player 1's own buttons (Skip Any Cutscene's hold)
        uint16_t apply_toggles(uint16_t buttons); // player 1's buttons: Toggle R-Look / Crouch
        void apply_walk(uint16_t buttons, float* x, float* y); // player 1's stick: Walk Button
        void apply_swim(float* y); // player 1's stick: Invert Swimming
        int walk_button(); // 0 off, 1 hold L, 2 toggle L
        bool invert_swimming();
        int longer_breath(); // 0 original, 1 twice, 2 four times
        bool air_meter();
        // settings.cpp: the options.
        bool saving_icon();
        bool pause_unfocused();
        bool skip_any_cutscene();
        bool toggle_r_look();
        bool toggle_crouch();
        bool reduce_motion();
        bool always_show_hud();
        int cash_counter(); // 0 off, 1 when it changes, 2 always (cash_hud.cpp)
        bool ledge_grab();  // ledge_grab.cpp
        bool longer_spin();
        bool cutscene_playing(); // the game's cutscene skip check ran in the last quarter second
        void longer_spin_on_vi(uint8_t* rdram);
        void on_vi_memory(uint8_t* rdram); // every VI, with the game's memory: Always Show HUD
    }

    // fps_counter.cpp: Show FPS, the frame rate counter (from CBFD-Recompiled V0.1.5).
    namespace fps_counter {
        // settings.cpp: whether the setting is on.
        bool enabled();
        // From the launcher's init (frontend.cpp): recompui's UI exists now, so the counter can be made.
        void on_ui_ready();
        // On the main thread (update_gfx): shows or hides the counter, and updates it.
        void update();
        // From the game thread, as the game starts a frame's display list.
        void game_frame();
    }

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
