// adx-thread: main
//
// WAV: 16- and 24-bit PCM and 32-bit float, streaming, with RF64 past 4 GiB
// (phase_8.md §2; EBU TECH 3306).
//
// The header is written first with placeholder sizes and a JUNK chunk exactly the size
// of RF64's ds64 chunk, then patched at finish(). A file that grew past what a RIFF
// header can say becomes RF64 in place - RIFF -> RF64, JUNK -> ds64 - without moving a
// byte of audio. That is the standard way to do it, and it means the writer never has
// to know the length in advance.
#pragma once

#include <cstdio>

#include "engine/render/Encoders.h"

namespace adx::render {

class WavEncoder final : public Encoder {
public:
    ~WavEncoder() override;
    WavEncoder() = default;
    WavEncoder(const WavEncoder&) = delete;
    WavEncoder& operator=(const WavEncoder&) = delete;
    WavEncoder(WavEncoder&&) = delete;
    WavEncoder& operator=(WavEncoder&&) = delete;

    bool open(const std::filesystem::path& path, const EncodeSettings& settings,
              std::string& error) override;
    bool write(std::span<const float> interleaved, std::string& error) override;
    bool finish(std::string& error) override;

    /// Rewrites the header for what has been written so far and carries on: a
    /// recorder calls it every second, so a crash mid-take still leaves a file that
    /// plays up to the last checkpoint (phase_8.md §3.1).
    bool checkpoint(std::string& error);

    [[nodiscard]] std::uint64_t framesWritten() const noexcept {
        return m_frames;
    }

private:
    bool patchHeader(bool padded);
    std::FILE* m_file{nullptr};
    EncodeSettings m_settings;
    std::uint32_t m_bytesPerSample{4};
    std::uint64_t m_dataBytes{0};
    std::uint64_t m_frames{0};
};

} // namespace adx::render
