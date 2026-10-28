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
        // Aiming: Swap Sticks: whether a player (0 to 3) is aiming now with the setting on, so their controller's
        // sticks are swapped (frontend.cpp): the right stick aims and the left moves.
        bool swap_sticks_now(int player = 0);
        // Whether the player's character is aiming the shotgun, where Z makes C-Left and C-Right turn the aim.
        bool z_turns_aim(int player);
        // Aiming: Move While Shooting (move_shoot.cpp).
        bool move_while_shooting();
        // Aiming: Face Where the Camera Looks (free_camera.cpp).
        bool face_camera();
    }

    // settings.cpp: the settings tabs.
    void init_settings();
    bool camera_inverted();
    bool camera_tilt_inverted();
    float camera_turn_speed();
    bool camera_turn_direct();
    float camera_field_of_view();
    // free_camera.cpp: whether player 1's game camera was the normal follow camera last frame (not
    // R-Look, aiming, cutscenes or another special camera).
    bool normal_camera();
    // field_of_view.cpp (CBFD-Recompiled V0.1.5): Camera: Field of View's part of the culls.
    namespace field_of_view {
        void adjust_cull_scales(uint8_t* rdram, uint64_t camera_address);
    }
    bool skip_intro();
    uint32_t launcher_poster(); // Settings > Conker > Launcher Poster: 0..8 a poster, 9 Random
    bool free_camera_enabled();
    bool free_camera_mouse(); // Free Camera: Right Stick and Mouse

    // Button Prompts (General tab; key_prompts.cpp): which the speech bubbles' button pictures show.
    enum class ButtonPrompts : uint32_t { Automatic, Controller, Keyboard };
    ButtonPrompts button_prompts();
    void key_prompts_init();   // once SDL is up
    void key_prompts_update(); // now and then, from the UI thread
    void key_prompts_write_pictures(const std::filesystem::path& dir); // testing
    bool keyboard_prompts_in_use(); // the prompts show keys and mouse buttons now (else the controller's)
    // free_camera.cpp: the right stick for the Free Camera (x right, y up, past the dead zone).
    void free_camera_stick(float* x, float* y);
    // free_camera.cpp: testing (F9 with capture_enabled): the free camera's last frames, as text.
    void free_camera_write_history(const std::filesystem::path& path);
    // scene_fixes.cpp: the last room changes and button presses (an F9 capture writes them, events_<n>.txt), to
    // see what led to a rare state (the game over that leaves Conker controllable in the throne room).
    void events_note(const char* what, uint32_t a, uint32_t b);
    void events_write(const std::filesystem::path& path);
    // scene_fixes.cpp: Skip Intro's steps.
    void skip_intro_on_vi(uint8_t* rdram);
    void skip_intro_cancel(); // a save state was loaded: lift Skip Intro's black screen
    bool skip_intro_pressing_start();

    // pad_mappings.cpp: N64 pads whose C-buttons SDL maps as face buttons get them as the right stick
    // (from CBFD-Recompiled V0.1.5, their issue #28).
    namespace pad_mappings {
        // Rewrites the mapping of each such controller connected.
        void fix_all();
        // From SDL's event watch: a controller was connected (fixed at the next update).
        void on_device_added();
        // Main thread (update_gfx), each frame: fixes the controllers connected since the last one.
        void update();
    }

    // photo_mode.cpp: Photo Mode (Conker tab).
    namespace photo {
        bool enabled(); // settings.cpp: the setting
        bool active();
        void init(); // frontend.cpp, once SDL is up
        // frontend.cpp's get_input, player 1: keeps the buttons for the camera; the game gets none while it's open.
        void filter_input(uint16_t* buttons, float* x, float* y);
        // free_camera.cpp (conker_mouse_camera_view): true when the view's eye and look-at point are Photo Mode's.
        bool view(uint8_t* rdram, recomp_context* ctx);
        int lens_degrees(); // field_of_view.cpp: Photo Mode's lens (vertical degrees), 0 the game's
        void on_ui_ready(); // frontend.cpp, as fps_counter's
        void update_ui();   // main thread (update_gfx): the hint along the bottom
    }

    // crosshair.cpp: Aiming Crosshair (Conker tab).
    namespace crosshair {
        // settings.cpp: whether the setting is on.
        bool enabled();
        // look_aim.cpp, each frame of the second aiming mode (player 1): zoomed in (the sniper scope) or not.
        void aiming(bool zoomed);
        // look_aim.cpp, each frame of the look mode (player 1), with its kind (the camera's +0x23E): shown for the shotgun.
        void look_mode(uint8_t kind);
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
        // Player 1 after scripted input, camera/photo filtering and intro input; before walk/toggles/skip presses.
        uint16_t get_player_buttons();
        uint16_t apply_toggles(uint16_t buttons); // player 1's buttons: Toggle R-Look / Crouch
        uint16_t apply_skip_press(uint16_t buttons); // player 1's buttons: Hold to Skip's press in the game over
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
    // Graphics: Conker's Detail (settings.cpp; the model is picked in model_detail.cpp).
    namespace model_detail {
        // Always Highest: Conker stays on his most detailed model in single player.
        bool always_highest();
    }

    // widescreen.cpp: the motion blur is drawn this frame (qol.cpp), so no frames are drawn between it and the next.
    void motion_blur_drawn();

    // interpolation.cpp: shadows aren't drawn on their first game frame back after a gap.
    namespace shadows {
        // From the game thread, as the game starts a frame's display list.
        void game_frame();
    }

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

    // save_states.cpp: save states, a testing aid only (a savestates_enabled file in the settings folder, or
    // CONKER_TEST_STATE_SAVE / CONKER_TEST_STATE_LOAD).
    namespace save_states {
        void init();  // at start (frontend.cpp), once SDL is up
        void on_vi(); // every VI (main.cpp): a test run's save or load
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

        // sound_mix.cpp: each screen refresh, a changed Speech volume reaches a line already playing.
        void refresh_stream_volume(uint8_t* rdram);
        // sound_mix.cpp: the Sound tab's Music, Sound Effects and Speech volumes.
        inline const std::string music_volume = "conker_music_volume";
        inline const std::string effects_volume = "conker_effects_volume";
        inline const std::string speech_volume = "conker_speech_volume";
        void add_volume_options();
    }
}
