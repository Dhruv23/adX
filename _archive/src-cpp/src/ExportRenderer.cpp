#include "ExportRenderer.h"
#include "AudioEngine.h"
#include "AudioEffect.h"

#include <readerwriterqueue.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>

extern "C" {
#include <layer3.h> // shine MP3 encoder
}
#include <FLAC/stream_encoder.h>

namespace ExportRenderer {

const char* FormatLabel(Format f) {
    switch (f) {
        case Format::Wav16: return "WAV (16-bit PCM)";
        case Format::Wav24: return "WAV (24-bit PCM)";
        case Format::WavFloat32: return "WAV (32-bit float)";
        case Format::Mp3: return "MP3 (320 kbps)";
        case Format::Flac: return "FLAC (16-bit lossless)";
    }
    return "?";
}

const char* FormatExtension(Format f) {
    switch (f) {
        case Format::Wav16:
        case Format::Wav24:
        case Format::WavFloat32: return ".wav";
        case Format::Mp3: return ".mp3";
        case Format::Flac: return ".flac";
    }
    return ".wav";
}

float EstimateContentSeconds(const std::vector<Track>& tracks, float bpm) {
    float endSeconds = 0.0f;
    float secondsPerBeat = 60.0f / std::max(1.0f, bpm);
    for (const auto& track : tracks) {
        for (const auto& note : track.notes) {
            endSeconds = std::max(endSeconds, (note.startBeat + note.lengthBeats) * secondsPerBeat);
        }
        for (const auto& clip : track.audioClips) {
            if (!clip.pcmData || clip.channels == 0) continue;
            float dur = static_cast<float>(clip.pcmData->size() / clip.channels) / static_cast<float>(clip.sampleRate);
            endSeconds = std::max(endSeconds, clip.startTimeSeconds + dur);
        }
    }
    return endSeconds;
}

Request BuildRequest(const SequencerState& state,
                     Format format, const std::string& outputPath, float tailSeconds) {
    Request req;
    req.tracks = state.tracks;
    // Deep-clone every effect: the live audio thread is processing the
    // originals' DSP state concurrently, so the offline engine gets its own.
    for (auto& track : req.tracks) {
        for (auto& fx : track.effects) {
            if (fx) fx = fx->clone();
        }
    }
    req.patches = state.patches;
    req.automation = state.automation; // Phase 2
    req.bpm = state.bpm.load();
    req.masterVolume = state.masterVolume.load();
    req.tuning = state.tuning.load();
    req.masterFx = state.masterFx;
    req.format = format;
    req.outputPath = outputPath;
    req.tailSeconds = tailSeconds;
    return req;
}

// --- Encoders -------------------------------------------------------------

namespace {

void put32(std::FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void put16(std::FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

// Minimal canonical WAV writer for 16-bit PCM, 24-bit PCM, and 32-bit float.
bool WriteWav(const std::string& path, const std::vector<float>& interleaved,
              unsigned int sampleRate, Format fmt, uint64_t& bytesOut) {
    const uint16_t channels = 2;
    uint16_t bitsPerSample = fmt == Format::Wav16 ? 16 : (fmt == Format::Wav24 ? 24 : 32);
    uint16_t audioFormat = fmt == Format::WavFloat32 ? 3 /*IEEE float*/ : 1 /*PCM*/;
    uint16_t blockAlign = channels * bitsPerSample / 8;
    uint32_t dataSize = static_cast<uint32_t>(interleaved.size() * bitsPerSample / 8);
    bool needFact = audioFormat == 3;
    uint32_t riffSize = 4 + (8 + 16) + (needFact ? 8 + 4 : 0) + (8 + dataSize);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    std::fwrite("RIFF", 1, 4, f); put32(f, riffSize); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); put32(f, 16);
    put16(f, audioFormat); put16(f, channels); put32(f, sampleRate);
    put32(f, sampleRate * blockAlign); put16(f, blockAlign); put16(f, bitsPerSample);
    if (needFact) {
        std::fwrite("fact", 1, 4, f); put32(f, 4);
        put32(f, static_cast<uint32_t>(interleaved.size() / channels));
    }
    std::fwrite("data", 1, 4, f); put32(f, dataSize);

    if (fmt == Format::WavFloat32) {
        std::fwrite(interleaved.data(), 4, interleaved.size(), f);
    } else if (fmt == Format::Wav16) {
        std::vector<int16_t> pcm(interleaved.size());
        for (size_t i = 0; i < interleaved.size(); ++i) {
            pcm[i] = static_cast<int16_t>(std::lround(std::clamp(interleaved[i], -1.0f, 1.0f) * 32767.0f));
        }
        std::fwrite(pcm.data(), 2, pcm.size(), f);
    } else { // Wav24: pack 3 little-endian bytes per sample
        std::vector<uint8_t> pcm(interleaved.size() * 3);
        for (size_t i = 0; i < interleaved.size(); ++i) {
            int32_t v = static_cast<int32_t>(std::lround(std::clamp(interleaved[i], -1.0f, 1.0f) * 8388607.0));
            pcm[i * 3 + 0] = static_cast<uint8_t>(v & 0xFF);
            pcm[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            pcm[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        }
        std::fwrite(pcm.data(), 1, pcm.size(), f);
    }

    bytesOut = static_cast<uint64_t>(riffSize) + 8;
    return std::fclose(f) == 0;
}

bool WriteMp3(const std::string& path, const std::vector<float>& interleaved,
              unsigned int sampleRate, uint64_t& bytesOut, std::string& error) {
    shine_config_t config;
    shine_set_config_mpeg_defaults(&config.mpeg);
    config.wave.channels = PCM_STEREO;
    config.wave.samplerate = static_cast<int>(sampleRate);
    config.mpeg.mode = STEREO;
    config.mpeg.bitr = 320;

    if (shine_check_config(config.wave.samplerate, config.mpeg.bitr) < 0) {
        error = "shine: unsupported samplerate/bitrate combination";
        return false;
    }
    shine_t enc = shine_initialise(&config);
    if (!enc) {
        error = "shine: initialisation failed";
        return false;
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        shine_close(enc);
        error = "could not open output file";
        return false;
    }

    const size_t framesPerPass = static_cast<size_t>(shine_samples_per_pass(enc)); // per channel
    const size_t totalFrames = interleaved.size() / 2;
    std::vector<int16_t> pass(framesPerPass * 2, 0);
    bytesOut = 0;

    for (size_t offset = 0; offset < totalFrames; offset += framesPerPass) {
        size_t n = std::min(framesPerPass, totalFrames - offset);
        for (size_t i = 0; i < n * 2; ++i) {
            pass[i] = static_cast<int16_t>(std::lround(std::clamp(interleaved[offset * 2 + i], -1.0f, 1.0f) * 32767.0f));
        }
        // Final short pass is zero-padded to a full MP3 granule
        std::fill(pass.begin() + n * 2, pass.end(), static_cast<int16_t>(0));

        int written = 0;
        unsigned char* data = shine_encode_buffer_interleaved(enc, pass.data(), &written);
        if (written > 0) { std::fwrite(data, 1, written, f); bytesOut += written; }
    }

    int written = 0;
    unsigned char* data = shine_flush(enc, &written);
    if (written > 0) { std::fwrite(data, 1, written, f); bytesOut += written; }

    shine_close(enc);
    return std::fclose(f) == 0;
}

bool WriteFlac(const std::string& path, const std::vector<float>& interleaved,
               unsigned int sampleRate, uint64_t& bytesOut, std::string& error) {
    FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
    if (!enc) {
        error = "FLAC: encoder allocation failed";
        return false;
    }

    const size_t totalFrames = interleaved.size() / 2;
    FLAC__stream_encoder_set_channels(enc, 2);
    FLAC__stream_encoder_set_bits_per_sample(enc, 16);
    FLAC__stream_encoder_set_sample_rate(enc, sampleRate);
    FLAC__stream_encoder_set_compression_level(enc, 5);
    FLAC__stream_encoder_set_total_samples_estimate(enc, totalFrames);

    if (FLAC__stream_encoder_init_file(enc, path.c_str(), nullptr, nullptr) !=
        FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        error = std::string("FLAC: init failed (") +
                FLAC__stream_encoder_get_resolved_state_string(enc) + ")";
        FLAC__stream_encoder_delete(enc);
        return false;
    }

    constexpr size_t kChunkFrames = 4096;
    std::vector<FLAC__int32> chunk(kChunkFrames * 2);
    bool ok = true;
    for (size_t offset = 0; ok && offset < totalFrames; offset += kChunkFrames) {
        size_t n = std::min(kChunkFrames, totalFrames - offset);
        for (size_t i = 0; i < n * 2; ++i) {
            chunk[i] = static_cast<FLAC__int32>(std::lround(std::clamp(interleaved[offset * 2 + i], -1.0f, 1.0f) * 32767.0f));
        }
        ok = FLAC__stream_encoder_process_interleaved(enc, chunk.data(), static_cast<uint32_t>(n));
    }
    ok = FLAC__stream_encoder_finish(enc) && ok;
    FLAC__stream_encoder_delete(enc);

    if (!ok) {
        error = "FLAC: encoding failed";
        return false;
    }
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f) { std::fseek(f, 0, SEEK_END); bytesOut = static_cast<uint64_t>(std::ftell(f)); std::fclose(f); }
    return true;
}

} // namespace

// --- Offline render -------------------------------------------------------

Result Render(const Request& request, std::atomic<float>* progress) {
    Result result;

    float contentSeconds = EstimateContentSeconds(request.tracks, request.bpm);
    if (contentSeconds <= 0.0f) {
        result.message = "Nothing to export: project has no notes or clips.";
        return result;
    }
    float totalSeconds = contentSeconds + std::clamp(request.tailSeconds, 0.0f, 30.0f);
    const uint64_t totalFrames = static_cast<uint64_t>(totalSeconds * kEngineSampleRate);

    // A fresh engine + queue, driven exactly like the realtime one but by
    // this thread. The engine takes ownership of the new'd patch/track
    // pointers (its destructor releases whatever is active at the end).
    moodycamel::ReaderWriterQueue<AudioEvent> queue(64);
    std::atomic<float> playhead{0.0f};
    AudioEngine engine(queue, kEngineSampleRate, playhead);

    {
        AudioEvent evt{};
        evt.type = AudioEventType::SequenceUpdate;
        auto* snapshot = new SequenceSnapshot();
        snapshot->tracks = request.tracks;
        snapshot->patches = request.patches;
        snapshot->automation = request.automation; // Phase 2
        evt.data.sequence = snapshot;
        queue.enqueue(evt);

        evt = {};
        evt.type = AudioEventType::BpmChange;
        evt.data.bpmState.bpm = request.bpm;
        queue.enqueue(evt);

        evt = {};
        evt.type = AudioEventType::MasterVolChange;
        evt.data.masterVol.volume = request.masterVolume;
        queue.enqueue(evt);

        evt = {};
        evt.type = AudioEventType::GlobalTuningChange;
        evt.data.globalTuning.tuning = request.tuning;
        queue.enqueue(evt);

        auto param = [&](EngineParam id, float value) {
            AudioEvent p{};
            p.type = AudioEventType::ParameterChange;
            p.data.paramData.paramId = static_cast<uint32_t>(id);
            p.data.paramData.value = value;
            queue.enqueue(p);
        };
        param(EngineParam::DelayTimeMs, request.masterFx.delayTimeMs);
        param(EngineParam::DelayFeedback, request.masterFx.delayFeedback);
        param(EngineParam::DelayMix, request.masterFx.delayMix);
        param(EngineParam::ReverbRoom, request.masterFx.reverbRoom);
        param(EngineParam::ReverbDamp, request.masterFx.reverbDamp);
        param(EngineParam::ReverbMix, request.masterFx.reverbMix);
        param(EngineParam::SidechainEnabled, request.masterFx.sidechainEnabled);
        param(EngineParam::SidechainAmount, request.masterFx.sidechainAmount);
        param(EngineParam::SidechainReleaseMs, request.masterFx.sidechainReleaseMs);
        param(EngineParam::MasterDrive, request.masterFx.masterDrive);

        evt = {};
        evt.type = AudioEventType::PlayStateChange;
        evt.data.playState.isPlaying = true;
        queue.enqueue(evt);
    }

    std::vector<float> mix(totalFrames * 2, 0.0f);
    constexpr unsigned int kChunk = 512;
    for (uint64_t offset = 0; offset < totalFrames; offset += kChunk) {
        unsigned int n = static_cast<unsigned int>(std::min<uint64_t>(kChunk, totalFrames - offset));
        AudioEngine::audioCallback(mix.data() + offset * 2, nullptr, n, 0.0, 0, &engine);
        if (progress) progress->store(static_cast<float>(offset) / static_cast<float>(totalFrames));
    }
    if (progress) progress->store(1.0f);

    uint64_t bytesOut = 0;
    std::string error;
    bool ok = false;
    switch (request.format) {
        case Format::Wav16:
        case Format::Wav24:
        case Format::WavFloat32:
            ok = WriteWav(request.outputPath, mix, kEngineSampleRate, request.format, bytesOut);
            if (!ok) error = "could not write WAV file";
            break;
        case Format::Mp3:
            ok = WriteMp3(request.outputPath, mix, kEngineSampleRate, bytesOut, error);
            break;
        case Format::Flac:
            ok = WriteFlac(request.outputPath, mix, kEngineSampleRate, bytesOut, error);
            break;
    }

    std::ostringstream msg;
    if (ok) {
        msg << "Exported " << totalSeconds << "s (" << (bytesOut / 1024) << " KB) to " << request.outputPath;
    } else {
        msg << "Export failed: " << (error.empty() ? "unknown error" : error);
    }
    result.success = ok;
    result.message = msg.str();
    return result;
}

}
