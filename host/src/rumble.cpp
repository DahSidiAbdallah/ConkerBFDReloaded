// From CBFD-Recompiled (Copyright (c) 2026 Sean Ciaschi, MIT License; see
// recomp/THIRD_PARTY_LICENSE), used unchanged except for this note.
// Rumble: the game turns each controller port's Rumble Pak on and off (set, the runtime's rumble
// callback), and every VI update sends it to that port's controller.
//
// RecompFrontend's own (recompinput::update_rumble) drove only a controller's small, high-frequency
// motor, eased the Rumble Pak's on and off into a ramp, and in its single-player input mode sent port
// 1's rumble to every controller, so with a controller on each port (frontend.cpp) player 2 felt
// player 1's. Issue #62 asked for the large motor (the Rumble Pak's is a slow, heavy one), and for
// the Rumble Pak's plain on and off: to a player who can't hear the game, the ramp blurs what the
// game is saying. Both are settings on the General tab, with Rumble: Strength; the defaults keep
// the original behaviour.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_set>

#include <SDL.h>

#include "recompinput/players.h"
#include "recompui/config.h"

#include "conker.hpp"

namespace {
    constexpr int max_ports = 4;

    namespace options {
        const std::string motor = "rumble_motor";
        const std::string style = "rumble_style";
    }
    enum class Motor : uint32_t { Small, Large, Both };
    enum class Style : uint32_t { Smooth, OnOff };

    template <typename T>
    T option(const std::string& id) {
        return static_cast<T>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(id)));
    }

    // What the game asked for, per port, and the ramp's level (Smooth).
    std::array<std::atomic_bool, max_ports> active{};
    std::array<float, max_ports> level{};
    // Controllers that refused to rumble aren't asked again.
    std::mutex failed_mutex;
    std::unordered_set<SDL_JoystickID> failed;

    void send_rumble(SDL_GameController* controller, float strength) {
        if (controller == nullptr) {
            return;
        }
        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
        const SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
        {
            std::lock_guard lock{ failed_mutex };
            if (failed.contains(id)) {
                return;
            }
        }
        const Uint16 value = (Uint16)std::clamp(strength * 65535.0f, 0.0f, 65535.0f);
        const Motor motor = option<Motor>(options::motor);
        const Uint16 low = (motor == Motor::Large || motor == Motor::Both) ? value : 0;
        const Uint16 high = (motor == Motor::Small || motor == Motor::Both) ? value : 0;
        // Long enough to last until the next update, which sets it again (0 stops it).
        if (SDL_JoystickRumble(joystick, low, high, 1000000) != 0) {
            std::lock_guard lock{ failed_mutex };
            failed.insert(id);
        }
    }
}

void conker::rumble::add_options(recomp::config::Config& config) {
    using EnumOptions = const std::vector<recomp::config::ConfigOptionEnumOption>;
    static EnumOptions motors = {
        {Motor::Small, "Small", "Small"},
        {Motor::Large, "Large", "Large"},
        {Motor::Both, "Both", "Both"},
    };
    static EnumOptions styles = {
        {Style::Smooth, "Smooth", "Smooth"},
        {Style::OnOff, "OnOff", "On/Off"},
    };
    // RecompFrontend's Rumble Strength (same id, so a saved value carries over), added here to sit
    // with the other rumble settings.
    config.add_percent_number_option(recompui::config::general::options::rumble_strength, "Rumble: Strength",
        "How strong the rumble is, on controllers that have it. <b>Zero turns rumble off.</b>",
        25.0);
    config.add_enum_option(options::motor, "Rumble: Motor",
        "Which of the controller's motors the Rumble Pak's rumble uses. "
        "<recomp-color primary>Small</recomp-color>: the quick, light one (as before). "
        "<recomp-color primary>Large</recomp-color>: the slow, heavy one, closer to the Rumble Pak's own motor. "
        "<recomp-color primary>Both</recomp-color>: both at once.",
        motors, Motor::Small);
    config.add_enum_option(options::style, "Rumble: Style",
        "How the rumble follows the game. <recomp-color primary>Smooth</recomp-color>: it eases in and out (as before). "
        "<recomp-color primary>On/Off</recomp-color>: it starts and stops exactly when the game turns the Rumble Pak "
        "on and off, as a real Rumble Pak and controllers with an on/off motor do.",
        styles, Style::Smooth);
}

void conker::rumble::set(int port, bool on) {
    if (port >= 0 && port < max_ports) {
        active[port] = on;
    }
}

void conker::rumble::update() {
    if (!recompui::config::general::has_rumble_strength_option()) {
        return;
    }
    const float strength = (float)recompui::config::general::get_rumble_strength() / 100.0f;
    const bool smooth = option<Style>(options::style) == Style::Smooth;

    std::array<SDL_GameController*, max_ports> controllers{};
    int ports = 0;
    if (recompinput::players::is_single_player_mode()) {
        ports = conker::frontend::port_controllers(controllers);
    }
    else {
        // recompinput's multiplayer mode: the players assigned in its menus.
        ports = std::min((int)recompinput::players::get_number_of_assigned_players(), max_ports);
        for (int i = 0; i < ports; i++) {
            controllers[i] = recompinput::players::get_player(i).controller;
        }
    }

    for (int i = 0; i < max_ports; i++) {
        const bool on = active[i];
        float amount;
        if (smooth) {
            // recompinput's ramp, measured by feel there: up quickly, down more slowly, then a smoothstep.
            if (on) {
                level[i] = std::min(level[i] + 0.17f, 1.0f);
            }
            else {
                level[i] = std::max(level[i] * 0.92f - 0.01f, 0.0f);
            }
            amount = level[i] * level[i] * (3.0f - 2.0f * level[i]);
        }
        else {
            level[i] = on ? 1.0f : 0.0f;
            amount = level[i];
        }
        if (i < ports) {
            send_rumble(controllers[i], amount * strength);
        }
    }
}
