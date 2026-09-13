#include "AudioFileLoader.h"
#include "miniaudio.h"

#include <iostream>

namespace AudioFileLoader {

std::optional<AudioClip> LoadAudioClip(const std::string& filePath, float startTimeSeconds) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 2, kEngineSampleRate);

    ma_uint64 frameCount = 0;
    float* pFrames = nullptr;
    ma_result result = ma_decode_file(filePath.c_str(), &config, &frameCount, reinterpret_cast<void**>(&pFrames));

    if (result != MA_SUCCESS) {
        std::cerr << "AudioFileLoader: failed to decode '" << filePath << "' (miniaudio result " << result << ")" << std::endl;
        return std::nullopt;
    }

    AudioClip clip;
    clip.filePath = filePath;
    clip.startTimeSeconds = startTimeSeconds;
    clip.sampleRate = kEngineSampleRate;
    clip.channels = 2;
    clip.originalPcmData = std::make_shared<const std::vector<float>>(pFrames, pFrames + frameCount * clip.channels);
    clip.pcmData = clip.originalPcmData; // fresh import starts at identity pitch/stretch — same buffer, no copy

    ma_free(pFrames, nullptr);

    return clip;
}

std::optional<std::vector<float>> DecodeMono(const std::string& filePath, unsigned int targetSampleRate) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, targetSampleRate);

    ma_uint64 frameCount = 0;
    float* pFrames = nullptr;
    ma_result result = ma_decode_file(filePath.c_str(), &config, &frameCount, reinterpret_cast<void**>(&pFrames));

    if (result != MA_SUCCESS) {
        std::cerr << "AudioFileLoader::DecodeMono: failed to decode '" << filePath << "' (miniaudio result " << result << ")" << std::endl;
        return std::nullopt;
    }

    std::vector<float> samples(pFrames, pFrames + frameCount);
    ma_free(pFrames, nullptr);

    return samples;
}

}
