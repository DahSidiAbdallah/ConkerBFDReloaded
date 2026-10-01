// The settings menu's tabs (recompui), with our own Conker tab first: the
// Classic/Modern experience switch and camera options.

#include <atomic>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "recompui/config.h"
#include "recompui/renderer.h"
#include "ultramodern/config.hpp"
#include "recompinput/recompinput.h"
#include "util/file.h"

#include "conker.hpp"

extern "C" volatile int rt64_conker_smooth_shading; // RT64 (rt64_rsp.cpp, patches/rt64_conker.patch)

namespace {
    namespace gfx = recompui::config::graphics::options;
    using namespace ultramodern::renderer;

    std::atomic<bool> skip_intro_enabled{ false };
    std::atomic<bool> free_camera{ true };
    std::atomic<bool> auto_follow{ true };

    enum class Experience : uint32_t { Classic, Modern, Custom };

    // Graphics options RT64 has that recompui's Graphics tab doesn't; added to that tab.
    namespace extra {
        const std::string texture_filter = "conker_texture_filter";
        const std::string upscale_2d = "conker_upscale_2d";
        const std::string screen_filter = "conker_screen_filter";
        const std::string shading = "conker_shading";
    }
    enum class TextureFilter : uint32_t { N64, Smooth };
    enum class Upscale2D : uint32_t { Original, ScaledOnly, All };
    enum class ScreenFilter : uint32_t { Sharp, Smooth };
    enum class Shading : uint32_t { Original, Smooth };

    uint32_t graphics_value(const std::string& id) {
        auto value = recompui::config::get_graphics_config().get_option_value(id);
        const uint32_t* v = std::get_if<uint32_t>(&value);
        return v != nullptr ? *v : 0;
    }

    // Called by recompui whenever it applies the graphics settings to RT64.
    void extend_rt64_config(RT64::UserConfiguration& rt64) {
        rt64.threePointFiltering = (TextureFilter)graphics_value(extra::texture_filter) == TextureFilter::N64;
        switch ((Upscale2D)graphics_value(extra::upscale_2d)) {
            case Upscale2D::Original: rt64.upscale2D = RT64::UserConfiguration::Upscale2D::Original; break;
            case Upscale2D::ScaledOnly: rt64.upscale2D = RT64::UserConfiguration::Upscale2D::ScaledOnly; break;
            case Upscale2D::All: rt64.upscale2D = RT64::UserConfiguration::Upscale2D::All; break;
        }
        rt64.filtering = (ScreenFilter)graphics_value(extra::screen_filter) == ScreenFilter::Sharp
            ? RT64::UserConfiguration::Filtering::AntiAliasedPixelScaling
            : RT64::UserConfiguration::Filtering::Linear;
    }

    void add_extra_graphics_options() {
        recomp::config::Config& graphics = recompui::config::get_graphics_config();
        graphics.add_enum_option(extra::texture_filter, "Texture Filtering",
            "N64 uses the console's own three-point filtering, the way textures looked on the N64. "
            "Smooth uses standard bilinear filtering, which blends textures a little more evenly.",
            { { TextureFilter::N64, "N64", "N64" }, { TextureFilter::Smooth, "Smooth", "Smooth" } },
            TextureFilter::N64);
        // RT64 has a third choice, ScaledOnly (sharpen only the 2D the game scales); tested
        // side by side on the menus and the pause screen it looked the same as Original, so
        // it isn't offered.
        graphics.add_enum_option(extra::upscale_2d, "2D Graphics",
            "How 2D pictures (text, menus, the HUD) are drawn. Original keeps them at the N64's resolution, "
            "pixels and all, as on the console. Smooth draws them at full resolution.",
            { { Upscale2D::Original, "Original", "Original" }, { Upscale2D::All, "All", "Smooth" } },
            Upscale2D::Original);
        graphics.add_enum_option(extra::screen_filter, "Screen Filter",
            "How the picture is enlarged to the window when the resolution is lower than the window's. "
            "Sharp keeps pixels crisp, Smooth blends them.",
            { { ScreenFilter::Sharp, "Sharp", "Sharp" }, { ScreenFilter::Smooth, "Smooth", "Smooth" } },
            ScreenFilter::Sharp);
        graphics.add_enum_option(extra::shading, "Shading",
            "How the game's lights (torches, lamps, fires) light walls and characters. Original works the light "
            "out at the corners of each triangle and blends it between them, as the N64 did: pools of light "
            "look blotchy and angular. Smooth works it out for every pixel: soft, round pools of light.",
            { { Shading::Original, "Original", "Original" }, { Shading::Smooth, "Smooth", "Smooth" } },
            Shading::Original);
        graphics.add_option_change_callback(extra::shading,
            [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                rt64_conker_smooth_shading = (Shading)std::get<uint32_t>(cur) == Shading::Smooth ? 1 : 0;
            });
        // It only does anything below the window's resolution, so it's hidden at Auto.
        graphics.add_option_hidden_dependency(extra::screen_filter, gfx::res_option, Resolution::Auto);
        for (const std::string* id : { &extra::texture_filter, &extra::upscale_2d, &extra::screen_filter }) {
            graphics.add_option_change_callback(*id,
                [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
                    if (context != recomp::config::OptionChangeContext::Temporary) {
                        recompui::renderer::refresh_user_config_extension();
                    }
                });
        }
        recompui::renderer::set_user_config_extension(extend_rt64_config);
    }

    // What Classic and Modern set. Classic: the game as it looked, ran and played on the N64 (4:3,
    // 30 fps, native resolution, no smoothing of edges, N64 texture filtering, the full intro, the
    // game's own camera, no mouse). Modern: widescreen, the display's frame rate, the window's
    // resolution, 4x anti-aliasing, smooth textures, 2D and shading, straight to the save menu, the free
    // camera with auto-follow, and the mouse turning the camera and aiming directly. Settings that
    // are about the player's hands or controller (invert, turning speed, rumble, gyro) are left alone.
    enum class Tab { Graphics, Conker, General };
    struct PresetSetting {
        Tab tab;
        std::string id;
        std::optional<recomp::config::ConfigValueVariant> classic, modern; // none: not set by it
    };

    const std::vector<PresetSetting>& preset_settings() {
        static const std::vector<PresetSetting> settings = [] {
            using V = recomp::config::ConfigValueVariant;
            auto e = [](auto value) { return V{ (uint32_t)value }; };
            std::vector<PresetSetting> list{
                { Tab::Graphics, gfx::res_option, e(Resolution::Original), e(Resolution::Auto) },
                { Tab::Graphics, gfx::ar_option, e(AspectRatio::Original), e(AspectRatio::Expand) },
                { Tab::Graphics, gfx::rr_option, e(RefreshRate::Original), e(RefreshRate::Display) },
                { Tab::Graphics, gfx::msaa_option, e(Antialiasing::None), e(Antialiasing::MSAA4X) },
                { Tab::Graphics, gfx::hr_option, e(HUDRatioMode::Original), e(HUDRatioMode::Clamp16x9) },
                { Tab::Graphics, extra::texture_filter, e(TextureFilter::N64), e(TextureFilter::Smooth) },
                { Tab::Graphics, extra::upscale_2d, e(Upscale2D::Original), e(Upscale2D::All) },
                { Tab::Graphics, extra::shading, e(Shading::Original), e(Shading::Smooth) },
                // Only matters below the window's resolution, as in Classic.
                { Tab::Graphics, extra::screen_filter, e(ScreenFilter::Sharp), std::nullopt },
                { Tab::Conker, "skip_intro", V{ false }, V{ true } },
                { Tab::Conker, "free_camera", V{ false }, V{ true } },
                { Tab::Conker, "camera_auto_follow", V{ true }, V{ true } },
                { Tab::General, recompui::config::general::options::mouse_sensitivity, V{ 0.0 }, V{ 50.0 } },
                { Tab::General, "look_mouse_response", e(0u /* Smooth */), e(1u /* Direct */) },
            };
            return list;
        }();
        return settings;
    }

    recomp::config::Config& tab_config(Tab tab) {
        // Looked up each time: the tabs' configs move in memory as tabs are added.
        switch (tab) {
            case Tab::Graphics: return recompui::config::get_graphics_config();
            case Tab::General: return recompui::config::get_general_config();
            case Tab::Conker: default: return recompui::config::get_config("conker");
        }
    }

    const std::optional<recomp::config::ConfigValueVariant>& preset_value(const PresetSetting& setting, Experience experience) {
        return experience == Experience::Modern ? setting.modern : setting.classic;
    }

    bool same_value(const recomp::config::ConfigValueVariant& a, const recomp::config::ConfigValueVariant& b) {
        const double* da = std::get_if<double>(&a);
        const double* db = std::get_if<double>(&b);
        if (da != nullptr && db != nullptr) {
            return std::abs(*da - *db) < 0.5;
        }
        return a == b;
    }

    // Set while the Conker tab's Experience is being changed by us, not the player, and while
    // Classic or Modern is being applied.
    bool syncing_experience = false;

    void apply_experience(Experience experience) {
        if (experience == Experience::Custom) {
            return;
        }
        syncing_experience = true;
        for (Tab tab : { Tab::Graphics, Tab::Conker, Tab::General }) {
            recomp::config::Config& config = tab_config(tab);
            bool changed = false;
            for (const PresetSetting& setting : preset_settings()) {
                const auto& value = preset_value(setting, experience);
                if (setting.tab == tab && value.has_value()) {
                    config.set_option_value(setting.id, *value);
                    changed = true;
                }
            }
            if (changed && tab != Tab::Conker) {
                config.save_config(); // the Graphics tab's also applies it to the renderer
            }
        }
        syncing_experience = false;
        recompui::renderer::refresh_user_config_extension();
    }

    // Which experience the settings match: Custom once the player has changed any of them from
    // what Classic or Modern set.
    Experience matching_experience() {
        for (Experience experience : { Experience::Modern, Experience::Classic }) {
            bool matches = true;
            for (const PresetSetting& setting : preset_settings()) {
                const auto& value = preset_value(setting, experience);
                if (value.has_value()) {
                    matches = matches && same_value(tab_config(setting.tab).get_option_value(setting.id), *value);
                }
            }
            if (matches) {
                return experience;
            }
        }
        return Experience::Custom;
    }

    void sync_experience() {
        if (syncing_experience) {
            return;
        }
        recomp::config::Config& conker = recompui::config::get_config("conker");
        Experience shown = (Experience)std::get<uint32_t>(conker.get_option_value("experience"));
        Experience matching = matching_experience();
        if (shown != matching) {
            syncing_experience = true;
            conker.update_option_value("experience", (uint32_t)matching);
            syncing_experience = false;
        }
    }

    // Keeps the Experience shown in step with the settings as they're applied or loaded (not
    // while they're being tried out before Apply). The runtime runs these after an option's own
    // callbacks (patches/n64modernruntime_conker.patch).
    void watch_settings_for_experience() {
        for (const PresetSetting& setting : preset_settings()) {
            tab_config(setting.tab).add_option_change_callback(setting.id,
                [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
                    if (context != recomp::config::OptionChangeContext::Temporary) {
                        sync_experience();
                    }
                });
        }
    }

    void create_conker_tab() {
        recomp::config::Config& conker = recompui::config::create_config_tab("Conker", "conker", false);
        conker.add_enum_option(
            "experience", "Experience",
            "Classic plays the game as it was on the N64: 4:3, 30 frames per second, the original resolution, "
            "the full intro and the game's own camera. Modern picks the recommended settings: widescreen, your "
            "display's frame rate, full resolution, anti-aliasing, smooth textures and shading, straight to the save menu, "
            "the free camera and mouse control. Custom is shown by itself once you change any of those; picking "
            "Classic or Modern again sets them back. Invert, turning speed, rumble and gyro stay as you set them.",
            { { Experience::Classic, "Classic", "Classic" }, { Experience::Modern, "Modern", "Modern" },
              { Experience::Custom, "Custom", "Custom" } },
            Experience::Modern);
        // Custom can't be picked: it only shows that the Graphics settings are the player's own.
        conker.update_enum_option_disabled("experience", (uint32_t)Experience::Custom, true);
        conker.add_option_change_callback("experience",
            [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
                if (context == recomp::config::OptionChangeContext::Load) {
                    // The saved choice is only a label: show what the Graphics settings match.
                    sync_experience();
                } else if (!syncing_experience) {
                    // The player picked one.
                    apply_experience((Experience)std::get<uint32_t>(cur));
                    recompui::config::get_config("conker").save_config();
                    recompui::config::get_general_config().save_config();
                }
            });
        conker.add_bool_option(
            "skip_intro", "Skip Intro",
            "Skips the opening (the legal screens, the Nintendo logo and the chainsaw scene): the game "
            "starts at the save menu in the bar.",
            false);
        conker.add_option_change_callback("skip_intro",
            [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                skip_intro_enabled = std::get<bool>(cur);
            });
        conker.add_bool_option(
            "free_camera", "Free Camera",
            "The right stick turns the camera freely around Conker and tilts it up and down; it stays where you "
            "leave it. C-Left or C-Right hands the camera back to the game, and cutscenes, special cameras and "
            "aiming keep the game's own. With a mouse, set Mouse Sensitivity (General tab) above 0 to do the same "
            "with the mouse, and the scroll wheel to zoom.",
            true);
        conker.add_option_change_callback("free_camera",
            [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                free_camera = std::get<bool>(cur);
            });
        conker.add_bool_option(
            "camera_auto_follow", "Camera Auto-Follow",
            "With the Free Camera: once you haven't turned the camera for a moment, it eases back behind Conker "
            "while he moves. Off, it stays wherever you leave it.",
            true);
        conker.add_option_change_callback("camera_auto_follow",
            [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                auto_follow = std::get<bool>(cur);
            });
    }

    void describe_controls() {
        using recompinput::GameInput;
        using recompinput::set_game_input_description;
        const char* stick = "Moves Conker, and the cursor in menus.";
        for (GameInput axis : { GameInput::Y_AXIS_POS, GameInput::Y_AXIS_NEG, GameInput::X_AXIS_NEG, GameInput::X_AXIS_POS }) {
            set_game_input_description(axis, stick);
        }
        set_game_input_description(GameInput::A, "Jump (press again in the air to hover with the tail); confirm in menus.");
        set_game_input_description(GameInput::B, "Context-sensitive action, or attack with the current weapon.");
        set_game_input_description(GameInput::Z, "Crouch.");
        set_game_input_description(GameInput::L, "Not used by the game.");
        set_game_input_description(GameInput::R, "Center the camera behind Conker.");
        set_game_input_description(GameInput::START, "Pause; skips some cutscenes.");
        set_game_input_description(GameInput::C_UP, "First-person view.");
        set_game_input_description(GameInput::C_DOWN, "Camera distance.");
        set_game_input_description(GameInput::C_LEFT, "Rotate the camera.");
        set_game_input_description(GameInput::C_RIGHT, "Rotate the camera.");
        for (GameInput pad : { GameInput::DPAD_UP, GameInput::DPAD_DOWN, GameInput::DPAD_LEFT, GameInput::DPAD_RIGHT }) {
            set_game_input_description(pad, "Not used by the game.");
        }
    }
}

void conker::init_settings() {
    std::filesystem::path app_folder = recompui::file::get_app_folder_path();
    if (!app_folder.empty()) {
        std::filesystem::create_directories(app_folder);
    }
    create_conker_tab();
    recompui::config::GeneralTabOptions general{};
    // Rumble strength and the gyro and mouse sensitivities are added by rumble.cpp and
    // look_aim.cpp instead (CBFD-Recompiled), with the same ids, each next to the settings it
    // goes with. Mouse sensitivity defaults to 0, which leaves the mouse and cursor alone.
    general.has_rumble_strength = false;
    general.has_gyro_sensitivity = false;
    general.has_mouse_sensitivity = false;
    recomp::config::Config& general_config = recompui::config::create_general_tab(general);
    conker::rumble::add_options(general_config);
    conker::look_aim::add_options(general_config);
    recompui::config::create_graphics_tab();
    add_extra_graphics_options();
    watch_settings_for_experience();
    describe_controls();
    recompui::config::create_controls_tab();
    recompui::config::create_sound_tab();
    conker::sound::add_volume_options();
    recompui::config::create_mods_tab();
    recompui::config::finalize();
}

// The General tab's Camera: Invert Turning and Camera: Turning Speed (look_aim.cpp, from
// CBFD-Recompiled), which the Free Camera's right stick follows too.
bool conker::camera_inverted() {
    auto value = recompui::config::get_general_config().get_option_value("camera_invert_turning");
    const uint32_t* v = std::get_if<uint32_t>(&value);
    return v != nullptr && *v == 1; // Invert X
}

float conker::camera_turn_speed() {
    auto value = recompui::config::get_general_config().get_option_value("camera_turn_speed");
    const double* v = std::get_if<double>(&value);
    return v != nullptr ? (float)(*v / 100.0) : 1.0f;
}

// Testing aid: CONKER_SKIP_INTRO=1 skips it whatever the setting.
bool conker::camera_auto_follow() {
    return auto_follow;
}

bool conker::free_camera_enabled() {
    return free_camera;
}

bool conker::skip_intro() {
    static const bool forced = std::getenv("CONKER_SKIP_INTRO") != nullptr;
    return skip_intro_enabled || forced;
}
