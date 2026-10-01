// SDL sound output. The game hands over each mixed buffer (osAiSetNextBuffer) and
// paces itself by what's still queued (osAiGetLength), so SDL runs in queue mode.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

#include <SDL.h>

#include "recompui/config.h"
#include "ultramodern/ultramodern.hpp"
#include "conker.hpp"

namespace {
    constexpr int channels = 2;
    std::mutex mutex;
    SDL_AudioDeviceID device = 0;
    std::vector<float> converted;
    uint32_t current_frequency = 22050;
}

void conker::sound::set_frequency(uint32_t frequency) {
    std::lock_guard lock{mutex};
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "[sound] SDL audio init failed: %s\n", SDL_GetError());
        return;
    }
    if (device != 0) {
        SDL_CloseAudioDevice(device);
    }
    current_frequency = frequency;
    SDL_AudioSpec want{};
    want.freq = (int)frequency;
    want.format = AUDIO_F32SYS;
    want.channels = channels;
    want.samples = 256;
    device = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (device == 0) {
        std::fprintf(stderr, "[sound] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(device, 0);
}

void conker::sound::queue_samples(int16_t* samples, size_t count) {
    std::lock_guard lock{mutex};
    if (device == 0) {
        return;
    }
    // RDRAM holds native-endian 32-bit words, so each stereo pair reads back swapped.
    const float scale = recompui::config::sound::get_main_volume() / (100.0f * 32768.0f);
    converted.resize(count);
    for (size_t i = 0; i + 1 < count; i += channels) {
        converted[i + 0] = samples[i + 1] * scale;
        converted[i + 1] = samples[i + 0] * scale;
    }
    // Muted while fast-forwarding (Skip Intro, test runs).
    if (ultramodern::get_speed_multiplier() == 1) {
        SDL_QueueAudio(device, converted.data(), (Uint32)(count * sizeof(float)));
    }
    // CONKER_RECORD_WAV=path: also write what's played to a WAV file (testing).
    static FILE* wav = [] {
        const char* path = SDL_getenv("CONKER_RECORD_WAV");
        FILE* f = path != nullptr ? std::fopen(path, "wb") : nullptr;
        if (f != nullptr) {
            char header[44] = {};
            std::fwrite(header, 1, sizeof(header), f); // filled in as the file grows
        }
        return f;
    }();
    if (wav != nullptr) {
        static uint32_t data_bytes = 0;
        std::vector<int16_t> pcm(count);
        for (size_t i = 0; i < count; i++) {
            pcm[i] = (int16_t)std::clamp(converted[i] * 32767.0f, -32768.0f, 32767.0f);
        }
        std::fwrite(pcm.data(), sizeof(int16_t), count, wav);
        data_bytes += (uint32_t)(count * sizeof(int16_t));
        const uint32_t rate = current_frequency;
        uint32_t riff = 36 + data_bytes, fmt_size = 16, byte_rate = rate * channels * 2, rate32 = rate;
        uint16_t pcm_format = 1, ch = channels, align = channels * 2, bits = 16;
        long end = std::ftell(wav);
        std::fseek(wav, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, wav); std::fwrite(&riff, 4, 1, wav); std::fwrite("WAVEfmt ", 1, 8, wav);
        std::fwrite(&fmt_size, 4, 1, wav); std::fwrite(&pcm_format, 2, 1, wav); std::fwrite(&ch, 2, 1, wav);
        std::fwrite(&rate32, 4, 1, wav); std::fwrite(&byte_rate, 4, 1, wav); std::fwrite(&align, 2, 1, wav);
        std::fwrite(&bits, 2, 1, wav); std::fwrite("data", 1, 4, wav); std::fwrite(&data_bytes, 4, 1, wav);
        std::fseek(wav, end, SEEK_SET);
    }
    // CONKER_PROBE=1: print the output's loudness every two seconds (debugging).
    static const bool probe = SDL_getenv("CONKER_PROBE") != nullptr;
    if (probe) {
        static double sum = 0;
        static size_t n = 0, reports = 0;
        for (size_t i = 0; i < count; i++) {
            sum += (double)converted[i] * converted[i];
        }
        n += count;
        if (n >= 2 * 2 * 22050) {
            std::printf("[loudness] %zus %.5f\n", (++reports) * 2, std::sqrt(sum / n));
            sum = 0;
            n = 0;
        }
    }
}

size_t conker::sound::frames_remaining() {
    std::lock_guard lock{mutex};
    if (device == 0) {
        return 0;
    }
    // On hardware AI_LEN counts only what's left of the buffer playing now. Conker's
    // audio thread shortens its next buffer when much is left, so counting the whole
    // queue makes it underrun: leave one buffer (736 frames) out.
    constexpr uint32_t one_buffer = 736;
    uint32_t queued = SDL_GetQueuedAudioSize(device) / (channels * sizeof(float));
    return queued > one_buffer ? queued - one_buffer : 0;
}
