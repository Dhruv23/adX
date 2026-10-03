// Stand-ins for the archive's audio-clip loading, which the A/B reference files do
// not use (suffocation.adx is fully synthesized).
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "AudioClipProcessor.h"
#include "AudioFileLoader.h"

namespace AudioClipProcessor {
void ReprocessClip(AudioClip& /*clip*/) {}
} // namespace AudioClipProcessor

namespace AudioFileLoader {
std::optional<AudioClip> LoadAudioClip(const std::string& path, float /*startTimeSeconds*/) {
    std::cerr << "v1render: audio clip '" << path << "' skipped\n";
    return std::nullopt;
}
std::optional<std::vector<float>> DecodeMono(const std::string& /*path*/, unsigned int /*rate*/) {
    return std::nullopt;
}
} // namespace AudioFileLoader
