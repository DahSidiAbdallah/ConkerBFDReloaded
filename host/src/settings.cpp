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
    std::atomic<bool> crosshair_on{ true };
    // qol.cpp's options.
    std::atomic<bool> saving_icon_on{ true }, pause_unfocused_on{ true }, skip_cutscene_on{ false };
    std::atomic<bool> toggle_r_on{ false }, toggle_z_on{ false }, reduce_motion_on{ false }, always_hud_on{ false }, longer_spin_on{ false };
    std::atomic<int> cash_counter_mode{ 0 };

    enum class Experience : uint32_t { Classic, Modern, Custom };

    // Graphics options RT64 has that recompui's Graphics tab doesn't; added to that tab.
    namespace extra {
        const std::string texture_filter = "conker_texture_filter";
        const std::string upscale_2d = "conker_upscale_2d";
        const std::string screen_filter = "conker_screen_filter";
        const std::string shading = "conker_shading";
        const std::string show_fps = "show_fps"; // (fps_counter.cpp)
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
        graphics.add_enum_option(extra::show_fps, "Show FPS",
            "Shows the frame rate in the top-right corner while playing, to spot slowdowns. "
            "<recomp-color primary>FPS</recomp-color> is the frames drawn to the screen each second (with the smooth frame rate, "
            "more than the game makes): a drop there is the PC falling behind. "
            "<recomp-color primary>Game</recomp-color> is the frames the game itself makes, up to 30: a drop there with FPS "
            "steady is the game's own slowdown, as on the N64.",
            { { 0u, "Off", "Off" }, { 1u, "On", "On" } }, 0u);
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
    enum class Tab { Graphics, Conker, Accessibility, General };
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
                { Tab::Conker, "aiming_crosshair", V{ false }, V{ true } },
                { Tab::Conker, "saving_icon", V{ false }, V{ true } },
                { Tab::Accessibility, "pause_unfocused", V{ false }, V{ true } },
                { Tab::Conker, "skip_any_cutscene", V{ false }, V{ true } },
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
            case Tab::Accessibility: return recompui::config::get_config("accessibility");
            case Tab::Conker: default: return recompui::config::get_config("conker");
        }
    }

    const std::optional<recomp::config::ConfigValueVariant>& preset_value(const PresetSetting& setting, Experience experience) {
        return experience == Experience::Modern ? setting.modern : setting.classic;
    }

    // Set while the Conker tab's Experience is being changed by us, not the player, and while
    // Classic or Modern is being applied.
    bool syncing_experience = false;

    // Classic and Modern decide their settings: those are greyed out (locked) while either is picked,
    // and unlocked to choose freely with Custom. MSAA stays greyed out on graphics cards that can't
    // do it (recompui greys it out itself then).
    void lock_preset_settings(Experience experience) {
        for (const PresetSetting& setting : preset_settings()) {
            const bool locked = experience != Experience::Custom && preset_value(setting, experience).has_value();
            if (!locked && setting.id == gfx::msaa_option && !recompui::renderer::RT64SamplePositionsSupported()) {
                continue;
            }
            tab_config(setting.tab).update_option_disabled(setting.id, locked);
        }
    }

    void apply_experience(Experience experience) {
        lock_preset_settings(experience);
        if (experience == Experience::Custom) {
            return;
        }
        syncing_experience = true;
        for (Tab tab : { Tab::Graphics, Tab::Conker, Tab::Accessibility, Tab::General }) {
            recomp::config::Config& config = tab_config(tab);
            bool changed = false;
            for (const PresetSetting& setting : preset_settings()) {
                const auto& value = preset_value(setting, experience);
                if (setting.tab == tab && value.has_value()) {
                    // update_option_value also has the open settings page show the new value
                    // (set_option_value alone changed it unseen: the switches stayed as they were).
                    config.update_option_value(setting.id, *value);
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

    // After every tab has loaded: the saved Experience's settings are set (Classic and Modern decide
    // theirs, including settings added since they were saved) and locked, or unlocked for Custom.
    void enforce_experience() {
        const Experience saved = (Experience)std::get<uint32_t>(recompui::config::get_config("conker").get_option_value("experience"));
        apply_experience(saved);
        if (saved != Experience::Custom) {
            recompui::config::get_config("conker").save_config();
            recompui::config::get_config("accessibility").save_config();
            recompui::config::get_general_config().save_config();
        }
    }

    void create_conker_tab() {
        recomp::config::Config& conker = recompui::config::create_config_tab("Conker", "conker", false);
        conker.add_enum_option(
            "experience", "Experience",
            "Classic plays the game as it was on the N64: 4:3, 30 frames per second, the original resolution, "
            "the full intro and the game's own camera. Modern picks the recommended settings: widescreen, your "
            "display's frame rate, full resolution, anti-aliasing, smooth textures and shading, straight to the save menu, "
            "the free camera, the aiming crosshair and mouse control. While Classic or Modern is picked, the settings it "
            "decides are greyed out; pick Custom to change them yourself. Invert, turning speed, rumble and gyro stay "
            "as you set them.",
            { { Experience::Classic, "Classic", "Classic" }, { Experience::Modern, "Modern", "Modern" },
              { Experience::Custom, "Custom", "Custom" } },
            Experience::Modern);
        conker.add_option_change_callback("experience",
            [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
                if (context == recomp::config::OptionChangeContext::Load) {
                    // The other tabs load after this one: enforce_experience applies it once they have.
                } else if (!syncing_experience) {
                    // The player picked one.
                    apply_experience((Experience)std::get<uint32_t>(cur));
                    recompui::config::get_config("conker").save_config();
                    recompui::config::get_config("accessibility").save_config();
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
        conker.add_bool_option(
            "aiming_crosshair", "Aiming Crosshair",
            "Shows a small crosshair in the middle of the screen while Conker aims to throw something (or aims the "
            "magnum), so you can see where it'll go. The original game has none there. The sniper scope keeps its own.",
            true);
        conker.add_option_change_callback("aiming_crosshair",
            [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                crosshair_on = std::get<bool>(cur);
            });
        auto add_bool = [&conker](const char* id, const char* name, const char* about, bool fallback, std::atomic<bool>* target) {
            conker.add_bool_option(id, name, about, fallback);
            conker.add_option_change_callback(id,
                [target](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                    *target = std::get<bool>(cur);
                });
        };
        add_bool("saving_icon", "Autosave Icon",
            "Shows Conker's head in the bottom-right corner while the game saves (at checkpoints and on the save "
            "menu), as modern games do. The original game shows none.", true, &saving_icon_on);
        add_bool("skip_any_cutscene", "Skip Any Cutscene",
            "Hold L to skip any cutscene, even the first time you see it (the original only lets you skip ones you've "
            "watched before), including the ones it never lets you skip, like the opening. \"Hold to Skip\" shows in "
            "the corner with a ring that fills while you hold; a quick press doesn't skip.", false, &skip_cutscene_on);
    }

    // The Accessibility tab: options that make the game easier to see, play and control.
    void create_accessibility_tab() {
        recomp::config::Config& accessibility = recompui::config::create_config_tab("Accessibility", "accessibility", false);
        auto add_bool = [&accessibility](const char* id, const char* name, const char* about, bool fallback, std::atomic<bool>* target) {
            accessibility.add_bool_option(id, name, about, fallback);
            accessibility.add_option_change_callback(id,
                [target](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                    *target = std::get<bool>(cur);
                });
        };
        add_bool("toggle_r_look", "Toggle R-Look",
            "Press R once to look around and again to stop, instead of holding it.", false, &toggle_r_on);
        add_bool("toggle_crouch", "Toggle Crouch",
            "Press Z once to crouch and again to stand, instead of holding it.", false, &toggle_z_on);
        add_bool("reduce_motion", "Reduce Motion Effects",
            "Turns off the motion blur (the ghostly trails while Conker is drunk at the start of the "
            "game), which can cause motion sickness.", false, &reduce_motion_on);
        add_bool("always_show_hud", "Always Show Health",
            "Keeps Conker's health (the chocolate) on screen all the time during play, instead of only for a few "
            "seconds after it changes.", false, &always_hud_on);
        accessibility.add_enum_option("cash_counter", "Show Cash",
            "Shows how much cash Conker has during play, drawn just as on the pause screen (the wad of bills and the "
            "gold numbers), counting up or down when it changes. The original game only shows it on the pause screen. "
            "When It Changes shows it for a few seconds after you get or spend cash; Always keeps it on screen.",
            { { 0u, "Off", "Off" }, { 1u, "WhenItChanges", "When It Changes" }, { 2u, "Always", "Always" } }, 0u);
        accessibility.add_option_change_callback("cash_counter",
            [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                cash_counter_mode = (int)std::get<uint32_t>(cur);
            });
        add_bool("longer_tail_spin", "Longer Tail Spin",
            "When Conker spins his tail after a jump (press A again in the air), he floats up for longer and glides "
            "down more slowly. Off matches the original game. It lets you glide further than the levels were made for.",
            false, &longer_spin_on);
        add_bool("pause_unfocused", "Pause When Unfocused",
            "Pauses the game while its window isn't the one you're using (after alt-tab, or clicking another window), "
            "and carries on when you come back.", true, &pause_unfocused_on);
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
    create_accessibility_tab();
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
    describe_controls();
    recompui::config::create_controls_tab();
    recompui::config::create_sound_tab();
    conker::sound::add_volume_options();
    recompui::config::create_mods_tab();
    recompui::config::finalize();
    enforce_experience();
}

// The General tab's Camera: Invert Turning and Camera: Turning Speed (look_aim.cpp, from
// CBFD-Recompiled), which the Free Camera's right stick follows too.
bool conker::camera_inverted() {
    auto value = recompui::config::get_general_config().get_option_value("camera_invert_turning");
    const uint32_t* v = std::get_if<uint32_t>(&value);
    return v != nullptr && (*v == 1 || *v == 3); // Invert X, Invert Both
}

bool conker::camera_tilt_inverted() {
    auto value = recompui::config::get_general_config().get_option_value("camera_invert_turning");
    const uint32_t* v = std::get_if<uint32_t>(&value);
    return v != nullptr && (*v == 2 || *v == 3); // Invert Y, Invert Both
}

// The General tab's Mouse: Turn the Camera and Camera: Field of View (look_aim.cpp, from
// CBFD-Recompiled V0.1.5).
bool conker::mouse_turns_camera() {
    auto value = recompui::config::get_general_config().get_option_value("mouse_turns_camera");
    const uint32_t* v = std::get_if<uint32_t>(&value);
    return v == nullptr || *v == 0; // On
}

float conker::camera_field_of_view() {
    auto value = recompui::config::get_general_config().get_option_value("camera_field_of_view_degrees");
    const double* v = std::get_if<double>(&value);
    return v != nullptr ? (float)*v : 50.0f;
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

bool conker::crosshair::enabled() {
    return crosshair_on;
}

bool conker::skip_intro() {
    static const bool forced = std::getenv("CONKER_SKIP_INTRO") != nullptr;
    return skip_intro_enabled || forced;
}

bool conker::fps_counter::enabled() {
    return graphics_value(extra::show_fps) == 1;
}

bool conker::qol::saving_icon() { return saving_icon_on; }
bool conker::qol::pause_unfocused() { return pause_unfocused_on; }
bool conker::qol::skip_any_cutscene() { return skip_cutscene_on; }
bool conker::qol::toggle_r_look() { return toggle_r_on; }
bool conker::qol::toggle_crouch() { return toggle_z_on; }
bool conker::qol::reduce_motion() { return reduce_motion_on; }
bool conker::qol::always_show_hud() { return always_hud_on; }
int conker::qol::cash_counter() { return cash_counter_mode; }
bool conker::qol::longer_spin() { return longer_spin_on; }
