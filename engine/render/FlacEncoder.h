// adx-thread: main
//
// FLAC through libFLAC, ported from the archive's ExportRenderer (phase_8.md §4): 16 or
// 24 bits, compression level 0-8. Lossless, so the golden corpus hashes what it
// decodes to (phase_8.md §3.9).
#pragma once

#include <vector>

#include "engine/render/Encoders.h"

namespace adx::render {

class FlacEncoder final : public Encoder {
public:
    FlacEncoder() = default;
    ~FlacEncoder() override;
    FlacEncoder(const FlacEncoder&) = delete;
    FlacEncoder& operator=(const FlacEncoder&) = delete;
    FlacEncoder(FlacEncoder&&) = delete;
    FlacEncoder& operator=(FlacEncoder&&) = delete;

    bool open(const std::filesystem::path& path, const EncodeSettings& settings,
              std::string& error) override;
    bool write(std::span<const float> interleaved, std::string& error) override;
    bool finish(std::string& error) override;

private:
    /// A FLAC__StreamEncoder*, which is a typedef of an anonymous struct and so cannot be
    /// forward-declared; FlacEncoder.cpp casts it back.
    void* m_encoder{nullptr};
    std::uint32_t m_channels{2};
    std::uint32_t m_bits{24};
    std::vector<std::int32_t> m_chunk;
};

} // namespace adx::render
