// adx-thread: main
#include "engine/render/Encoders.h"

#include <algorithm>
#include <cmath>

#include "engine/render/FlacEncoder.h"
#include "engine/render/Mp3Encoder.h"
#include "engine/render/OggEncoder.h"
#include "engine/render/WavEncoder.h"

namespace adx::render {

const char* toString(FileFormat format) noexcept {
    switch (format) {
    case FileFormat::Wav16:
        return "wav16";
    case FileFormat::Wav24:
        return "wav24";
    case FileFormat::Wav32f:
        return "wav32f";
    case FileFormat::Mp3:
        return "mp3";
    case FileFormat::Flac:
        return "flac";
    case FileFormat::Ogg:
        return "ogg";
    }
    return "";
}

bool fileFormatFromString(std::string_view name, FileFormat& out) noexcept {
    if (name == "wav") {
        out = FileFormat::Wav24;
        return true;
    }
    for (const FileFormat format : {FileFormat::Wav16, FileFormat::Wav24, FileFormat::Wav32f,
                                    FileFormat::Mp3, FileFormat::Flac, FileFormat::Ogg}) {
        if (name == toString(format)) {
            out = format;
            return true;
        }
    }
    return false;
}

const char* extensionOf(FileFormat format) noexcept {
    switch (format) {
    case FileFormat::Mp3:
        return ".mp3";
    case FileFormat::Flac:
        return ".flac";
    case FileFormat::Ogg:
        return ".ogg";
    case FileFormat::Wav16:
    case FileFormat::Wav24:
    case FileFormat::Wav32f:
        break;
    }
    return ".wav";
}

std::uint32_t integerBits(FileFormat format, std::uint32_t flacBits) noexcept {
    switch (format) {
    case FileFormat::Wav16:
        return 16;
    case FileFormat::Wav24:
        return 24;
    case FileFormat::Flac:
        return flacBits;
    case FileFormat::Wav32f:
    case FileFormat::Mp3:
    case FileFormat::Ogg:
        break;
    }
    return 0;
}

std::int32_t toInteger(float sample, std::uint32_t bits) noexcept {
    const double scale = std::ldexp(1.0, static_cast<int>(bits) - 1);
    const double value = std::nearbyint(static_cast<double>(sample) * scale);
    return static_cast<std::int32_t>(std::clamp(value, -scale, scale - 1.0));
}

std::unique_ptr<Encoder> makeEncoder(FileFormat format) {
    switch (format) {
    case FileFormat::Mp3:
        return std::make_unique<Mp3Encoder>();
    case FileFormat::Flac:
        return std::make_unique<FlacEncoder>();
    case FileFormat::Ogg:
        return std::make_unique<OggEncoder>();
    case FileFormat::Wav16:
    case FileFormat::Wav24:
    case FileFormat::Wav32f:
        break;
    }
    return std::make_unique<WavEncoder>();
}

bool encodeFile(const std::filesystem::path& path, std::span<const float> interleaved,
                const EncodeSettings& settings, std::string& error) {
    const std::unique_ptr<Encoder> encoder = makeEncoder(settings.format);
    if (!encoder->open(path, settings, error)) {
        return false;
    }
    // In blocks, as an export streams them.
    constexpr std::size_t kBlockFrames = 65536;
    const std::size_t block = kBlockFrames * std::max<std::uint32_t>(settings.channels, 1);
    for (std::size_t at = 0; at < interleaved.size(); at += block) {
        if (!encoder->write(interleaved.subspan(at, std::min(block, interleaved.size() - at)),
                            error)) {
            return false;
        }
    }
    return encoder->finish(error);
}

} // namespace adx::render
