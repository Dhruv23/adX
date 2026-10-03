// adx-thread: main
#include "engine/format/audio/AudioFileLoader.h"

#include <fstream>
#include <memory>
#include <vector>

#include <miniaudio.h>

namespace adx::format {
namespace {

/// Owns a miniaudio decoder for the scope of one decode.
struct DecoderGuard {
    ma_decoder decoder{};
    bool open{false};
    DecoderGuard() = default;
    DecoderGuard(const DecoderGuard&) = delete;
    DecoderGuard& operator=(const DecoderGuard&) = delete;
    DecoderGuard(DecoderGuard&&) = delete;
    DecoderGuard& operator=(DecoderGuard&&) = delete;
    ~DecoderGuard() {
        if (open) {
            ma_decoder_uninit(&decoder);
        }
    }
};

std::string describe(ma_result result) {
    return std::string(ma_result_description(result)) + " (miniaudio " +
           std::to_string(static_cast<int>(result)) + ")";
}

/// Reads every frame of an opened decoder into planar storage.
bool readAll(ma_decoder& decoder, SampleBuffer& out, std::string& error) {
    const ma_uint32 channels = decoder.outputChannels;
    if (channels == 0 || decoder.outputSampleRate == 0) {
        error = "the decoder reports no channels or no sample rate";
        return false;
    }
    out.sampleRate = decoder.outputSampleRate;
    out.sourceChannels = channels;
    out.left.assign(kSampleGuardFrames, 0.0F);
    out.right.clear();
    if (channels > 1) {
        out.right.assign(kSampleGuardFrames, 0.0F);
    }

    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) == MA_SUCCESS && length > 0) {
        out.left.reserve(static_cast<std::size_t>(length) + (2 * kSampleGuardFrames));
        if (channels > 1) {
            out.right.reserve(static_cast<std::size_t>(length) + (2 * kSampleGuardFrames));
        }
    }

    // Folding more than two channels: odd-numbered ones to the left, even to the right,
    // each side averaged, so a quad recording plays as its stereo sum rather than as
    // its first two channels.
    std::vector<float> chunk(static_cast<std::size_t>(4096) * channels);
    // Odd channels fold left, even right: the left takes the extra one.
    const auto rightCount = channels / 2;
    const auto leftCount = channels - rightCount;
    const float leftScale = channels > 2 ? 1.0F / static_cast<float>(leftCount) : 1.0F;
    const float rightScale = channels > 2 ? 1.0F / static_cast<float>(rightCount) : 1.0F;
    for (;;) {
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), 4096, &read);
        for (ma_uint64 f = 0; f < read; ++f) {
            const float* frame = chunk.data() + (f * channels);
            if (channels == 1) {
                out.left.push_back(frame[0]);
                continue;
            }
            float left = 0.0F;
            float right = 0.0F;
            for (ma_uint32 c = 0; c < channels; ++c) {
                ((c % 2 == 0) ? left : right) += frame[c];
            }
            out.left.push_back(left * leftScale);
            out.right.push_back(right * rightScale);
        }
        if (result == MA_AT_END || read == 0) {
            break;
        }
        if (result != MA_SUCCESS) {
            error = "decode failed partway: " + describe(result);
            return false;
        }
    }
    out.frames = out.left.size() - kSampleGuardFrames;
    if (out.frames == 0) {
        error = "the file decodes to no audio";
        return false;
    }
    out.left.resize(out.left.size() + kSampleGuardFrames, 0.0F);
    if (!out.right.empty()) {
        out.right.resize(out.right.size() + kSampleGuardFrames, 0.0F);
    }
    return true;
}

} // namespace

bool decodeFile(const std::filesystem::path& path, SampleBuffer& out, std::string& error) {
    // f32 at the file's own rate and channel count: 0 means "as the file is".
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    DecoderGuard guard;
#ifdef _WIN32
    const ma_result opened = ma_decoder_init_file_w(path.c_str(), &config, &guard.decoder);
#else
    const ma_result opened = ma_decoder_init_file(path.c_str(), &config, &guard.decoder);
#endif
    if (opened != MA_SUCCESS) {
        error = "cannot decode '" + path.string() + "': " + describe(opened);
        return false;
    }
    guard.open = true;
    return readAll(guard.decoder, out, error);
}

bool decodeMemory(std::span<const std::byte> bytes, SampleBuffer& out, std::string& error) {
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    DecoderGuard guard;
    const ma_result opened =
        ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &guard.decoder);
    if (opened != MA_SUCCESS) {
        error = "cannot decode buffer: " + describe(opened);
        return false;
    }
    guard.open = true;
    return readAll(guard.decoder, out, error);
}

bool hashFile(const std::filesystem::path& path, std::uint64_t& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    std::vector<char> chunk(static_cast<std::size_t>(1) << 16U);
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            hash ^= static_cast<std::uint8_t>(chunk[static_cast<std::size_t>(i)]);
            hash *= 0x100000001B3ULL;
        }
    }
    out = hash;
    return true;
}

} // namespace adx::format
