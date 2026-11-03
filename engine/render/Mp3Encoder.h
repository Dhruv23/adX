// adx-thread: main
//
// MP3 through shine, ported from the archive's ExportRenderer (phase_8.md §4).
//
// shine is a fixed-point MPEG-1/2/2.5 layer III encoder: small, dependency-free, and
// constant-bitrate only. Its sample rates are the MPEG ones (8 to 48 kHz); anything
// else is refused at open() with the reason, and the export job resamples first.
#pragma once

#include <cstdio>
#include <vector>

#include "engine/render/Encoders.h"

struct shine_global_flags;

namespace adx::render {

class Mp3Encoder final : public Encoder {
public:
    Mp3Encoder() = default;
    ~Mp3Encoder() override;
    Mp3Encoder(const Mp3Encoder&) = delete;
    Mp3Encoder& operator=(const Mp3Encoder&) = delete;
    Mp3Encoder(Mp3Encoder&&) = delete;
    Mp3Encoder& operator=(Mp3Encoder&&) = delete;

    bool open(const std::filesystem::path& path, const EncodeSettings& settings,
              std::string& error) override;
    bool write(std::span<const float> interleaved, std::string& error) override;
    bool finish(std::string& error) override;

    /// True when shine accepts `rate` at `kbps`.
    [[nodiscard]] static bool supports(std::uint32_t rate, std::uint32_t kbps) noexcept;

private:
    bool encodePass(std::string& error);

    std::FILE* m_file{nullptr};
    shine_global_flags* m_shine{nullptr};
    std::uint32_t m_channels{2};
    std::size_t m_framesPerPass{0};
    /// Interleaved int16 waiting for a full pass.
    std::vector<std::int16_t> m_pending;
};

} // namespace adx::render
