// From CBFD-Recompiled V0.1.5 (MIT, Sean Ciaschi).
// N64-style controllers whose SDL mapping makes the C-buttons face buttons (issue #28).
//
// SDL3 (and so sdl2-compat, what Linux distributions ship as SDL2) maps these controllers as
// Switch pads: the C-buttons become Back, X, Y and a trigger or misc button, which the controls
// screen shows as those and one of which (C-Right) SDL2's names can't hold at all. RecompFrontend
// binds the C-buttons to the right stick, so each such mapping is rewritten with its C-buttons as
// the right stick's four directions, the rest of it kept as SDL has it:
// - The Nintendo Switch Online N64 controller, and pads like the 8BitDo 64 in its Switch mode,
//   through the kernel's driver (057e:2019): the C-buttons are buttons 10 (up), 3 (down), 4 (left)
//   and 2 (right). Only a mapping with the kernel driver's layout (A b0, B b1, Start b11) is taken.
// - The 8BitDo 64 in its D-Input mode (2dc8:3019): the C-buttons are two axes, SDL mapping their
//   halves to Back (up), X (down), Y (left) and Misc2 (right). Those two axes become the right stick.
// A mapping that already has a right stick is left alone (SDL2's own on Windows, for instance).

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include <SDL.h>

#include "conker.hpp"

namespace {
    constexpr Uint16 vendor_nintendo = 0x057E, product_n64 = 0x2019;
    constexpr Uint16 vendor_8bitdo = 0x2DC8, product_8bitdo_64 = 0x3019;

    std::vector<std::string> split_mapping(const std::string& mapping) {
        std::vector<std::string> fields;
        size_t start = 0;
        while (start < mapping.size()) {
            size_t end = mapping.find(',', start);
            if (end == std::string::npos) {
                end = mapping.size();
            }
            if (end > start) {
                fields.push_back(mapping.substr(start, end - start));
            }
            start = end + 1;
        }
        return fields;
    }

    // An element's name without its half ("+rightx" is rightx) and the input it's bound to
    // without its half or inversion ("-a3~" is a3).
    std::string element_name(const std::string& field) {
        std::string name = field.substr(0, field.find(':'));
        if (!name.empty() && (name[0] == '+' || name[0] == '-')) {
            name.erase(0, 1);
        }
        return name;
    }

    std::string bound_input(const std::string& field) {
        size_t colon = field.find(':');
        std::string input = colon == std::string::npos ? std::string() : field.substr(colon + 1);
        if (!input.empty() && (input[0] == '+' || input[0] == '-')) {
            input.erase(0, 1);
        }
        if (!input.empty() && input.back() == '~') {
            input.pop_back();
        }
        return input;
    }

    bool has_field(const std::vector<std::string>& fields, const char* field) {
        for (size_t i = 2; i < fields.size(); i++) {
            if (fields[i] == field) {
                return true;
            }
        }
        return false;
    }

    // The input bound to element (as "name:<half>aN"), or empty.
    std::string axis_half_for(const std::vector<std::string>& fields, const char* element, char half) {
        const std::string prefix = std::string(element) + ":" + half + "a";
        for (size_t i = 2; i < fields.size(); i++) {
            if (fields[i].compare(0, prefix.size(), prefix) == 0) {
                return bound_input(fields[i]);
            }
        }
        return {};
    }

    // The mapping with the C-buttons as the right stick, or empty if it isn't one to rewrite.
    std::string rewrite_mapping(Uint16 vendor, Uint16 product, const std::string& mapping) {
        std::vector<std::string> fields = split_mapping(mapping);
        if (fields.size() < 3) {
            return {};
        }
        for (size_t i = 2; i < fields.size(); i++) {
            const std::string name = element_name(fields[i]);
            if (name == "rightx" || name == "righty") {
                return {};
            }
        }

        std::vector<std::string> c_inputs;
        std::string c_stick;
        if (vendor == vendor_nintendo && product == product_n64) {
            if (!has_field(fields, "a:b0") || !has_field(fields, "b:b1") || !has_field(fields, "start:b11")) {
                return {};
            }
            c_inputs = { "b10", "b3", "b4", "b2" };
            c_stick = "-righty:b10,+righty:b3,-rightx:b4,+rightx:b2";
        } else if (vendor == vendor_8bitdo && product == product_8bitdo_64) {
            const std::string vertical = axis_half_for(fields, "back", '-');
            const std::string horizontal = axis_half_for(fields, "y", '-');
            if (vertical.empty() || horizontal.empty() || vertical == horizontal) {
                return {};
            }
            c_inputs = { vertical, horizontal };
            c_stick = "rightx:" + horizontal + ",righty:" + vertical;
        } else {
            return {};
        }

        std::string result = fields[0] + "," + fields[1] + ",";
        for (size_t i = 2; i < fields.size(); i++) {
            const std::string input = bound_input(fields[i]);
            bool on_c = false;
            for (const std::string& c : c_inputs) {
                on_c = on_c || input == c;
            }
            if (!on_c) {
                result += fields[i] + ",";
            }
        }
        return result + c_stick + ",";
    }

    std::atomic<bool> devices_added{ false };
}

void conker::pad_mappings::fix_all() {
    devices_added = false;
    for (int index = 0; index < SDL_NumJoysticks(); index++) {
        char* mapping = SDL_GameControllerMappingForDeviceIndex(index);
        if (mapping == nullptr) {
            continue;
        }
        const std::string fixed = rewrite_mapping(SDL_JoystickGetDeviceVendor(index), SDL_JoystickGetDeviceProduct(index), mapping);
        SDL_free(mapping);
        if (fixed.empty()) {
            continue;
        }
        if (SDL_GameControllerAddMapping(fixed.c_str()) < 0) {
            std::printf("[controller] couldn't remap the C-buttons: %s\n", SDL_GetError());
        } else {
            std::printf("[controller] C-buttons remapped to the right stick: %s\n", fixed.c_str());
        }
    }
    std::fflush(stdout);
}

void conker::pad_mappings::on_device_added() {
    devices_added = true;
}

void conker::pad_mappings::update() {
    if (devices_added) {
        fix_all();
    }
}
