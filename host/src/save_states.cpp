// Save states: a testing aid only, not a feature players see. Off unless one of these is there:
// - a file named savestates_enabled in the settings folder: F5 saves the game as it is to the current
//   slot, F7 loads it, F6 picks the next slot (1 to 9); the states are in the savestates folder there;
// - CONKER_TEST_STATE_SAVE=seconds:file (test runs): the game is saved to that file at that game time;
// - CONKER_TEST_STATE_LOAD=file (test runs): that state is loaded as soon as the game can, so a test
//   starts right where it was taken instead of playing up to it.
//
// From CBFD-Recompiled V0.1.6 (MIT, its issue #94), which has the runtime's side
// (patches/n64modernruntime_savestates.patch: ultramodern/save_states.hpp tells how a recompiled game
// can be saved at all). A state is taken at a safe point, when every game thread but the running one
// waits, and loaded at a safe point where every thread is where it was in the state; so a request is
// done at the next such moment, within a frame or so. The game's own save file (the EEPROM) isn't in a
// state. What happens is written to savestates_log.txt beside the settings, and printed.
//
// The file: "CBFDSTAT", the file's version (u32), the program's version (u32 length, text), when it was
// taken (u64, seconds since 1970), then the runtime's state (u32 length, bytes).

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>

#include "librecomp/game.hpp"
#include "librecomp/save_states.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/save_states.hpp"

#include "conker.hpp"

namespace {
    using clock = std::chrono::steady_clock;

    constexpr char file_magic[8] = { 'C', 'B', 'F', 'D', 'S', 'T', 'A', 'T' };
    constexpr uint32_t file_version = 1;
    constexpr int slot_count = 9;
    // How long a save or a load keeps trying for a moment it can be done at (a test's load longer: the
    // game may still be starting).
    constexpr auto give_up_after = std::chrono::seconds(5);
    constexpr auto test_load_give_up_after = std::chrono::seconds(30);

    enum class Request { None, Save, Load };

    std::atomic<Request> request = Request::None;
    std::atomic<int> current_slot = 1;
    std::filesystem::path request_path;
    clock::time_point requested_at;
    std::chrono::seconds request_patience = give_up_after;
    std::vector<uint8_t> load_state;
    bool logged_not_now = false;
    std::string last_failure;
    uint32_t attempts = 0;

    bool keys_enabled = false;
    double test_save_at = -1.0;
    std::filesystem::path test_save_path;
    std::filesystem::path test_load_path;
    bool test_save_done = false, test_load_done = false;

    std::filesystem::path folder() {
        return recomp::get_config_path() / "savestates";
    }

    std::filesystem::path slot_path(int slot) {
        return folder() / ("slot" + std::to_string(slot) + ".state");
    }

    std::mutex log_mutex;
    void log_line(const char* format, ...) {
        static FILE* file = std::fopen((recomp::get_config_path() / "savestates_log.txt").string().c_str(), "w");
        std::lock_guard lock{log_mutex};
        va_list args;
        va_start(args, format);
        char line[4096];
        std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        std::printf("[savestate] %s\n", line);
        std::fflush(stdout);
        if (file != nullptr) {
            std::fprintf(file, "%s\n", line);
            std::fflush(file);
        }
    }

    template <typename T> void put(std::vector<uint8_t>& out, const T& value) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }

    void put_block(std::vector<uint8_t>& out, const void* data, size_t size) {
        put(out, (uint32_t)size);
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + size);
    }

    // Written on a thread of its own (a state is 8MB), to a temporary file first, so a file is never
    // left half written.
    std::mutex write_mutex;
    void write_state(std::filesystem::path path, std::vector<uint8_t> state) {
        std::thread([path = std::move(path), state = std::move(state)]() {
            std::lock_guard lock{write_mutex};
            std::vector<uint8_t> out;
            out.reserve(state.size() + 256);
            out.insert(out.end(), file_magic, file_magic + sizeof(file_magic));
            put(out, file_version);
            const std::string version = recomp::get_project_version().to_string();
            put_block(out, version.data(), version.size());
            put(out, (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            put_block(out, state.data(), state.size());
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::filesystem::path temporary = path;
            temporary += ".new";
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
                if (!file.good()) {
                    log_line("couldn't write %s", temporary.string().c_str());
                    return;
                }
            }
            std::filesystem::rename(temporary, path, ec);
            if (ec) {
                log_line("couldn't replace %s (%s)", path.string().c_str(), ec.message().c_str());
                return;
            }
            log_line("written %s (%zu bytes)", path.string().c_str(), out.size());
        }).detach();
    }

    // Reads a state file. False, with why, if there's none or it isn't one.
    bool read_state(const std::filesystem::path& path, std::vector<uint8_t>& state, std::string& version, std::string& error) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            error = "no state at " + path.string();
            return false;
        }
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        size_t at = 0;
        auto get = [&](void* out, size_t size) {
            if (at + size > data.size()) {
                return false;
            }
            std::memcpy(out, data.data() + at, size);
            at += size;
            return true;
        };
        auto get_block = [&](std::vector<uint8_t>& out) {
            uint32_t size = 0;
            if (!get(&size, sizeof(size)) || at + size > data.size()) {
                return false;
            }
            out.assign(data.begin() + at, data.begin() + at + size);
            at += size;
            return true;
        };
        char magic[sizeof(file_magic)] = {};
        uint32_t version_number = 0;
        uint64_t taken = 0;
        std::vector<uint8_t> version_bytes;
        if (!get(magic, sizeof(magic)) || std::memcmp(magic, file_magic, sizeof(magic)) != 0 ||
            !get(&version_number, sizeof(version_number)) || version_number != file_version ||
            !get_block(version_bytes) || !get(&taken, sizeof(taken)) || !get_block(state)) {
            error = path.string() + " isn't a state this version can load";
            return false;
        }
        version.assign(version_bytes.begin(), version_bytes.end());
        return true;
    }

    void finish() {
        load_state.clear();
        load_state.shrink_to_fit();
        request = Request::None;
        ultramodern::save_states::set_safe_points_wanted(false);
    }

    bool gave_up(const char* what) {
        if (clock::now() - requested_at < request_patience) {
            return false;
        }
        log_line("%s %s: gave up after %u tries: %s", what, request_path.string().c_str(), attempts, last_failure.c_str());
        return true;
    }

    void on_safe_point(uint8_t* rdram) {
        const Request pending = request.load();
        if (pending == Request::None) {
            return;
        }
        attempts++;
        const char* what = pending == Request::Save ? "save" : "load";
        // Only with nothing on its way to the game; checked again, with the timers held, as it's done.
        if (!ultramodern::save_states::is_quiet()) {
            last_failure = "the game was busy";
            if (gave_up(what)) {
                finish();
            }
            return;
        }
        if (pending == Request::Save) {
            std::vector<uint8_t> state;
            std::string error;
            if (!recomp::save_states::capture(rdram, state, error)) {
                last_failure = error;
                if (gave_up(what)) {
                    finish();
                }
                return;
            }
            log_line("saved after %u tries (game time %.2f)", attempts, conker::testing::game_seconds());
            write_state(request_path, std::move(state));
            finish();
            return;
        }
        std::string error;
        switch (recomp::save_states::restore(rdram, load_state, error)) {
            case recomp::save_states::LoadResult::Loaded:
                log_line("loaded %s after %u tries", request_path.string().c_str(), attempts);
                conker::skip_intro_cancel();
                finish();
                break;
            case recomp::save_states::LoadResult::NotNow:
                if (!logged_not_now) {
                    logged_not_now = true;
                    log_line("load %s not now: %s\n%s%s", request_path.string().c_str(), error.c_str(),
                        recomp::save_states::describe_threads(rdram, load_state).c_str(),
                        recomp::save_states::describe_current_threads(rdram).c_str());
                }
                last_failure = error;
                if (gave_up(what)) {
                    finish();
                }
                break;
            case recomp::save_states::LoadResult::Invalid:
                log_line("load %s: %s", request_path.string().c_str(), error.c_str());
                finish();
                break;
        }
    }

    void start(Request what, std::filesystem::path path, std::chrono::seconds patience) {
        request_path = std::move(path);
        request_patience = patience;
        requested_at = clock::now();
        attempts = 0;
        last_failure.clear();
        logged_not_now = false;
        request = what;
        ultramodern::save_states::set_safe_points_wanted(true);
    }

    void ask_to_save(std::filesystem::path path) {
        if (request.load() != Request::None) {
            return;
        }
        start(Request::Save, std::move(path), give_up_after);
    }

    void ask_to_load(std::filesystem::path path, std::chrono::seconds patience) {
        if (request.load() != Request::None) {
            return;
        }
        std::string version, error;
        if (!read_state(path, load_state, version, error)) {
            log_line("%s", error.c_str());
            return;
        }
        log_line("read %s: from version %s, %s", path.string().c_str(), version.c_str(),
            recomp::save_states::same_build(load_state) ? "this build" : "another build");
        start(Request::Load, std::move(path), patience);
    }

    int SDLCALL watch_keys(void*, SDL_Event* event) {
        if (event->type != SDL_KEYDOWN || event->key.repeat != 0 || !ultramodern::is_game_started()) {
            return 0;
        }
        switch (event->key.keysym.sym) {
            case SDLK_F5:
                ask_to_save(slot_path(current_slot));
                break;
            case SDLK_F7:
                ask_to_load(slot_path(current_slot), give_up_after);
                break;
            case SDLK_F6:
                current_slot = current_slot % slot_count + 1;
                log_line("slot %d", current_slot.load());
                break;
            default:
                break;
        }
        return 0;
    }
}

void conker::save_states::init() {
    keys_enabled = std::filesystem::exists(recomp::get_config_path() / "savestates_enabled");
    if (const char* s = std::getenv("CONKER_TEST_STATE_SAVE")) {
        const char* colon = std::strchr(s, ':');
        if (colon != nullptr) {
            test_save_at = std::atof(s);
            test_save_path = std::filesystem::path(colon + 1);
        }
    }
    if (const char* l = std::getenv("CONKER_TEST_STATE_LOAD")) {
        test_load_path = std::filesystem::path(l);
    }
    if (!keys_enabled && test_save_path.empty() && test_load_path.empty()) {
        return;
    }
    ultramodern::save_states::set_safe_point_callback(on_safe_point);
    if (keys_enabled) {
        SDL_AddEventWatch(watch_keys, nullptr);
    }
}

// Every VI: a test run's save or load, once the game is running.
void conker::save_states::on_vi() {
    if (!ultramodern::is_game_started()) {
        return;
    }
    if (!test_load_path.empty() && !test_load_done) {
        test_load_done = true;
        ask_to_load(test_load_path, test_load_give_up_after);
    }
    if (!test_save_path.empty() && !test_save_done && conker::testing::game_seconds() >= test_save_at) {
        test_save_done = true;
        ask_to_save(test_save_path);
    }
}
