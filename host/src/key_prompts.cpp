// Keyboard and mouse button prompts (General tab: Button Prompts). Playing with the keyboard and mouse,
// the speech bubbles' N64 button pictures show the keys and mouse buttons they're bound to instead.
//
// The bubbles' text is drawn by func_150417AC. Characters 0xA8 to 0xB1 are button pictures, each a
// sprite (func_150415E0 gives their sizes; the jump table at 0x80098B14 picks the sprite, with its
// animation frame for the stick): 0xA8 A, 0xA9 B, 0xAA C-Left, 0xAB C-Right, 0xAC C-Up, 0xAD C-Down,
// 0xAE Z, 0xAF L, 0xB0 R, 0xB1 the control stick. (0xA1 to 0xA7 are colour codes, 0xB2 to 0xBA swear
// icons, "st", "nd", "rd", "th" and "more...".) Each is set up by func_15094F70 at 0x150420E4: it loads
// the picture (func_1510D0EC keeps the game's textures loaded once, by number) and writes the commands
// that load it for drawing (G_SETTIMG, with its address).
//
// How it's shown: the game draws each button picture as usual (where it goes in the bubble, its size,
// fading in and out with the bubble), but the picture in memory is first replaced by a marker: a plain
// key, with the button's number hidden in its see-through corners. The game draws that; the renderer
// then swaps each marker for a sharp key picture made here from the player's bindings, from a texture
// folder of its own (the settings folder's key_prompts, loaded with the texture packs; RecompFrontend
// patch recompfrontend_keyprompts.patch) by the marker's hash (RT64's, the same every time). Going
// back to the controller, the original pictures are put back.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL.h>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "recompinput/profiles.h"
#include "recompui/renderer.h"
#include "util/file.h"
#include "conker.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

namespace {
    // The button pictures, by character - 0xA8.
    constexpr int button_count = 10;
    enum Button { A, B, CLeft, CRight, CUp, CDown, Z, L, R, Stick };
    struct ButtonInfo {
        const char* name;
        int width, height;       // the picture's size
        int bits;                // 32 or 16 a pixel
        const char* marker_hash; // RT64's hash of its marker (measured: a texture dump of the markers)
    };
    constexpr ButtonInfo buttons[button_count] = {
        { "A", 32, 32, 32, "109b0858f3526780" },
        { "B", 32, 32, 32, "095ea6a04299a6d7" },
        { "C-Left", 32, 32, 16, "27156c65dc6d3aed" },
        { "C-Right", 32, 32, 16, "0455e144d6fc4752" },
        { "C-Up", 32, 32, 16, "61b21c8b289e5287" },
        { "C-Down", 32, 32, 16, "007aa69e558b72cd" },
        { "Z", 16, 32, 16, "72af486f0fc7887a" },
        { "L", 32, 16, 16, "ceff6cbde778fe76" },
        { "R", 32, 16, 16, "06a07d79a2a2aa4e" },
        { "Stick", 32, 32, 32, "f2cd00a1c97d2a07" },
    };

    // --- Which prompts: the option, and for Automatic the device used last ---------------------------

    std::atomic<bool> keyboard_used_last{ false };

    int SDLCALL watch_input(void*, SDL_Event* event) {
        switch (event->type) {
            case SDL_KEYDOWN: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEWHEEL:
                keyboard_used_last = true;
                break;
            case SDL_MOUSEMOTION:
                if (std::abs(event->motion.xrel) + std::abs(event->motion.yrel) > 2) keyboard_used_last = true;
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                keyboard_used_last = false;
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (std::abs(event->caxis.value) > 12000) keyboard_used_last = false;
                break;
        }
        return 1;
    }

    // Testing: CONKER_TEST_DEVICE="seconds:k;seconds:c;..." counts as using the keyboard (k) or a
    // controller (c) from those times (the test runs' scripted input isn't SDL's events).
    void test_device() {
        struct Switch { double at; bool keyboard; };
        static std::vector<Switch> switches = [] {
            std::vector<Switch> list;
            if (const char* text = std::getenv("CONKER_TEST_DEVICE")) {
                std::string all(text);
                size_t pos = 0;
                while (pos < all.size()) {
                    const size_t end = all.find(';', pos);
                    double at = 0; char which = 0;
                    if (std::sscanf(all.substr(pos, end - pos).c_str(), "%lf:%c", &at, &which) == 2) list.push_back({ at, which == 'k' });
                    if (end == std::string::npos) break;
                    pos = end + 1;
                }
            }
            return list;
        }();
        const double t = conker::testing::game_seconds();
        for (const Switch& s : switches) {
            if (t >= s.at) keyboard_used_last = s.keyboard;
        }
    }

    bool keyboard_prompts() {
        test_device();
        switch (conker::button_prompts()) {
            case conker::ButtonPrompts::Keyboard: return true;
            case conker::ButtonPrompts::Controller: return false;
            case conker::ButtonPrompts::Automatic: default: return keyboard_used_last.load();
        }
    }

    // --- The markers in the game's memory ------------------------------------------------------------

    // A marker picture, RGBA, 8 bits a channel: a plain light key with a dark outline, the button's
    // number in the see-through corners.
    std::vector<uint32_t> marker_pixels(int button) {
        const ButtonInfo& info = buttons[button];
        std::vector<uint32_t> pixels((size_t)info.width * info.height);
        const float radius = 0.22f * std::min(info.width, info.height);
        auto inside = [&](int x, int y, float inset) {
            const float cx = std::clamp(x + 0.5f, inset + radius, info.width - inset - radius);
            const float cy = std::clamp(y + 0.5f, inset + radius, info.height - inset - radius);
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            return dx * dx + dy * dy <= radius * radius;
        };
        for (int y = 0; y < info.height; y++) {
            for (int x = 0; x < info.width; x++) {
                uint32_t rgba;
                if (inside(x, y, 2.5f)) rgba = 0xEEEEEEFF;
                else if (inside(x, y, 0.5f)) rgba = 0x101010FF;
                else rgba = ((uint32_t)(button * 8) << 24) | ((uint32_t)(0xA0 + button) << 16) | (0x5Au << 8);
                pixels[(size_t)y * info.width + x] = rgba;
            }
        }
        return pixels;
    }

    // The marker as the game's pixels (32-bit RGBA, or 16-bit RGBA 5551).
    const std::vector<uint32_t>& marker(int button) {
        static const std::vector<std::vector<uint32_t>> markers = [] {
            std::vector<std::vector<uint32_t>> list(button_count);
            for (int b = 0; b < button_count; b++) {
                for (uint32_t p : marker_pixels(b)) {
                    if (buttons[b].bits == 32) {
                        list[b].push_back(p);
                    } else {
                        const uint32_t r = p >> 24, g = (p >> 16) & 0xFF, bl = (p >> 8) & 0xFF, a = p & 0xFF;
                        list[b].push_back(((r >> 3) << 11) | ((g >> 3) << 6) | ((bl >> 3) << 1) | (a ? 1 : 0));
                    }
                }
            }
            return list;
        }();
        return markers[button];
    }

    uint32_t read_pixel(uint8_t* rdram, gpr address, int bits, size_t i) {
        return bits == 32 ? (uint32_t)MEM_W((int32_t)(i * 4), address) : (uint32_t)(uint16_t)MEM_H((int32_t)(i * 2), address);
    }

    void write_pixel(uint8_t* rdram, gpr address, int bits, size_t i, uint32_t value) {
        if (bits == 32) MEM_W((int32_t)(i * 4), address) = (int32_t)value;
        else MEM_H((int32_t)(i * 2), address) = (int16_t)value;
    }

    bool holds_marker(uint8_t* rdram, gpr address, int button) {
        const std::vector<uint32_t>& m = marker(button);
        for (size_t i = 0; i < m.size(); i++) {
            if (read_pixel(rdram, address, buttons[button].bits, i) != m[i]) return false;
        }
        return true;
    }

    // The pictures a marker is over: where, which button, and the original pixels to put back.
    struct Marked { int button; std::vector<uint32_t> original; };
    std::unordered_map<uint32_t, Marked> marked;

    void put_marker(uint8_t* rdram, gpr address, int button) {
        if (holds_marker(rdram, address, button)) {
            return;
        }
        const std::vector<uint32_t>& m = marker(button);
        Marked entry{ button, std::vector<uint32_t>(m.size()) };
        for (size_t i = 0; i < m.size(); i++) {
            entry.original[i] = read_pixel(rdram, address, buttons[button].bits, i);
            write_pixel(rdram, address, buttons[button].bits, i, m[i]);
        }
        marked[(uint32_t)address] = std::move(entry);
    }

    void put_originals_back(uint8_t* rdram) {
        for (auto& [address, entry] : marked) {
            // Only where the marker still is (the game may have loaded something else there since).
            if (holds_marker(rdram, (gpr)(int32_t)address, entry.button)) {
                for (size_t i = 0; i < entry.original.size(); i++) {
                    write_pixel(rdram, (gpr)(int32_t)address, buttons[entry.button].bits, i, entry.original[i]);
                }
            }
        }
        marked.clear();
    }

    // --- The key pictures --------------------------------------------------------------------------

    constexpr int scale = 8; // the key pictures are 8 times the game's pictures

    struct Canvas {
        int width, height;
        std::vector<float> r, g, b, a; // premultiplied
        Canvas(int w, int h) : width(w), height(h), r((size_t)w * h), g((size_t)w * h), b((size_t)w * h), a((size_t)w * h) {}
        void blend(int x, int y, uint32_t rgb, float coverage) {
            if (x < 0 || y < 0 || x >= width || y >= height || coverage <= 0.0f) return;
            const size_t i = (size_t)y * width + x;
            const float c = std::min(coverage, 1.0f);
            r[i] = ((rgb >> 16) & 0xFF) / 255.0f * c + r[i] * (1 - c);
            g[i] = ((rgb >> 8) & 0xFF) / 255.0f * c + g[i] * (1 - c);
            b[i] = (rgb & 0xFF) / 255.0f * c + b[i] * (1 - c);
            a[i] = c + a[i] * (1 - c);
        }
        // A rounded rectangle, smooth-edged.
        void rounded(float x0, float y0, float x1, float y1, float radius, uint32_t rgb) {
            radius = std::max(0.0f, std::min({ radius, (x1 - x0) / 2, (y1 - y0) / 2 }));
            for (int y = (int)std::floor(y0); y <= (int)std::ceil(y1); y++) {
                for (int x = (int)std::floor(x0); x <= (int)std::ceil(x1); x++) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float cx = std::clamp(px, x0 + radius, x1 - radius), cy = std::clamp(py, y0 + radius, y1 - radius);
                    const float outside = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy)) - radius;
                    const float edge = std::min({ px - x0, x1 - px, py - y0, y1 - py }) + 0.5f;
                    blend(x, y, rgb, radius > 0.5f ? 0.5f - outside : std::min(1.0f, edge));
                }
            }
        }
        void triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgb) {
            const int minx = (int)std::floor(std::min({ x0, x1, x2 })), maxx = (int)std::ceil(std::max({ x0, x1, x2 }));
            const int miny = (int)std::floor(std::min({ y0, y1, y2 })), maxy = (int)std::ceil(std::max({ y0, y1, y2 }));
            auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
                const float len = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
                return ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / len;
            };
            const float sign = edge(x0, y0, x1, y1, x2, y2) > 0 ? 1.0f : -1.0f;
            for (int y = miny; y <= maxy; y++) {
                for (int x = minx; x <= maxx; x++) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float d = std::min({ sign * edge(x0, y0, x1, y1, px, py), sign * edge(x1, y1, x2, y2, px, py), sign * edge(x2, y2, x0, y0, px, py) });
                    blend(x, y, rgb, d + 0.5f);
                }
            }
        }
        std::vector<uint8_t> rgba8() const {
            std::vector<uint8_t> out((size_t)width * height * 4);
            for (size_t i = 0; i < (size_t)width * height; i++) {
                const float al = a[i];
                out[i * 4 + 0] = (uint8_t)std::lround(al > 0 ? std::min(1.0f, r[i] / al) * 255 : 0);
                out[i * 4 + 1] = (uint8_t)std::lround(al > 0 ? std::min(1.0f, g[i] / al) * 255 : 0);
                out[i * 4 + 2] = (uint8_t)std::lround(al > 0 ? std::min(1.0f, b[i] / al) * 255 : 0);
                out[i * 4 + 3] = (uint8_t)std::lround(al * 255);
            }
            return out;
        }
    };

    // The font for the keys' names: Luckiest Guy (the launcher's), heavy like the bubbles' lettering.
    struct Font {
        std::vector<uint8_t> data;
        stbtt_fontinfo info{};
        bool ok = false;
    };
    Font& font() {
        static Font f = [] {
            Font loaded;
            std::ifstream file(recompui::file::get_asset_path("LuckiestGuy-Regular.ttf"), std::ios::binary);
            loaded.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            loaded.ok = !loaded.data.empty() && stbtt_InitFont(&loaded.info, loaded.data.data(), stbtt_GetFontOffsetForIndex(loaded.data.data(), 0));
            return loaded;
        }();
        return f;
    }

    // Text centred in a box, as big as fits (up to max_height pixels for its capitals).
    void text(Canvas& canvas, const std::string& label, float cx, float cy, float max_width, float max_height, uint32_t rgb) {
        Font& f = font();
        if (!f.ok || label.empty()) return;
        auto width_at = [&](float px) {
            const float s = stbtt_ScaleForPixelHeight(&f.info, px);
            float w = 0;
            for (size_t i = 0; i < label.size(); i++) {
                int advance, lsb;
                stbtt_GetCodepointHMetrics(&f.info, (unsigned char)label[i], &advance, &lsb);
                w += advance * s;
                if (i + 1 < label.size()) w += stbtt_GetCodepointKernAdvance(&f.info, (unsigned char)label[i], (unsigned char)label[i + 1]) * s;
            }
            return w;
        };
        // Capitals' height, from the H's box, to size and centre by.
        int hx0, hy0, hx1, hy1;
        stbtt_GetCodepointBox(&f.info, 'H', &hx0, &hy0, &hx1, &hy1);
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
        const float cap_ratio = (float)hy1 / (float)(ascent - descent);
        float px = max_height / cap_ratio;
        while (px > 4 && width_at(px) > max_width) px -= 1;
        const float s = stbtt_ScaleForPixelHeight(&f.info, px);
        float x = cx - width_at(px) / 2;
        const float baseline = cy + hy1 * s / 2;
        for (size_t i = 0; i < label.size(); i++) {
            const int c = (unsigned char)label[i];
            int w, h, xoff, yoff;
            const float shift_x = x - std::floor(x);
            unsigned char* bitmap = stbtt_GetCodepointBitmapSubpixel(&f.info, s, s, shift_x, 0, c, &w, &h, &xoff, &yoff);
            for (int yy = 0; yy < h; yy++) {
                for (int xx = 0; xx < w; xx++) {
                    canvas.blend((int)std::floor(x) + xoff + xx, (int)std::lround(baseline) + yoff + yy, rgb, bitmap[yy * w + xx] / 255.0f);
                }
            }
            stbtt_FreeBitmap(bitmap, nullptr);
            int advance, lsb;
            stbtt_GetCodepointHMetrics(&f.info, c, &advance, &lsb);
            x += advance * s;
            if (i + 1 < label.size()) x += stbtt_GetCodepointKernAdvance(&f.info, c, (unsigned char)label[i + 1]) * s;
        }
    }

    constexpr uint32_t outline_colour = 0x141414, top_colour = 0xF7F7F7, side_colour = 0xB9B9B9, label_colour = 0x141414;
    constexpr uint32_t highlight_colour = 0xF28C28; // a mouse button

    // What's on a key: a name, or an arrow (for the arrow keys).
    struct KeyLabel {
        std::string name;
        int arrow = -1; // 0 up, 1 down, 2 left, 3 right
    };

    // A key: outline, its lower edge, its top and the name on it, in the box.
    void key(Canvas& canvas, float x0, float y0, float x1, float y1, const KeyLabel& label) {
        const float size = std::min(x1 - x0, y1 - y0);
        const float line = std::max(1.5f, size * 0.08f), radius = size * 0.22f, lip = size * 0.11f;
        canvas.rounded(x0, y0, x1, y1, radius, outline_colour);
        canvas.rounded(x0 + line, y0 + line, x1 - line, y1 - line, radius - line, side_colour);
        canvas.rounded(x0 + line, y0 + line, x1 - line, y1 - line - lip, radius - line, top_colour);
        const float cx = (x0 + x1) / 2, cy = (y0 + line + y1 - line - lip) / 2;
        const float face_w = x1 - x0 - 2 * line, face_h = y1 - y0 - 2 * line - lip;
        if (label.arrow >= 0) {
            const float s = std::min(face_w, face_h) * 0.27f;
            switch (label.arrow) {
                case 0: canvas.triangle(cx, cy - s, cx - s, cy + s * 0.8f, cx + s, cy + s * 0.8f, label_colour); break;
                case 1: canvas.triangle(cx, cy + s, cx - s, cy - s * 0.8f, cx + s, cy - s * 0.8f, label_colour); break;
                case 2: canvas.triangle(cx - s, cy, cx + s * 0.8f, cy - s, cx + s * 0.8f, cy + s, label_colour); break;
                case 3: canvas.triangle(cx + s, cy, cx - s * 0.8f, cy - s, cx - s * 0.8f, cy + s, label_colour); break;
            }
        } else {
            text(canvas, label.name, cx, cy, face_w * 0.8f, face_h * 0.5f, label_colour);
        }
    }

    // A mouse, the bound button picked out (1 left, 2 middle, 3 right; 4 and 5 the side buttons, numbered).
    void mouse(Canvas& canvas, float x0, float y0, float x1, float y1, int button) {
        const float w = x1 - x0, h = y1 - y0;
        const float mw = std::min(w, h * 0.68f), mh = mw / 0.68f;
        const float mx0 = (x0 + x1 - mw) / 2, my0 = (y0 + y1 - mh) / 2, mx1 = mx0 + mw, my1 = my0 + mh;
        const float line = std::max(1.5f, mw * 0.08f), radius = mw * 0.46f;
        canvas.rounded(mx0, my0, mx1, my1, radius, outline_colour);
        canvas.rounded(mx0 + line, my0 + line, mx1 - line, my1 - line, radius - line, top_colour);
        const float split = my0 + mh * 0.45f, mid = (mx0 + mx1) / 2;
        if (button == 1 || button == 3) {
            const float r = radius - line;
            for (int y = (int)my0; y <= (int)split + 1; y++) {
                for (int x = (int)mx0; x <= (int)mx1; x++) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    if ((button == 1) != (px < mid)) continue;
                    const float cx = std::clamp(px, mx0 + line + r, mx1 - line - r), cy = std::clamp(py, my0 + line + r, my1 - line - r);
                    const float d = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy)) - r;
                    canvas.blend(x, y, highlight_colour, std::min(0.5f - d, split - py + 0.5f));
                }
            }
        }
        canvas.rounded(mid - line / 2, my0 + line, mid + line / 2, split, 0, outline_colour);        // between the buttons
        canvas.rounded(mx0 + line, split - line / 2, mx1 - line, split + line / 2, 0, outline_colour); // under them
        const float wheel = mw * 0.15f;
        canvas.rounded(mid - wheel, my0 + mh * 0.11f, mid + wheel, my0 + mh * 0.34f, wheel, outline_colour);
        canvas.rounded(mid - wheel + line * 0.8f, my0 + mh * 0.11f + line * 0.8f, mid + wheel - line * 0.8f, my0 + mh * 0.34f - line * 0.8f,
            wheel - line, button == 2 ? highlight_colour : top_colour);
        if (button >= 4) {
            text(canvas, button == 4 ? "4" : "5", mid, (split + my1) / 2, mw * 0.5f, mh * 0.22f, label_colour);
        }
    }

    // What an input is bound to on the keyboard and mouse.
    struct Binding {
        bool none = true;
        bool mouse = false;
        int mouse_button = 0;
        KeyLabel label;
    };

    KeyLabel key_label(SDL_Scancode code) {
        switch (code) {
            case SDL_SCANCODE_UP: return { "", 0 };
            case SDL_SCANCODE_DOWN: return { "", 1 };
            case SDL_SCANCODE_LEFT: return { "", 2 };
            case SDL_SCANCODE_RIGHT: return { "", 3 };
            case SDL_SCANCODE_SPACE: return { "SPACE" };
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return { "ENTER" };
            case SDL_SCANCODE_ESCAPE: return { "ESC" };
            case SDL_SCANCODE_BACKSPACE: return { "BKSP" };
            case SDL_SCANCODE_TAB: return { "TAB" };
            case SDL_SCANCODE_CAPSLOCK: return { "CAPS" };
            case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return { "SHIFT" };
            case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return { "CTRL" };
            case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return { "ALT" };
            case SDL_SCANCODE_DELETE: return { "DEL" };
            case SDL_SCANCODE_INSERT: return { "INS" };
            case SDL_SCANCODE_HOME: return { "HOME" };
            case SDL_SCANCODE_END: return { "END" };
            case SDL_SCANCODE_PAGEUP: return { "PG UP" };
            case SDL_SCANCODE_PAGEDOWN: return { "PG DN" };
            default: break;
        }
        std::string name = SDL_GetScancodeName(code);
        if (name.rfind("Keypad ", 0) == 0) name = "NUM " + name.substr(7);
        for (char& c : name) c = (char)std::toupper((unsigned char)c);
        return { name.empty() ? "?" : name };
    }

    Binding binding_for(recompinput::GameInput input) {
        Binding result;
        const int profile = recompinput::profiles::get_sp_keyboard_profile_index();
        if (profile < 0) return result;
        for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
            const recompinput::InputField& field = recompinput::profiles::get_input_binding(profile, input, i);
            if (field.input_type == recompinput::InputType::Keyboard) {
                result.none = false;
                result.label = key_label((SDL_Scancode)field.input_id);
                return result;
            }
            if (field.input_type == recompinput::InputType::Mouse) {
                result.none = false;
                result.mouse = true;
                result.mouse_button = field.input_id;
                return result;
            }
        }
        return result;
    }

    // One button's picture, `scale` times the game's.
    std::vector<uint8_t> button_picture(int button, int& width, int& height) {
        using GI = recompinput::GameInput;
        const ButtonInfo& info = buttons[button];
        width = info.width * scale;
        height = info.height * scale;
        Canvas canvas(width, height);
        const float pad = std::min(width, height) * 0.04f;
        if (button == Stick) {
            // The four movement keys, laid out like WASD.
            const Binding up = binding_for(GI::Y_AXIS_POS), left = binding_for(GI::X_AXIS_NEG),
                down = binding_for(GI::Y_AXIS_NEG), right = binding_for(GI::X_AXIS_POS);
            const float k = (width - 2 * pad) / 3.0f, gap = k * 0.03f;
            const float top = height / 2.0f - k;
            key(canvas, pad + k + gap, top + gap, pad + 2 * k - gap, top + k - gap, up.label);
            key(canvas, pad + gap, top + k + gap, pad + k - gap, top + 2 * k - gap, left.label);
            key(canvas, pad + k + gap, top + k + gap, pad + 2 * k - gap, top + 2 * k - gap, down.label);
            key(canvas, pad + 2 * k + gap, top + k + gap, pad + 3 * k - gap, top + 2 * k - gap, right.label);
            return canvas.rgba8();
        }
        static const GI inputs[] = { GI::A, GI::B, GI::C_LEFT, GI::C_RIGHT, GI::C_UP, GI::C_DOWN, GI::Z, GI::L, GI::R };
        const Binding bound = binding_for(inputs[button]);
        if (bound.mouse) {
            // As tall as a key next to it (the pictures are bigger than the N64 buttons they hold).
            const float h = std::min(width, height) * 0.8f;
            mouse(canvas, pad, (height - h) / 2, width - pad, (height + h) / 2, bound.mouse_button);
            return canvas.rgba8();
        }
        KeyLabel label = bound.label;
        if (bound.none) label.name = info.name; // not bound: the N64 button's name
        // A square key for a short name; a long name gets a wide key (in a square picture, a lower one).
        float kw = width - 2 * pad, kh = height - 2 * pad;
        if (info.width == info.height) {
            if (label.arrow >= 0 || label.name.size() <= 2) kw = kh = std::min(kw, kh) * 0.86f;
            else kh = kw * 0.62f;
        } else if (info.height > info.width) {
            kh = kw * 1.1f; // Z's tall picture: a key, not a tower over the next line
        }
        const float x0 = (width - kw) / 2, y0 = (height - kh) / 2;
        key(canvas, x0, y0, x0 + kw, y0 + kh, label);
        return canvas.rgba8();
    }

    std::vector<uint8_t> png(const std::vector<uint8_t>& rgba, int width, int height) {
        std::vector<uint8_t> out;
        stbi_write_png_to_func([](void* context, void* data, int size) {
            auto* bytes = (std::vector<uint8_t>*)context;
            bytes->insert(bytes->end(), (uint8_t*)data, (uint8_t*)data + size);
        }, &out, width, height, 4, rgba.data(), width * 4);
        return out;
    }

    void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    }

    // --- The texture folder ----------------------------------------------------------------------------

    std::mutex folder_mutex;
    std::string folder_path_utf8; // for conker_extra_texture_directory
    std::string made_for;         // the bindings the pictures show

    std::string bindings_signature() {
        using GI = recompinput::GameInput;
        std::string s;
        for (GI input : { GI::A, GI::B, GI::C_LEFT, GI::C_RIGHT, GI::C_UP, GI::C_DOWN, GI::Z, GI::L, GI::R,
                          GI::Y_AXIS_POS, GI::X_AXIS_NEG, GI::Y_AXIS_NEG, GI::X_AXIS_POS }) {
            const Binding b = binding_for(input);
            s += std::to_string(b.mouse) + ":" + std::to_string(b.mouse_button) + ":" + b.label.name + ":" + std::to_string(b.label.arrow) + ";";
        }
        return s;
    }

    // (Re)makes the folder for the current bindings: the pictures, and rt64.json naming them by the
    // markers' hashes. Returns whether it changed.
    bool make_folder() {
        const std::string signature = bindings_signature();
        if (signature == made_for) {
            return false;
        }
        const std::filesystem::path dir = recomp::get_config_path() / "key_prompts";
        std::error_code ec;
        std::filesystem::create_directories(dir / "textures", ec);
        std::string json = "{\n    \"configuration\": {\n        \"autoPath\": \"rt64\",\n        \"configurationVersion\": 3,\n"
            "        \"hashVersion\": 5,\n        \"defaultOperation\": \"preload\",\n        \"defaultShift\": \"none\"\n    },\n    \"textures\": [\n";
        for (int b = 0; b < button_count; b++) {
            int w = 0, h = 0;
            const std::vector<uint8_t> rgba = button_picture(b, w, h);
            write_file(dir / "textures" / (std::string(buttons[b].marker_hash) + ".png"), png(rgba, w, h));
            json += std::string("        { \"hashes\": { \"rt64\": \"") + buttons[b].marker_hash + "\" }, \"path\": \"textures/" +
                buttons[b].marker_hash + "\" }" + (b + 1 < button_count ? ",\n" : "\n");
        }
        json += "    ]\n}\n";
        write_file(dir / "rt64.json", std::vector<uint8_t>(json.begin(), json.end()));
        made_for = signature;
        std::lock_guard lock(folder_mutex);
        folder_path_utf8 = (const char*)dir.u8string().c_str();
        return true;
    }

    std::atomic<bool> was_keyboard{ false };

    // Whether to show the keys now; going back to the controller, the original pictures are put back.
    bool follow_device(uint8_t* rdram) {
        const bool keyboard = keyboard_prompts();
        if (keyboard != was_keyboard.exchange(keyboard) && !keyboard) {
            put_originals_back(rdram);
        }
        return keyboard;
    }

    gpr glyph_list_start = 0;
    uint8_t glyph_character = 0;

    float as_float(uint32_t bits) {
        float value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
}

// RecompFrontend (recompfrontend_keyprompts.patch): the folder to load with the texture packs.
extern "C" const char* conker_extra_texture_directory() {
    static std::string copy;
    std::lock_guard lock(folder_mutex);
    copy = folder_path_utf8;
    return copy.c_str();
}

// Once SDL is up (frontend.cpp): watch which device is used.
void conker::key_prompts_init() {
    SDL_AddEventWatch(watch_input, nullptr);
}

// About once a second from the UI thread (qol.cpp's update): the pictures follow the bindings; when
// they're made or change, the texture packs are loaded again with them.
void conker::key_prompts_update() {
    if (make_folder()) {
        recompui::renderer::trigger_texture_pack_update();
    }
}

// Testing: the key pictures as files to look at.
void conker::key_prompts_write_pictures(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (int b = 0; b < button_count; b++) {
        int w = 0, h = 0;
        const std::vector<uint8_t> rgba = button_picture(b, w, h);
        write_file(dir / (std::string(buttons[b].name) + ".png"), png(rgba, w, h));
    }
}

// func_150417AC before 0x150420E4, about to set up a button picture's sprite: the character at
// $sp+0x1B7, and in $a0 where func_15094F70 will write its commands. (Testing: CONKER_PROMPT_DEBUG
// logs each one, with the pen at $f20/$f22 and the sprite's settings at $sp+0x19C.)
extern "C" void conker_prompt_glyph(uint8_t* rdram, recomp_context* ctx) {
    const gpr sp = ctx->r29;
    glyph_list_start = ctx->r4;
    glyph_character = (uint8_t)MEM_BU(0x1B7, sp);
    static const bool debug = std::getenv("CONKER_PROMPT_DEBUG") != nullptr;
    if (debug) {
        std::printf("[prompt] %.2fs char %02X pen %.1f %.1f w %d h %d scale %.3f alpha %d\n",
            conker::testing::game_seconds(), glyph_character, as_float(ctx->f20.u32l), as_float(ctx->f22.u32l),
            (int16_t)MEM_HU(0x1A2, sp), (int16_t)MEM_HU(0x1A4, sp), as_float((uint32_t)MEM_W(0x108, sp)), (int)MEM_BU(0x1A6, sp));
    }
}

// Just after func_15094F70 (0x150420EC): the commands it wrote, from $a0 before to $v0 now. Its
// texture load (G_SETTIMG, 0xFD) says where the picture is, and the load block (G_LOADBLOCK, 0xF3) how
// many pixels; on the keyboard, the marker goes there.
extern "C" void conker_prompt_glyph_set_up(uint8_t* rdram, recomp_context* ctx) {
    const bool keyboard = follow_device(rdram);
    if (glyph_list_start == 0 || glyph_character < 0xA8 || glyph_character > 0xB1) {
        glyph_list_start = 0;
        return;
    }
    const int button = glyph_character - 0xA8;
    gpr texture = 0;
    int texels = 0;
    for (gpr cmd = glyph_list_start; cmd < ctx->r2 && cmd < glyph_list_start + 0x400; cmd += 8) {
        const uint32_t w0 = (uint32_t)MEM_W(0, cmd), w1 = (uint32_t)MEM_W(4, cmd);
        if ((w0 >> 24) == 0xFD && texture == 0) texture = (gpr)(int32_t)w1;
        if ((w0 >> 24) == 0xF3 && texels == 0) texels = (int)((w1 >> 12) & 0xFFF) + 1;
    }
    glyph_list_start = 0;
    // Only pictures as expected (their size), in the game's memory.
    if (!keyboard || texture == 0 || ((uint32_t)texture & 0xFF000000u) != 0x80000000u ||
        texels != buttons[button].width * buttons[button].height) {
        return;
    }
    put_marker(rdram, texture, button);
}

// The control stick of the pause menu and the save menu (between PLAY and ERASE, QUIT and CONT...) is
// another picture: numbers 0x7E7 to 0x7EC (its animation, tilting), drawn bigger. The game keeps its
// pictures loaded by number (func_1510D0EC; at its end, 0x1510D360, $s0 the number and $v0 where it
// is, 32 by 32, 32 bits a pixel, in a few places it reuses), and looks them up each time it draws one.
// On the keyboard, the stick's marker goes there too, once the picture's in (it's loaded after the
// look-up): it's then shown as the control stick's keys, as in the speech bubbles.
extern "C" void conker_picture_looked_up(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t number = (uint32_t)ctx->r16, at = (uint32_t)ctx->r2;
    // (Not loaded at all, it gives 0x80000000.)
    if (number < 0x7E7 || number > 0x7EC || at < 0x80100000u || at >= 0x80800000u || !follow_device(rdram)) {
        return;
    }
    // Loaded: pixels the stick covers in all of its pictures are there (row 16 column 16, row 20
    // column 4, row 24 column 28, row 28 column 15).
    const gpr address = (gpr)(int32_t)at;
    for (int pixel : { 16 * 32 + 16, 20 * 32 + 4, 24 * 32 + 28, 28 * 32 + 15 }) {
        if ((MEM_W(pixel * 4, address) & 0xFF) == 0) {
            return;
        }
    }
    put_marker(rdram, address, Stick);
}
