// The window, the launcher and menus (RecompFrontend's recompui) and remappable
// input (recompinput). Launcher design: a full-screen collage of the game's moments with the game's logo
// over the middle, put together as the launcher opens (assets/poster, add_posters), and the menu on the
// right, in orange and cream.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <map>
#include <random>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#ifdef _WIN32
#   include <SDL_syswm.h> // (on Linux it brings X11's macros, e.g. None, which clash with the runtime's names)
#endif

#include "nfd.h"

#include "librecomp/game.hpp"
#include "recompinput/input_events.h"
#include "recompinput/input_state.h"
#include "recompinput/players.h"
#include "recompinput/profiles.h"
#include "recompui/config.h"
#include "recompui/program_config.h"
#include "recompui/recompui.h"
#include "recompui/renderer.h"
#include "base/ui_launcher.h"
#include "elements/ui_image.h"
#include "util/file.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "conker.hpp"

// recompui's launcher shows the first entry; ui_launcher.cpp declares it extern.
std::vector<recomp::GameEntry> supported_games;
// The game window, which recompui also uses (ui_state.cpp declares it extern).
SDL_Window* window = nullptr;

void conker_mouse_camera_init();
extern "C" void conker_probe_pause_start(); // testing.cpp
void conker_pause_background_start_pressed(); // pause_background.cpp

namespace {
    // Each poll also tells the look mode that the next mouse and gyro movement is in (look_aim.cpp).
    void poll_inputs() {
        recompinput::poll_inputs();
        conker::look_aim::on_input_poll();
    }
}

namespace {
    // Luckiest Guy (Apache License 2.0, assets/LuckiestGuy-LICENSE.txt).
    constexpr const char* title_font = "Luckiest Guy";

    const recompui::Color cream{ 0xFF, 0xE9, 0xC2, 0xFF };

    // The launcher's poster: a collage of the game's moments (real N64 frames) in slanted panels, in the
    // style of the GTA covers, with the game's own logo over the middle (assets/poster: one picture a
    // panel, cut to its shape, and layout.txt with where each goes on the 2560x1440 poster, the logo last).
    // As the launcher opens the panels fly in one after another from outside, then the logo lands on top,
    // then the menu's buttons slide in (any key, click or controller button skips to the end). Other posters
    // can be picked in Settings > Conker > Launcher Poster: poster_folders, laid out the same way.
    struct PosterPiece {
        recompui::Image* image = nullptr;
        float dx = 0.0f, dy = 0.0f;   // the way it flies in from (a part of its own size)
        float delay = 0.0f;           // seconds after the launcher shows
        bool logo = false;
        bool shade = false;           // the shade behind the menu: only fades in, with the logo
    };
    std::vector<PosterPiece> poster_pieces;
    bool poster_started = false, poster_done = false;
    std::chrono::steady_clock::time_point poster_start;
    constexpr float panel_seconds = 0.55f, panel_step = 0.07f, panels_after = 0.05f;
    constexpr float logo_seconds = 0.85f, logo_after = 0.35f; // after the last panel starts

    // The choices of Launcher Poster (settings.cpp) in order; the one after them, Random, picks one each start.
    // (tools/launcher_art: hero_posters.py makes king..berri, poster_previews.py their pictures for the setting.)
    constexpr const char* poster_folders[] = { "poster", "poster_old", "poster_king", "poster_chainsaw", "poster_dollar",
        "poster_panther", "poster_three", "poster_gregg", "poster_berri" };
    recompui::Element* poster_holder = nullptr;
    uint32_t poster_choice = 0;   // the setting the poster shown follows
    size_t poster_shown = SIZE_MAX;
    std::atomic<bool> intro_skip{ false };
    bool buttons_shown = false;
    std::chrono::steady_clock::time_point buttons_start;
    constexpr float button_seconds = 0.45f, button_step = 0.07f;

    void build_poster(const std::string& folder) {
        recompui::ContextId context = recompui::get_current_context();
        recompui::Element* parent = poster_holder;
        std::ifstream layout(recompui::file::get_asset_path((folder + "/layout.txt").c_str()));
        float poster_w = 0.0f, poster_h = 0.0f;
        if (!(layout >> poster_w >> poster_h) || poster_w <= 0.0f || poster_h <= 0.0f) {
            std::printf("[frontend] no poster layout\n");
            return;
        }
        auto load = [&](const std::string& file, const std::string& src) {
            std::ifstream in(recompui::file::get_asset_path((file.find('/') == std::string::npos ? folder + "/" + file : file).c_str()), std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!bytes.empty()) {
                recompui::queue_image_from_bytes_file(src, bytes);
            }
            return !bytes.empty();
        };
        // Covers the window's width, centred (a 16:9 poster): a clear 16:9 picture gives the frame its height.
        recompui::Element* frame = context.create_element<recompui::Element>(parent);
        frame->set_position(recompui::Position::Absolute);
        frame->set_top(50.0f, recompui::Unit::Percent);
        frame->set_left(0);
        frame->set_width(100.0f, recompui::Unit::Percent);
        frame->set_height_auto();
        frame->set_translate_2D(0.0f, -50.0f, recompui::Unit::Percent);
        if (load("poster/sizer.png", "?/conker/poster_sizer")) {
            recompui::Image* sizer = context.create_element<recompui::Image>(frame, "?/conker/poster_sizer");
            sizer->set_display(recompui::Display::Block);
            sizer->set_width(100.0f, recompui::Unit::Percent);
            sizer->set_height_auto();
        }
        poster_pieces.clear();
        std::string file, name;
        float x, y, w, h;
        int panels = 0;
        while (layout >> file >> x >> y >> w >> h >> name) {
            const std::string src = "?/conker/" + folder + "_" + name;
            if (!load(file, src)) {
                continue;
            }
            recompui::Image* image = context.create_element<recompui::Image>(frame, src);
            image->set_position(recompui::Position::Absolute);
            image->set_left(100.0f * x / poster_w, recompui::Unit::Percent);
            image->set_top(100.0f * y / poster_h, recompui::Unit::Percent);
            image->set_width(100.0f * w / poster_w, recompui::Unit::Percent);
            image->set_height(100.0f * h / poster_h, recompui::Unit::Percent);
            if (name == "base") {
                // A blurred, dark copy of the poster, there from the first frame (no black wait).
                continue;
            }
            image->set_opacity(0.0f);
            PosterPiece piece;
            piece.image = image;
            piece.logo = name == "logo";
            piece.shade = name == "shade";
            if (!piece.logo && !piece.shade) {
                // Out from the poster's middle toward where it sits.
                const float cx = (x + w * 0.5f) / poster_w - 0.5f, cy = (y + h * 0.5f) / poster_h - 0.5f;
                const float len = std::max(0.05f, std::sqrt(cx * cx + cy * cy));
                piece.dx = cx / len * 0.45f;
                piece.dy = cy / len * 0.45f;
                piece.delay = panels_after + panels * panel_step;
                panels++;
            }
            poster_pieces.push_back(piece);
        }
        for (PosterPiece& piece : poster_pieces) {
            if (piece.logo || piece.shade) {
                piece.delay = panels_after + std::max(0, panels - 1) * panel_step + logo_after;
            }
        }
        poster_started = false;
        poster_done = poster_pieces.empty();
    }

    // Shows the poster a Launcher Poster choice picks (Random: a different one than now).
    void show_poster(uint32_t choice) {
        size_t index = choice;
        if (index >= std::size(poster_folders)) {
            static std::mt19937 random{ std::random_device{}() };
            do {
                index = random() % std::size(poster_folders);
            } while (index == poster_shown);
        }
        poster_holder->clear_children();
        poster_pieces.clear();
        build_poster(poster_folders[index]);
        poster_shown = index;
    }

    // Any key, click or controller button skips the launcher's opening to the end.
    int SDLCALL watch_intro_skip(void*, SDL_Event* event) {
        switch (event->type) {
        case SDL_KEYDOWN:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_CONTROLLERBUTTONDOWN:
            intro_skip = true;
            break;
        }
        return 1;
    }

    void add_posters(recompui::LauncherMenu* menu) {
        poster_holder = recompui::get_current_context().create_element<recompui::Element>(menu->get_background_container());
        poster_holder->set_position(recompui::Position::Absolute);
        poster_holder->set_top(0);
        poster_holder->set_left(0);
        poster_holder->set_width(100.0f, recompui::Unit::Percent);
        poster_holder->set_height(100.0f, recompui::Unit::Percent);
        poster_choice = conker::launcher_poster();
        show_poster(poster_choice);
        SDL_AddEventWatch(watch_intro_skip, nullptr);
    }

    float ease_out_cubic(float t) { const float u = 1.0f - t; return 1.0f - u * u * u; }
    float ease_out_back(float t) { const float c = 1.7f, u = t - 1.0f; return 1.0f + (c + 1.0f) * u * u * u + c * u * u; }

    // The launcher's buttons in the game's own menu style (the pause and save menus' pill rings): each
    // option's words are a picture (assets/menu, made by tools/launcher_art/make_buttons.py), lit (an
    // orange-red ring) while pointed at or selected, a dark steel ring otherwise. The option's own text is
    // hidden but kept, so it still works the same way.
    struct MenuButton {
        recompui::GameOption* option = nullptr;
        recompui::Image* on = nullptr;
        recompui::Image* off = nullptr;
        std::string name;   // which picture it shows now
        bool lit = false;
    };
    std::vector<MenuButton> menu_buttons;
    std::string rom_button_name; // follows the ROM option's title (rom_title)
    std::filesystem::path stored_rom_path(const std::u8string& game_id);
    std::u8string launcher_game_id;

    // "ROM: Original" -> "rom_original", "Change ROM" -> "change_rom".
    std::string button_name(const std::string& title) {
        std::string name;
        for (char c : title) {
            if (std::isalnum((unsigned char)c)) {
                name += (char)std::tolower((unsigned char)c);
            } else if (c == ' ' && !name.empty() && name.back() != '_') {
                name += '_';
            }
        }
        return name;
    }

    // The pictures' height on the 2560x1440 poster, read from the PNG header; shown at the poster's scale
    // (recompui's dp is a 1080th of the window's height).
    float picture_height(const std::vector<char>& png) {
        if (png.size() < 24) {
            return 0.0f;
        }
        const auto byte = [&](size_t i) { return (uint32_t)(uint8_t)png[i]; };
        return (float)(byte(20) << 24 | byte(21) << 16 | byte(22) << 8 | byte(23));
    }
    std::map<std::string, float> picture_heights;

    void load_button_pictures() {
        for (const char* name : { "start_game", "load_rom", "controls", "settings", "mods", "exit", "change_rom", "rom_original",
                 "rom_uncensored", "rom_french", "rom_german", "rom_russian", "rom_modified" }) {
            for (const char* state : { "on", "off" }) {
                const std::string file = std::string(name) + "_" + state + ".png";
                std::ifstream in(recompui::file::get_asset_path(("menu/" + file).c_str()), std::ios::binary);
                std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (!bytes.empty()) {
                    picture_heights[std::string(name) + "_" + state] = picture_height(bytes);
                    recompui::queue_image_from_bytes_file("?/conker/menu_" + std::string(name) + "_" + state, bytes);
                }
            }
        }
    }

    void show_button_picture(MenuButton& button, const std::string& name) {
        if (button.name == name || !picture_heights.contains(name + "_on")) {
            return;
        }
        button.name = name;
        constexpr float poster_to_dp = 1080.0f / 1440.0f;
        button.on->set_src("?/conker/menu_" + name + "_on");
        button.off->set_src("?/conker/menu_" + name + "_off");
        button.on->set_height(picture_heights[name + "_on"] * poster_to_dp, recompui::Unit::Dp);
        button.off->set_height(picture_heights[name + "_off"] * poster_to_dp, recompui::Unit::Dp);
    }

    void add_button_picture(recompui::GameOption* option, const std::string& name) {
        recompui::ContextId context = recompui::get_current_context();
        MenuButton button;
        button.option = option;
        option->get_label()->set_display(recompui::Display::None);
        option->set_padding(0.0f);
        option->set_margin_top(-20.0f); // the rings overlap a little, as on the poster
        option->hover_style.set_background_color(recompui::theme::color::Transparent);
        option->focus_style.set_background_color(recompui::theme::color::Transparent);
        button.on = context.create_element<recompui::Image>(option, "");
        button.off = context.create_element<recompui::Image>(option, "");
        for (recompui::Image* image : { button.on, button.off }) {
            image->set_width_auto();
        }
        button.on->set_display(recompui::Display::None);
        // Hidden until the poster is up (update_buttons_intro).
        option->set_opacity(0.0f);
        option->set_enabled(false);
        show_button_picture(button, name);
        menu_buttons.push_back(button);
    }

    void update_menu_buttons() {
        for (size_t i = 0; i < menu_buttons.size(); i++) {
            MenuButton& button = menu_buttons[i];
            if (i == 0) { // Start Game until there's a ROM to start: Load ROM
                show_button_picture(button, std::filesystem::exists(stored_rom_path(launcher_game_id)) ? "start_game" : "load_rom");
            } else if (button.name.starts_with("rom_") || button.name == "change_rom") {
                show_button_picture(button, rom_button_name);
            }
            const bool lit = button.option->is_style_enabled(recompui::hover_state) || button.option->is_style_enabled(recompui::focus_state);
            if (lit != button.lit) {
                button.lit = lit;
                button.on->set_display(lit ? recompui::Display::Block : recompui::Display::None);
                button.off->set_display(lit ? recompui::Display::None : recompui::Display::Block);
            }
        }
    }

    // The menu's buttons slide in from the right one after another once the poster is up, and work from then on.
    void update_buttons_intro(std::chrono::steady_clock::time_point now) {
        if (intro_skip) {
            buttons_start = now - std::chrono::seconds(60);
        }
        const float seconds = std::chrono::duration<float>(now - buttons_start).count();
        bool all_done = true;
        for (size_t i = 0; i < menu_buttons.size(); i++) {
            const float t = std::clamp((seconds - i * button_step) / button_seconds, 0.0f, 1.0f);
            const float e = ease_out_cubic(t);
            menu_buttons[i].option->set_opacity(e);
            menu_buttons[i].option->set_translate_2D(80.0f * (1.0f - e), 0.0f, recompui::Unit::Dp);
            all_done = all_done && t >= 1.0f;
        }
        if (all_done) {
            for (MenuButton& button : menu_buttons) {
                button.option->set_enabled(true);
            }
            buttons_shown = true;
            SDL_DelEventWatch(watch_intro_skip, nullptr);
            // Testing aid: CONKER_TEST_SETTINGS=<tab id> opens the settings on that tab once the launcher is up,
            // for screenshots without clicking.
            if (const char* tab = std::getenv("CONKER_TEST_SETTINGS")) {
                recompui::config::open();
                recompui::config::set_tab(tab);
            }
        }
    }

    void update_launcher(recompui::LauncherMenu*) {
        update_menu_buttons();
        const auto now = std::chrono::steady_clock::now();
        // A new Launcher Poster shows at once (and plays its opening).
        const uint32_t choice = conker::launcher_poster();
        if (choice != poster_choice) {
            poster_choice = choice;
            show_poster(choice);
        }
        if (poster_done) {
            if (!buttons_shown) {
                update_buttons_intro(now);
            }
            return;
        }
        if (!poster_started) {
            poster_started = true; // from the first frame shown, not while it loaded
            poster_start = now;
        }
        if (!buttons_shown) {
            // (recompui shows the options itself as they're set up: kept hidden until the poster is up.)
            for (MenuButton& button : menu_buttons) {
                button.option->set_opacity(0.0f);
            }
            if (intro_skip) {
                poster_start = now - std::chrono::seconds(60);
            }
        }
        const float seconds = std::chrono::duration<float>(now - poster_start).count();
        bool all_done = true;
        for (PosterPiece& piece : poster_pieces) {
            const float length = piece.logo ? logo_seconds : panel_seconds;
            const float t = std::clamp((seconds - piece.delay) / length, 0.0f, 1.0f);
            all_done = all_done && t >= 1.0f;
            if (piece.shade) {
                piece.image->set_opacity(t);
            } else if (piece.logo) {
                // Lands from big, a little turned, with a small overshoot.
                const float e = ease_out_back(t);
                const float scale = 1.0f + 0.9f * (1.0f - e);
                piece.image->set_opacity(std::clamp(t * 2.5f, 0.0f, 1.0f));
                piece.image->set_scale_2D(scale, scale);
                piece.image->set_rotation(-10.0f * (1.0f - e));
            } else {
                const float e = ease_out_cubic(t);
                piece.image->set_opacity(std::clamp(t * 1.6f, 0.0f, 1.0f));
                piece.image->set_translate_2D(100.0f * piece.dx * (1.0f - e), 100.0f * piece.dy * (1.0f - e), recompui::Unit::Percent);
                const float scale = 1.0f + 0.12f * (1.0f - e);
                piece.image->set_scale_2D(scale, scale);
            }
        }
        poster_done = all_done;
        if (poster_done && !buttons_shown) {
            buttons_start = now;
        }
    }

    // Prints what SDL says about a controller (name, GUID, mapping, device), for controller reports.
    // (From CBFD-Recompiled V0.1.5, as the two below.)
    void print_controller(int index) {
        char guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
        const char* name = SDL_GameControllerNameForIndex(index);
        const char* path = SDL_GameControllerPathForIndex(index);
        char* mapping = SDL_GameControllerMappingForDeviceIndex(index);
        std::printf("[controller] connected: %s (GUID %s, device %s)\n  mapping: %s\n", name ? name : "?", guid,
            path ? path : "?", mapping ? mapping : "none");
        SDL_free(mapping);
    }

    // Watches controllers connecting: each is printed, and its C-buttons remapped if they need it
    // (pad_mappings.cpp).
    int SDLCALL watch_controllers(void*, SDL_Event* event) {
        switch (event->type) {
        case SDL_JOYDEVICEADDED:
            conker::pad_mappings::on_device_added();
            break;
        case SDL_CONTROLLERDEVICEADDED:
            print_controller(event->cdevice.which);
            break;
        }
        std::fflush(stdout);
        return 1;
    }

    void* create_gfx() {
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#if defined(__linux__)
        // Nintendo's online classic controllers (the N64 one, and pads like the 8BitDo 64 in its
        // Switch mode) through the kernel's driver, not SDL's HIDAPI one, so there's one layout for
        // pad_mappings.cpp to make the C-buttons the right stick of: SDL3, under Linux distributions'
        // sdl2-compat, maps both as a Switch pad, and HIDAPI's puts one C-button on an axis. Windows
        // keeps SDL's default. The SDL_JOYSTICK_HIDAPI_NINTENDO_CLASSIC environment variable still
        // overrides this.
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_NINTENDO_CLASSIC, "0");
#endif
        Uint32 subsystems = SDL_INIT_VIDEO;
        // CONKER_NO_CONTROLLER=1 ignores controllers (test runs while someone plays).
        if (SDL_getenv("CONKER_NO_CONTROLLER") == nullptr) {
            subsystems |= SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC;
        }
        if (SDL_Init(subsystems) != 0) {
            std::fprintf(stderr, "[frontend] SDL_Init failed: %s\n", SDL_GetError());
        }
        SDL_version sdl_version;
        SDL_GetVersion(&sdl_version);
        std::printf("[frontend] SDL %d.%d.%d\n", sdl_version.major, sdl_version.minor, sdl_version.patch);
        // N64 pads and adapters SDL has no mapping for, or maps as other pads (assets/controllerdb.txt,
        // from CBFD-Recompiled V0.1.5). Only mappings, so the controllers connected at start are
        // picked up when they're opened.
        const std::u8string controller_db = recompui::file::get_asset_path("controllerdb.txt").u8string();
        const int controller_mappings = SDL_GameControllerAddMappingsFromFile(reinterpret_cast<const char*>(controller_db.c_str()));
        if (controller_mappings < 0) {
            std::fprintf(stderr, "[frontend] couldn't load the controller mappings: %s\n", SDL_GetError());
        } else {
            std::printf("[frontend] controller mappings: %d added\n", controller_mappings);
        }
        SDL_AddEventWatch(watch_controllers, nullptr);
        // The controllers connected at start were announced before the watch.
        for (int index = 0; index < SDL_NumJoysticks(); index++) {
            print_controller(index);
        }
        conker::pad_mappings::fix_all();
        NFD_Init(); // file dialogs (Load ROM, mods)
        conker_mouse_camera_init(); // the Free Camera's scroll wheel zoom (free_camera.cpp)
        conker::photo::init(); // Photo Mode's P key and picture (photo_mode.cpp)
        conker::save_states::init(); // save states, for testing only (save_states.cpp)
        conker::key_prompts_init(); // Button Prompts: which device is in use (key_prompts.cpp)
        conker::qol::init(); // Pause When Unfocused watches the window's focus
        return nullptr;
    }

    ultramodern::renderer::WindowHandle create_window(void*) {
#ifdef _WIN32
        const Uint32 flags = SDL_WINDOW_RESIZABLE;
#else
        const Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN; // RT64 draws with Vulkan through SDL
#endif
        window = SDL_CreateWindow(conker::program_name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 900, flags);
        if (window == nullptr) {
            std::fprintf(stderr, "[frontend] SDL_CreateWindow failed: %s\n", SDL_GetError());
            return {};
        }
#ifdef _WIN32
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        SDL_GetWindowWMInfo(window, &info);
        return ultramodern::renderer::WindowHandle{ info.info.win.window, GetCurrentThreadId() };
#else
        return window;
#endif
    }

    void refresh_connected_controllers();

    void update_gfx(void*) {
        recompinput::handle_events();
        // Both on the main thread, where SDL's device-index functions are used (see connected_ids).
        conker::pad_mappings::update();
        refresh_connected_controllers();
        conker::fps_counter::update();
        conker::photo::update_ui();
        conker::crosshair::update();
        conker::qol::update();
    }

    // The settings' backdrop: the poster, blurred and darkened (tools/launcher_art/make_settings_bg.py).
    void add_settings_backdrop() {
        std::ifstream in(recompui::file::get_asset_path("menu/settings_bg.png"), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!bytes.empty()) {
            recompui::queue_image_from_bytes_file("?/conker/settings_bg", bytes);
            recompui::config::set_backdrop("?/conker/settings_bg");
        }
        // Launcher Poster's pictures, for its description (settings.cpp launcher_poster_about).
        for (size_t i = 0; i < std::size(poster_folders); i++) {
            const std::string name = "poster_preview_" + std::to_string(i);
            std::ifstream preview(recompui::file::get_asset_path(("menu/" + name + ".png").c_str()), std::ios::binary);
            std::vector<char> preview_bytes((std::istreambuf_iterator<char>(preview)), std::istreambuf_iterator<char>());
            if (!preview_bytes.empty()) {
                recompui::queue_image_from_bytes_file("?/conker/" + name, preview_bytes);
            }
        }
    }

    // Conker's menu colours for the launcher and every menu (recompui's default is blue): orange-red for
    // what is chosen, gold letters (the settings' own pieces: RecompFrontend's ui_conker_look.h).
    void apply_theme() {
        using recompui::theme::color;
        auto set = recompui::theme::set_theme_color;
        set(color::Primary, { 0xEE, 0x4E, 0x14, 0xFF });
        set(color::PrimaryL, { 0xFF, 0x93, 0x2B, 0xFF });
        set(color::PrimaryD, { 0xA8, 0x30, 0x08, 0xFF });
        set(color::PrimaryA5, { 0xEE, 0x4E, 0x14, 0x0D });
        set(color::PrimaryA20, { 0xEE, 0x4E, 0x14, 0x33 });
        set(color::PrimaryA30, { 0xEE, 0x4E, 0x14, 0x4D });
        set(color::PrimaryA50, { 0xEE, 0x4E, 0x14, 0x80 });
        set(color::PrimaryA80, { 0xEE, 0x4E, 0x14, 0xCC });
        set(color::TextActive, cream);
        set(color::Text, { 0xF0, 0xD4, 0x8A, 0xFF });
        set(color::TextA5, { 0xF0, 0xD4, 0x8A, 0x0D });
        set(color::TextA20, { 0xF0, 0xD4, 0x8A, 0x33 });
        set(color::TextA30, { 0xF0, 0xD4, 0x8A, 0x4D });
        set(color::TextA50, { 0xF0, 0xD4, 0x8A, 0x80 });
        set(color::TextA80, { 0xF0, 0xD4, 0x8A, 0xCC });
        set(color::TextDim, { 0xC9, 0xAE, 0x6E, 0xFF });
        set(color::Elevated, { 0xF0, 0x7A, 0x1E, 0x38 });
        set(color::Background1, { 0x0B, 0x06, 0x04, 0xFF });
    }

    // The ROM option: shows which ROM is in use ("ROM: Original", "ROM: Uncensored", "ROM: French", or
    // "ROM: Modified" for another hack), and picks another one to play from now on: the US ROM, or a ROM hack of it
    // that only changes the game's data (main.cpp's accept_rom). It replaces the stored one; saves
    // and settings stay.
    constexpr uint64_t uncensored_rom_hash = 0xAC445026C8F77A94ULL; // the uncensored speech hack
    constexpr uint64_t german_rom_hash = 0xFA76F05BA7E6D7D9ULL; // the German translation (ConkerDeutschMod 0.2.0)
    constexpr uint64_t russian_rom_hash = 0xB97E9AFFEA6ED925ULL; // the Russian fan translation (as CBFD-Recompiled lists it)

    std::string rom_title(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::vector<char> rom((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (rom.empty()) {
            return "Change ROM";
        }
        const uint64_t hash = XXH3_64bits(rom.data(), rom.size());
        return hash == conker::us_rom_hash ? "ROM: Original" : hash == uncensored_rom_hash ? "ROM: Uncensored" :
            hash == conker::french_rom_hash ? "ROM: French" : hash == german_rom_hash ? "ROM: German" : hash == russian_rom_hash ? "ROM: Russian" : "ROM: Modified";
    }

    std::filesystem::path stored_rom_path(const std::u8string& game_id) {
        return recomp::get_config_path() / (game_id + u8".z64");
    }

    // Whether a ROM file is in one of the byte-swapped orders (.v64, .n64), from its first word.
    bool rom_file_byteswapped(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        unsigned char start[4] = {};
        file.read(reinterpret_cast<char*>(start), 4);
        return (start[0] == 0x37 && start[1] == 0x80) || (start[0] == 0x40 && start[1] == 0x12 && start[2] == 0x37);
    }

    std::vector<char> read_whole(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void change_rom(const std::u8string& game_id, recompui::GameOption* option) {
        recompui::file::open_file_dialog([game_id, option](bool success, const std::filesystem::path& path) {
            if (!success) {
                return;
            }
            // A ROM hack in a byte-swapped order is a patch applied to a .v64 or .n64 copy of the US ROM: patches
            // are made for .z64, so what they changed comes out scrambled once the ROM is put in order, while the
            // untouched code still passes the check. The game crashed as it started (issue #31: the uncensored and
            // German patches on a .v64 US ROM). Such a ROM is turned down, and the one in use kept.
            const bool byteswapped = rom_file_byteswapped(path);
            const std::filesystem::path stored = stored_rom_path(game_id);
            const std::vector<char> previous = byteswapped ? read_whole(stored) : std::vector<char>();
            switch (recomp::select_rom(path, game_id)) {
                case recomp::RomValidationError::Good: {
                    if (byteswapped && rom_title(stored) == "ROM: Modified") {
                        if (previous.empty()) {
                            std::error_code ec;
                            std::filesystem::remove(stored, ec);
                        } else {
                            std::ofstream out(stored, std::ios::binary | std::ios::trunc);
                            out.write(previous.data(), (std::streamsize)previous.size());
                        }
                        recompui::message_box("This ROM looks like a patch applied to a byte-swapped (.v64 or .n64) "
                            "copy of the US ROM. Patches are made for .z64 ROMs, so the patched parts come out scrambled "
                            "and the game would crash. Convert your US ROM to .z64 first, apply the patch to that, then "
                            "load the result. Your current ROM is still in use.");
                        break;
                    }
                    const std::string title = rom_title(stored_rom_path(game_id));
                    rom_button_name = button_name(title);
                    recompui::ContextId context = recompui::get_launcher_context_id();
                    const bool opened = context.open_if_not_already();
                    option->set_title(title);
                    if (opened) {
                        context.close();
                    }
                    break;
                }
                case recomp::RomValidationError::FailedToOpen:
                    recompui::message_box("Couldn't open that file.");
                    break;
                default:
                    recompui::message_box("That isn't the US ROM of Conker's Bad Fur Day, or a hack of it that keeps "
                        "the game's code. Your current ROM is still in use.");
                    break;
            }
        });
    }

    void init_launcher(recompui::LauncherMenu* menu) {
        const recomp::GameEntry& game = supported_games[0];
        menu->remove_default_title();
        add_posters(menu);
        add_settings_backdrop();
        // recompui's UI exists now: the FPS counter can make its own.
        conker::fps_counter::on_ui_ready();
        conker::photo::on_ui_ready();
        conker::crosshair::on_ui_ready();
        conker::qol::on_ui_ready();

        recompui::GameOptionsMenu* options = menu->init_game_options_menu(
            game.game_id, game.mod_game_id, game.display_name, game.thumbnail_bytes,
            recompui::GameOptionsMenuLayout::Right);
        options->set_bottom(6.0f); // a little lower than recompui's 24 px
        recompui::update_game_mod_id(game.mod_game_id);
        // In the menu's order (options show in the order they're added).
        recompui::GameOption* start_option = options->add_start_game_or_load_rom_option();
        recompui::GameOption* controls_option = options->add_setup_controls_option();
        recompui::GameOption* settings_option = options->add_settings_option();
        recompui::GameOption* mods_option = options->add_mods_option();
        static recompui::GameOption* rom_option = nullptr;
        rom_option = options->add_option(rom_title(stored_rom_path(game.game_id)),
            [game_id = game.game_id]() { change_rom(game_id, rom_option); });
        recompui::GameOption* entries[] = {
            start_option, controls_option, settings_option, mods_option, rom_option, options->add_exit_option(),
        };
        launcher_game_id = game.game_id;
        rom_button_name = button_name(rom_title(stored_rom_path(game.game_id)));
        load_button_pictures();
        menu_buttons.clear();
        const std::string names[] = { "start_game", "controls", "settings", "mods", rom_button_name, "exit" };
        for (size_t i = 0; i < std::size(entries); i++) {
            entries[i]->set_font_family(title_font);
            entries[i]->set_letter_spacing(1.5f);
            add_button_picture(entries[i], names[i]);
        }
    }

    std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
        uint8_t* rdram, ultramodern::renderer::WindowHandle handle, bool developer_mode) {
        return recompui::renderer::create_render_context(rdram, handle,
            ultramodern::renderer::PresentationMode::PresentEarly, developer_mode);
    }

    // Testing aid: CONKER_INPUT_SCRIPT="seconds:buttons[:held seconds],..." presses N64
    // buttons (hex, e.g. 1000 Start, 8000 A, 4000 B) at game times (so they match at any
    // CONKER_SPEED), so test runs can get from the intro into gameplay. Stick:
    // "seconds:x=1.0" or "y=-1".
    struct ScriptedPress { double at, held; uint16_t buttons; float x, y, rx = 0, ry = 0; };
    std::vector<ScriptedPress> input_script;

    void load_input_script() {
        const char* text = SDL_getenv("CONKER_INPUT_SCRIPT");
        if (text == nullptr) {
            return;
        }
        std::string s(text);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t end = s.find(',', pos);
            std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            ScriptedPress press{ 0, 0.2, 0, 0, 0, 0, 0 };
            size_t c1 = item.find(':');
            if (c1 != std::string::npos) {
                press.at = std::atof(item.c_str());
                std::string rest = item.substr(c1 + 1);
                size_t c2 = rest.find(':');
                std::string what = rest.substr(0, c2);
                if (c2 != std::string::npos) {
                    press.held = std::atof(rest.c_str() + c2 + 1);
                }
                if (what.rfind("rx=", 0) == 0) press.rx = (float)std::atof(what.c_str() + 3);
                else if (what.rfind("ry=", 0) == 0) press.ry = (float)std::atof(what.c_str() + 3);
                else if (what.rfind("x=", 0) == 0) press.x = (float)std::atof(what.c_str() + 2);
                else if (what.rfind("y=", 0) == 0) press.y = (float)std::atof(what.c_str() + 2);
                else press.buttons = (uint16_t)std::strtoul(what.c_str(), nullptr, 16);
                input_script.push_back(press);
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
    }

    bool get_port_input(int port, uint16_t* buttons, float* x, float* y); // below (controller ports)

    // The game's input: each port's controller (below), plus the test script, Skip Intro's
    // Start and the Free Camera's right stick.
    bool get_input(int controller, uint16_t* buttons, float* x, float* y) {
        bool ok = get_port_input(controller, buttons, x, y);
        if (controller == 0) {
            conker::testing::on_controller_read();
        }
        if (controller == 0 && !input_script.empty()) {
            double t = conker::testing::game_seconds();
            float rx = 0.0f, ry = 0.0f;
            for (const ScriptedPress& press : input_script) {
                if (t >= press.at && t < press.at + press.held) {
                    rx += press.rx;
                    ry += press.ry;
                }
            }
            conker::testing::set_right_stick(rx, ry);
            for (const ScriptedPress& press : input_script) {
                if (t >= press.at && t < press.at + press.held) {
                    static double last_logged = -1;
                    if (press.at != last_logged) {
                        last_logged = press.at;
                        std::printf("[input] %.1fs: buttons %04X\n", t, press.buttons);
                    }
                    *buttons |= press.buttons;
                    if (press.x != 0) *x = press.x;
                    if (press.y != 0) *y = press.y;
                }
            }
        }
        // The Free Camera steers with the right stick: while it's pushed, it doesn't also press
        // the C-buttons it's bound to (C-Left/C-Right would hand the camera back to the game).
        // (Not while Aiming: Swap Sticks has the right stick aiming: the C-buttons then come from the left stick.)
        if (controller == 0 && !conker::look_aim::swap_sticks_now(0)) {
            float cx = 0.0f, cy = 0.0f;
            conker::free_camera_stick(&cx, &cy);
            constexpr uint16_t c_right = 0x0001, c_left = 0x0002, c_down = 0x0004, c_up = 0x0008;
            if (cx != 0.0f) *buttons &= (uint16_t)~(c_left | c_right);
            if (cy != 0.0f) *buttons &= (uint16_t)~(c_up | c_down);
        }
        if (controller == 0) {
            conker::photo::filter_input(buttons, x, y);
        }
        if (controller == 0 && conker::skip_intro_pressing_start()) {
            *buttons |= 0x1000; // Start
        }
        if (controller == 0) {
            conker::qol::set_player_buttons(*buttons);
            {
                static uint16_t last_buttons = 0;
                if (*buttons != last_buttons) {
                    conker::events_note("buttons", *buttons, last_buttons);
                    last_buttons = *buttons;
                }
            }
            conker::qol::apply_walk(*buttons, x, y);
            conker::qol::apply_swim(y);
            *buttons = conker::qol::apply_toggles(*buttons);
            *buttons = conker::qol::apply_skip_press(*buttons);
            conker::crosshair::set_buttons(*buttons);
        }
        if (controller == 0) {
            static bool start_was_down = false;
            const bool start_down = (*buttons & 0x1000) != 0;
            if (start_down && !start_was_down) {
                conker_probe_pause_start();
                conker_pause_background_start_pressed();
            }
            start_was_down = start_down;
        }
        return ok;
    }

    ultramodern::input::connected_device_info_t device_info_callback(int controller) {
        return conker::frontend::device_info(controller);
    }
}

// From CBFD-Recompiled (MIT): controller ports, one per controller.
// Controller ports. recompinput's single-player mode reports all four ports as plugged
// in and gives every port the merged input of every device, so on the multiplayer
// screen one Start press joined players 2-4 as well. Instead, each controller gets its
// own port, in the order the controllers first press a button (Windows lists them in
// its own order, not the order they were turned on): the first to press one is player
// 1, with the keyboard, the next player 2, and so on. A controller that disconnects
// gives up its port, and the ones after it move up. As many ports as there are
// controllers report as plugged in (at least one), so the game sees player 2's port
// from the start; it gets that controller's input once it presses a button. Every
// controller uses the single-player controller bindings.
namespace {
    constexpr int max_ports = 4;

    std::mutex port_mutex;
    // The controllers holding ports 1-4, in the order they first pressed a button.
    std::vector<SDL_JoystickID> port_order;
    // The game controllers plugged in, listed on the main thread by refresh_connected_controllers:
    // SDL's device-index functions (SDL_NumJoysticks and the rest) aren't safe on two threads at once.
    // Under sdl2-compat, what Linux distributions ship as SDL2, each call rebuilds one shared list, so
    // the game thread reading it while a controller connects crashed (free(): unaligned chunk, issue #24).
    std::vector<SDL_JoystickID> connected_ids;

    bool controller_any_button(SDL_GameController* controller) {
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
            if (SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)b)) {
                return true;
            }
        }
        // The triggers are axes; the sticks are left out, so a drifting stick doesn't claim a port.
        return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384 ||
            SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384;
    }

    // Main thread, each frame after the events: lists the game controllers plugged in.
    void refresh_connected_controllers() {
        std::vector<SDL_JoystickID> ids;
        const int num_joysticks = SDL_NumJoysticks();
        for (int i = 0; i < num_joysticks; i++) {
            if (SDL_IsGameController(i)) {
                ids.push_back(SDL_JoystickGetDeviceInstanceID(i));
            }
        }
        std::lock_guard lock{ port_mutex };
        connected_ids = std::move(ids);
    }

    // Updates the port order and fills `out` with the controllers holding ports, in port
    // order. Returns how many hold ports; `connected` gets how many controllers are open.
    int get_port_controllers(std::array<SDL_GameController*, max_ports>& out, int* connected = nullptr) {
        std::lock_guard lock{ port_mutex };
        // The controllers recompinput has opened.
        std::vector<std::pair<SDL_JoystickID, SDL_GameController*>> open;
        for (SDL_JoystickID id : connected_ids) {
            SDL_GameController* controller = SDL_GameControllerFromInstanceID(id);
            if (controller != nullptr) {
                open.emplace_back(id, controller);
            }
        }
        auto find_open = [&](SDL_JoystickID id) -> SDL_GameController* {
            for (const auto& [open_id, controller] : open) {
                if (open_id == id) {
                    return controller;
                }
            }
            return nullptr;
        };
        // Disconnected controllers give up their ports.
        std::erase_if(port_order, [&](SDL_JoystickID id) { return find_open(id) == nullptr; });
        // Controllers pressing a button for the first time take the next port.
        for (const auto& [id, controller] : open) {
            if ((int)port_order.size() < max_ports && std::find(port_order.begin(), port_order.end(), id) == port_order.end() &&
                controller_any_button(controller)) {
                port_order.push_back(id);
            }
        }
        int count = 0;
        for (SDL_JoystickID id : port_order) {
            out[count++] = find_open(id);
        }
        if (connected != nullptr) {
            *connected = std::min((int)open.size(), max_ports);
        }
        return count;
    }

    // From CBFD-Recompiled V0.1.6 (MIT): the stick axis read for an axis bound. With swap (Aiming: Swap Sticks),
    // the left stick's axes are read from the right stick and the right's from the left, so whatever the profile
    // binds to either stick (by default the N64 stick to the left, the C-buttons to the right) moves to the other.
    int stick_axis(int axis, bool swap) {
        if (!swap) {
            return axis;
        }
        switch (axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: return SDL_CONTROLLER_AXIS_RIGHTX;
        case SDL_CONTROLLER_AXIS_LEFTY: return SDL_CONTROLLER_AXIS_RIGHTY;
        case SDL_CONTROLLER_AXIS_RIGHTX: return SDL_CONTROLLER_AXIS_LEFTX;
        case SDL_CONTROLLER_AXIS_RIGHTY: return SDL_CONTROLLER_AXIS_LEFTY;
        default: return axis;
        }
    }

    // Whether a binding is to the right stick (the C-buttons, by default).
    bool is_right_stick(const recompinput::InputField& field) {
        if (field.input_type != recompinput::InputType::ControllerAnalog) {
            return false;
        }
        const int axis = std::abs(field.input_id) - 1;
        return axis == SDL_CONTROLLER_AXIS_RIGHTX || axis == SDL_CONTROLLER_AXIS_RIGHTY;
    }

    float controller_field_analog(SDL_GameController* controller, const recompinput::InputField& field, bool swap = false) {
        switch (field.input_type) {
        case recompinput::InputType::ControllerDigital:
            if (field.input_id >= 0 && field.input_id < SDL_CONTROLLER_BUTTON_MAX) {
                return SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)field.input_id) ? 1.0f : 0.0f;
            }
            return 0.0f;
        case recompinput::InputType::ControllerAnalog: {
            int axis = std::abs(field.input_id) - 1;
            if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) {
                return 0.0f;
            }
            float value = SDL_GameControllerGetAxis(controller, (SDL_GameControllerAxis)stick_axis(axis, swap)) * (1.0f / 32768.0f);
            if (field.input_id < 0) {
                value = -value;
            }
            return std::clamp(value, 0.0f, 1.0f);
        }
        default:
            return 0.0f;
        }
    }

    bool controller_field_digital(SDL_GameController* controller, const recompinput::InputField& field, bool swap = false) {
        if (field.input_type == recompinput::InputType::ControllerAnalog) {
            return controller_field_analog(controller, field, swap) >= recompinput::axis_digital_threshold;
        }
        return controller_field_analog(controller, field, swap) > 0.0f;
    }

    // The keyboard and the mouse are player 1's: their bindings count on port 1 only. Mouse buttons
    // can be bound with the keyboard's controls or, in single player, the controller's.
    bool is_keyboard_or_mouse(const recompinput::InputField& field) {
        return field.input_type == recompinput::InputType::Keyboard || field.input_type == recompinput::InputType::Mouse;
    }

    // One controller (or none) and/or the keyboard and mouse, through the single-player bindings. With swap_sticks
    // (Aiming: Swap Sticks, while aiming), the controller's two sticks are read the other way round.
    void read_port(SDL_GameController* controller, bool keyboard, bool swap_sticks, bool z_turns_aim, uint16_t* buttons, float* x, float* y) {
        using recompinput::GameInput;
        static constexpr uint16_t button_values[] = {
            0x8000, 0x4000, 0x2000, 0x0020, 0x0010, 0x1000, 0x0008,
            0x0004, 0x0002, 0x0001, 0x0800, 0x0400, 0x0200, 0x0100,
        };
        // A..C Right, then the D-pad (N64_BUTTON_COUNT stops at C Right).
        static_assert(std::size(button_values) == (size_t)GameInput::DPAD_RIGHT - (size_t)GameInput::N64_BUTTON_START + 1);
        const int cont_profile = recompinput::profiles::get_sp_controller_profile_index();
        const int kb_profile = recompinput::profiles::get_sp_keyboard_profile_index();
        auto binding = [](int profile, GameInput input, size_t i) -> const recompinput::InputField& {
            return recompinput::profiles::get_input_binding(profile, input, i);
        };
        auto cont_analog = [&](GameInput input) {
            float v = 0.0f;
            for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
                const recompinput::InputField& field = binding(cont_profile, input, i);
                if (field.input_type == recompinput::InputType::Mouse) {
                    v += keyboard ? recompinput::get_input_analog(0, field) : 0.0f;
                }
                else if (controller != nullptr) {
                    v += controller_field_analog(controller, field, swap_sticks);
                }
            }
            return std::clamp(v, 0.0f, 1.0f);
        };
        auto kb_analog = [&](GameInput input) {
            float v = 0.0f;
            for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
                const recompinput::InputField& field = binding(kb_profile, input, i);
                if (is_keyboard_or_mouse(field)) {
                    v += recompinput::get_input_analog(0, field);
                }
            }
            return std::clamp(v, 0.0f, 1.0f);
        };

        uint16_t cur_buttons = 0;
        // With swap_sticks, the buttons pressed only by the left stick (bindings to the right stick, read from the
        // left): see below.
        uint16_t left_stick_buttons = 0;
        float cur_x = 0.0f;
        float cur_y = 0.0f;
        for (size_t b = 0; b < std::size(button_values); b++) {
            GameInput input = (GameInput)((size_t)GameInput::N64_BUTTON_START + b);
            bool pressed = false;
            bool by_left_stick = false;
            for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
                if (cont_profile >= 0) {
                    const recompinput::InputField& field = binding(cont_profile, input, i);
                    if (field.input_type == recompinput::InputType::Mouse) {
                        pressed |= keyboard && recompinput::get_input_digital(0, field);
                    }
                    else if (controller != nullptr) {
                        const bool down = controller_field_digital(controller, field, swap_sticks);
                        if (swap_sticks && is_right_stick(field)) {
                            by_left_stick |= down;
                        }
                        else {
                            pressed |= down;
                        }
                    }
                }
                if (keyboard && kb_profile >= 0) {
                    const recompinput::InputField& field = binding(kb_profile, input, i);
                    if (is_keyboard_or_mouse(field)) {
                        pressed |= recompinput::get_input_digital(0, field);
                    }
                }
            }
            if (pressed) {
                cur_buttons |= button_values[b];
            }
            else if (by_left_stick) {
                left_stick_buttons |= button_values[b];
            }
        }
        // Aiming: Swap Sticks: the left stick presses the C-buttons, which move Conker while aiming, but not C-Left and
        // C-Right while Z is held aiming the shotgun (z_turns_aim): Z makes them turn the aim instead.
        constexpr uint16_t z_button = 0x2000, c_left = 0x0002, c_right = 0x0001;
        if ((cur_buttons & z_button) && z_turns_aim) {
            left_stick_buttons &= (uint16_t)~(c_left | c_right);
        }
        cur_buttons |= left_stick_buttons;
        if (controller != nullptr && cont_profile >= 0) {
            cur_x = cont_analog(GameInput::X_AXIS_POS) - cont_analog(GameInput::X_AXIS_NEG);
            cur_y = cont_analog(GameInput::Y_AXIS_POS) - cont_analog(GameInput::Y_AXIS_NEG);
            recompinput::apply_joystick_deadzone(cur_x, cur_y, &cur_x, &cur_y);
        }
        if (keyboard && kb_profile >= 0) {
            const float kb_x = kb_analog(GameInput::X_AXIS_POS) - kb_analog(GameInput::X_AXIS_NEG);
            const float kb_y = kb_analog(GameInput::Y_AXIS_POS) - kb_analog(GameInput::Y_AXIS_NEG);
            if (swap_sticks) {
                // With the keyboard the mouse aims, so the keys bound to the stick (W, A, S, D by default) press the
                // C-buttons and move Conker, as the left stick does (not C-Left or C-Right while Z is held).
                constexpr uint16_t c_up = 0x0008, c_down = 0x0004;
                uint16_t keys = 0;
                if (kb_x <= -recompinput::axis_digital_threshold) keys |= c_left;
                if (kb_x >= recompinput::axis_digital_threshold) keys |= c_right;
                if (kb_y >= recompinput::axis_digital_threshold) keys |= c_up;
                if (kb_y <= -recompinput::axis_digital_threshold) keys |= c_down;
                if ((cur_buttons & z_button) && z_turns_aim) {
                    keys &= (uint16_t)~(c_left | c_right);
                }
                cur_buttons |= keys;
            }
            else {
                cur_x += kb_x;
                cur_y += kb_y;
            }
        }
        *buttons = cur_buttons;
        *x = std::clamp(cur_x, -1.0f, 1.0f);
        *y = std::clamp(cur_y, -1.0f, 1.0f);
    }

    bool get_port_input(int port, uint16_t* buttons, float* x, float* y) {
        // recompinput's own multiplayer mode (players assigned in its menus) gives each
        // player their controller and profiles already.
        if (!recompinput::players::is_single_player_mode()) {
            return recompinput::profiles::get_n64_input(port, buttons, x, y);
        }
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        if (port < 0 || port >= max_ports) {
            return false;
        }
        std::array<SDL_GameController*, max_ports> controllers{};
        int connected = 0;
        int count = get_port_controllers(controllers, &connected);
        if (port >= std::max(connected, 1)) {
            return false;
        }
        if (!recompinput::game_input_disabled()) {
            // Port 1 has the keyboard, and its controller once one has pressed a button.
            read_port(port < count ? controllers[port] : nullptr, port == 0, conker::look_aim::swap_sticks_now(port),
                conker::look_aim::z_turns_aim(port), buttons, x, y);
        }
        return true;
    }
}

int conker::frontend::port_controllers(std::array<SDL_GameController*, max_ports>& out) {
    return get_port_controllers(out);
}

void conker::frontend::on_vi() {
    conker::rumble::update();
}

ultramodern::input::connected_device_info_t conker::frontend::device_info(int controller_num) {
    if (!recompinput::players::is_single_player_mode()) {
        if (recompinput::players::get_player_is_assigned(controller_num)) {
            return { ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak };
        }
        return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }
    std::array<SDL_GameController*, max_ports> controllers{};
    int connected = 0;
    get_port_controllers(controllers, &connected);
    if (controller_num == 0 || controller_num < connected) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

void conker::frontend::init(recomp::GameEntry& game) {
    recompui::programconfig::set_program_name(program_name);
    recompui::programconfig::set_program_id(u8"ConkerBFDReloaded");
    supported_games.push_back(game);
    recompui::register_launcher_init_callback(init_launcher);
    recompui::register_launcher_update_callback(update_launcher);
    recompui::register_primary_font("InterVariable.ttf", "Inter Variable");
    recompui::register_extra_font("LuckiestGuy-Regular.ttf");
    apply_theme();
    recompui::register_ui_exports();
    recompinput::players::set_single_player_mode(true);
    load_input_script();
    conker::init_settings();
}

void conker::frontend::set_callbacks(recomp::Configuration& cfg) {
    cfg.renderer_callbacks.create_render_context = create_render_context;
    cfg.gfx_callbacks = { create_gfx, create_window, update_gfx };
    cfg.input_callbacks = { poll_inputs, get_input, conker::rumble::set,
                            device_info_callback };
    cfg.error_handling_callbacks.message_box = recompui::message_box;
}
