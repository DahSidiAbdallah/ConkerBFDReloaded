// The window, the launcher and menus (RecompFrontend's recompui) and remappable
// input (recompinput). Launcher design: a full-screen poster (assets/poster_bar.png),
// our logo in the style of the game's box logo on the left (assets/logo.png, rendered in Blender by
// art/logo_3d.py) and the menu on the right, in orange and cream.

#include <algorithm>
#include <array>
#include <mutex>
#include <chrono>
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
    const recompui::Color orange{ 0xF0, 0x7A, 0x1E, 0xFF };
    const recompui::Color cream{ 0xFF, 0xE9, 0xC2, 0xFF };

    // Launcher posters, shown in turn.
    constexpr const char* poster_files[] = { "poster_bar.png" };
    constexpr auto poster_time = std::chrono::seconds(12);
    std::vector<recompui::Image*> posters;
    size_t current_poster = 0;
    std::chrono::steady_clock::time_point poster_shown;

    void add_posters(recompui::LauncherMenu* menu) {
        recompui::ContextId context = recompui::get_current_context();
        recompui::Element* parent = menu->get_background_container();
        posters.clear();
        for (size_t i = 0; i < std::size(poster_files); i++) {
            std::ifstream file(recompui::file::get_asset_path(poster_files[i]), std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::printf("[frontend] poster %s: %zu bytes\n", poster_files[i], bytes.size());
            if (bytes.empty()) {
                continue;
            }
            std::string src = "?/conker/poster" + std::to_string(i);
            recompui::queue_image_from_bytes_file(src, bytes);
            recompui::Image* image = context.create_element<recompui::Image>(parent, src);
            // Cover the window, centred (the posters are a little taller than 16:9).
            image->set_position(recompui::Position::Absolute);
            image->set_top(50.0f, recompui::Unit::Percent);
            image->set_left(0);
            image->set_width(100.0f, recompui::Unit::Percent);
            image->set_height_auto();
            image->set_translate_2D(0.0f, -50.0f, recompui::Unit::Percent);
            image->set_opacity(posters.empty() ? 1.0f : 0.0f);
            posters.push_back(image);
        }
        current_poster = 0;
        poster_shown = std::chrono::steady_clock::now();
    }

    void update_launcher(recompui::LauncherMenu*) {
        if (posters.size() < 2 || std::chrono::steady_clock::now() - poster_shown < poster_time) {
            return;
        }
        posters[current_poster]->set_opacity(0.0f);
        current_poster = (current_poster + 1) % posters.size();
        posters[current_poster]->set_opacity(1.0f);
        poster_shown = std::chrono::steady_clock::now();
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

    void update_gfx(void*) {
        recompinput::handle_events();
        conker::fps_counter::update();
        conker::crosshair::update();
        conker::qol::update();
    }

    // The logo, in the dark left part of the poster.
    void add_logo(recompui::LauncherMenu* menu) {
        std::ifstream file(recompui::file::get_asset_path("logo.png"), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            return;
        }
        recompui::queue_image_from_bytes_file("?/conker/logo", bytes);
        recompui::Image* logo = recompui::get_current_context().create_element<recompui::Image>(
            menu->get_background_container(), "?/conker/logo");
        logo->set_position(recompui::Position::Absolute);
        logo->set_left(6.0f, recompui::Unit::Percent);
        logo->set_top(50.0f, recompui::Unit::Percent);
        logo->set_height(80.0f, recompui::Unit::Percent);
        logo->set_width_auto();
        logo->set_translate_2D(0.0f, -50.0f, recompui::Unit::Percent);
    }

    // Warm colours for the launcher and every menu (recompui's default is blue).
    void apply_theme() {
        using recompui::theme::color;
        auto set = recompui::theme::set_theme_color;
        set(color::Primary, orange);
        set(color::PrimaryL, { 0xFF, 0xA0, 0x55, 0xFF });
        set(color::PrimaryD, { 0xB8, 0x55, 0x10, 0xFF });
        set(color::PrimaryA5, { 0xF0, 0x7A, 0x1E, 0x0D });
        set(color::PrimaryA20, { 0xF0, 0x7A, 0x1E, 0x33 });
        set(color::PrimaryA30, { 0xF0, 0x7A, 0x1E, 0x4D });
        set(color::PrimaryA50, { 0xF0, 0x7A, 0x1E, 0x80 });
        set(color::PrimaryA80, { 0xF0, 0x7A, 0x1E, 0xCC });
        set(color::TextActive, cream);
        set(color::Text, { 0xF2, 0xDD, 0xBE, 0xFF });
        set(color::TextDim, { 0xC9, 0xA5, 0x7A, 0xFF });
        set(color::Elevated, { 0xF0, 0x7A, 0x1E, 0x38 });
        set(color::Background1, { 0x0B, 0x06, 0x04, 0xFF });
    }

    // The ROM option: shows which ROM is in use ("ROM: Original", "ROM: Uncensored", or "ROM: Modified"
    // for another hack), and picks another one to play from now on: the US ROM, or a ROM hack of it
    // that only changes the game's data (main.cpp's accept_rom). It replaces the stored one; saves
    // and settings stay.
    constexpr uint64_t uncensored_rom_hash = 0xAC445026C8F77A94ULL; // the uncensored speech hack

    std::string rom_title(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::vector<char> rom((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (rom.empty()) {
            return "Change ROM";
        }
        const uint64_t hash = XXH3_64bits(rom.data(), rom.size());
        return hash == conker::us_rom_hash ? "ROM: Original" : hash == uncensored_rom_hash ? "ROM: Uncensored" : "ROM: Modified";
    }

    std::filesystem::path stored_rom_path(const std::u8string& game_id) {
        return recomp::get_config_path() / (game_id + u8".z64");
    }

    void change_rom(const std::u8string& game_id, recompui::GameOption* option) {
        recompui::file::open_file_dialog([game_id, option](bool success, const std::filesystem::path& path) {
            if (!success) {
                return;
            }
            switch (recomp::select_rom(path, game_id)) {
                case recomp::RomValidationError::Good: {
                    const std::string title = rom_title(stored_rom_path(game_id));
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
        add_logo(menu);
        // recompui's UI exists now: the FPS counter can make its own.
        conker::fps_counter::on_ui_ready();
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
        for (recompui::GameOption* entry : entries) {
            entry->set_font_family(title_font);
            entry->set_letter_spacing(1.5f);
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
        if (controller == 0) {
            float cx = 0.0f, cy = 0.0f;
            conker::free_camera_stick(&cx, &cy);
            constexpr uint16_t c_right = 0x0001, c_left = 0x0002, c_down = 0x0004, c_up = 0x0008;
            if (cx != 0.0f) *buttons &= (uint16_t)~(c_left | c_right);
            if (cy != 0.0f) *buttons &= (uint16_t)~(c_up | c_down);
        }
        if (controller == 0 && conker::skip_intro_pressing_start()) {
            *buttons |= 0x1000; // Start
        }
        if (controller == 0) {
            conker::qol::set_player_buttons(*buttons);
            *buttons = conker::qol::apply_toggles(*buttons);
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

    // Updates the port order and fills `out` with the controllers holding ports, in port
    // order. Returns how many hold ports; `connected` gets how many controllers are open.
    int get_port_controllers(std::array<SDL_GameController*, max_ports>& out, int* connected = nullptr) {
        std::lock_guard lock{ port_mutex };
        // The controllers recompinput has opened.
        std::vector<std::pair<SDL_JoystickID, SDL_GameController*>> open;
        int num_joysticks = SDL_NumJoysticks();
        for (int i = 0; i < num_joysticks; i++) {
            if (!SDL_IsGameController(i)) {
                continue;
            }
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
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

    float controller_field_analog(SDL_GameController* controller, const recompinput::InputField& field) {
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
            float value = SDL_GameControllerGetAxis(controller, (SDL_GameControllerAxis)axis) * (1.0f / 32768.0f);
            if (field.input_id < 0) {
                value = -value;
            }
            return std::clamp(value, 0.0f, 1.0f);
        }
        default:
            return 0.0f;
        }
    }

    bool controller_field_digital(SDL_GameController* controller, const recompinput::InputField& field) {
        if (field.input_type == recompinput::InputType::ControllerAnalog) {
            return controller_field_analog(controller, field) >= recompinput::axis_digital_threshold;
        }
        return controller_field_analog(controller, field) > 0.0f;
    }

    // The keyboard and the mouse are player 1's: their bindings count on port 1 only. Mouse buttons
    // can be bound with the keyboard's controls or, in single player, the controller's.
    bool is_keyboard_or_mouse(const recompinput::InputField& field) {
        return field.input_type == recompinput::InputType::Keyboard || field.input_type == recompinput::InputType::Mouse;
    }

    // One controller (or none) and/or the keyboard and mouse, through the single-player bindings.
    void read_port(SDL_GameController* controller, bool keyboard, uint16_t* buttons, float* x, float* y) {
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
                    v += controller_field_analog(controller, field);
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
        float cur_x = 0.0f;
        float cur_y = 0.0f;
        for (size_t b = 0; b < std::size(button_values); b++) {
            GameInput input = (GameInput)((size_t)GameInput::N64_BUTTON_START + b);
            bool pressed = false;
            for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
                if (cont_profile >= 0) {
                    const recompinput::InputField& field = binding(cont_profile, input, i);
                    if (field.input_type == recompinput::InputType::Mouse) {
                        pressed |= keyboard && recompinput::get_input_digital(0, field);
                    }
                    else if (controller != nullptr) {
                        pressed |= controller_field_digital(controller, field);
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
        }
        if (controller != nullptr && cont_profile >= 0) {
            cur_x = cont_analog(GameInput::X_AXIS_POS) - cont_analog(GameInput::X_AXIS_NEG);
            cur_y = cont_analog(GameInput::Y_AXIS_POS) - cont_analog(GameInput::Y_AXIS_NEG);
            recompinput::apply_joystick_deadzone(cur_x, cur_y, &cur_x, &cur_y);
        }
        if (keyboard && kb_profile >= 0) {
            cur_x += kb_analog(GameInput::X_AXIS_POS) - kb_analog(GameInput::X_AXIS_NEG);
            cur_y += kb_analog(GameInput::Y_AXIS_POS) - kb_analog(GameInput::Y_AXIS_NEG);
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
            read_port(port < count ? controllers[port] : nullptr, port == 0, buttons, x, y);
        }
        return true;
    }
}

int conker::frontend::port_controllers(std::array<SDL_GameController*, max_ports>& out) {
    return get_port_controllers(out);
}

void conker::frontend::on_vi() {
    conker::rumble::update();
    conker::pad_mappings::update();
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
