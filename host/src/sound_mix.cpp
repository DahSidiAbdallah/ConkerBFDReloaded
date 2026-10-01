// Separate music, sound-effect and speech volumes, called from hooks in recomp/conker.toml.
//
// Everything the game plays goes through libultra's synthesizer as voices. The music
// comes from sequence players, which work out each of their notes' volumes in
// __n_vsVol(voiceState, seqp): the voice states passed are theirs (each voice sits 4
// bytes into its state). The game has several: 0x8007B820 plays the intro's and the
// bar's music, 0x8007D380 and 0x8007EEE0 the music in the levels (measured: with only
// the first counted as music, the Music slider barely changed the levels). Every other
// voice (the sound player's) is a sound effect. Rare also plays some ambience (a crowd,
// a fire) through the sequence players, so that follows the Music volume. When a voice's volume is set (n_alSynStartVoiceParams, n_alSynSetVol),
// it's scaled by the Sound tab's Music or Sound Effects volume.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_set>

#include "recomp.h"
#include "recompui/config.h"

#include "conker.hpp"

namespace {
    std::mutex music_voices_mutex;
    std::unordered_set<uint32_t> music_voices;

    double sound_percent(const std::string& id) {
        auto value = recompui::config::get_config(recompui::config::sound::id).get_option_value(id);
        const double* v = std::get_if<double>(&value);
        return v != nullptr ? *v / 100.0 : 1.0;
    }

    bool is_music_voice(uint32_t voice) {
        std::lock_guard lock(music_voices_mutex);
        return music_voices.contains(voice - 4);
    }

    int32_t scaled(uint32_t voice, int32_t volume) {
        double scale = sound_percent(is_music_voice(voice) ? conker::sound::music_volume : conker::sound::effects_volume);
        return (int32_t)(int16_t)(volume * scale);
    }
}

// __n_vsVol's entry: $a0 is the voice state, $a1 the sequence player.
extern "C" void conker_note_volume(uint8_t* rdram, recomp_context* ctx) {
    static const bool probe = std::getenv("CONKER_PROBE") != nullptr;
    if (probe) {
        static std::map<uint32_t, uint32_t> counts;
        static uint32_t calls = 0;
        std::lock_guard lock(music_voices_mutex);
        counts[(uint32_t)ctx->r5]++;
        if (++calls % 1000 == 0) {
            std::printf("[notes] room=%02X", (uint32_t)MEM_W(0, (int32_t)0x800BE9F0));
            for (auto& [seqp, n] : counts) std::printf(" %08X:%u", seqp, n);
            std::printf("\n");
            counts.clear();
        }
    }
    {
        std::lock_guard lock(music_voices_mutex);
        music_voices.insert((uint32_t)ctx->r4);
    }
}

// n_alSynStartVoiceParams(voice, wavetable, pitch, vol, ...) entry: $a3 is the volume.
extern "C" void conker_voice_start_volume(uint8_t* rdram, recomp_context* ctx) {
    // CONKER_PROBE_VOICES=1: log each sound started (investigating which ones are speech).
    static const bool probe = std::getenv("CONKER_PROBE_VOICES") != nullptr;
    if (probe) {
        uint32_t wave = (uint32_t)ctx->r5;
        std::printf("[voice] t=%.2f voice=%08X base=%08X len=%d type=%d vol=%d music=%d\n", conker::testing::game_seconds(),
            (uint32_t)ctx->r4, (uint32_t)MEM_W(0, (int32_t)wave), MEM_W(4, (int32_t)wave), (int)MEM_BU(8, (int32_t)wave),
            (int)(int16_t)ctx->r7, (int)is_music_voice((uint32_t)ctx->r4));
    }
    ctx->r7 = scaled((uint32_t)ctx->r4, (int16_t)ctx->r7);
}

// n_alSynSetVol(voice, vol, time) entry: $a1 is the volume.
extern "C" void conker_voice_set_volume(uint8_t* rdram, recomp_context* ctx) {
    ctx->r5 = scaled((uint32_t)ctx->r4, (int16_t)ctx->r5);
}

// func_151F2E88, the streamed sound: its volume, as read to work out the left and right
// levels (whenever a stream starts or its volume changes). The cutscenes stream their
// spoken lines, which follow the Speech volume; the opening's Nintendo logo scene (room
// 0x21) streams its music, which follows the Music volume. (Checked by ear.)
extern "C" int32_t conker_stream_volume(uint8_t* rdram, int32_t volume) {
    constexpr uint32_t logo_scene = 0x21;
    bool music = (uint32_t)MEM_W(0, (int32_t)0x800BE9F0) == logo_scene;
    return (int32_t)(int16_t)(volume * sound_percent(music ? conker::sound::music_volume : conker::sound::speech_volume));
}

void conker::sound::add_volume_options() {
    recomp::config::Config& sound = recompui::config::get_config(recompui::config::sound::id);
    sound.add_percent_number_option(music_volume, "Music Volume",
        "The volume of the music (and some ambience the game plays with it).", 100.0);
    sound.add_percent_number_option(effects_volume, "Sound Effects Volume", "The volume of sound effects.", 100.0);
    sound.add_percent_number_option(speech_volume, "Speech Volume", "The volume of the characters' spoken lines in cutscenes.", 100.0);
}
