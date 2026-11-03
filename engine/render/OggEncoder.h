// adx-thread: main
//
// Ogg Vorbis through libvorbis and libogg - the one new dependency of Phase 8,
// justified in FINAL_PLAN §8 (phase_8.md §4.6). Quality-based VBR, which is how
// Vorbis is meant to be used.
//
// The stream serial number is fixed rather than random. Nothing requires an Ogg stream
// to have a random serial when it is the only stream in the file, and a fixed one means
// the same render encodes to the same bytes - one less thing that differs between two
// exports of an unchanged project.
#pragma once

#include <cstdio>
#include <memory>

#include "engine/render/Encoders.h"

namespace adx::render {

class OggEncoder final : public Encoder {
public:
    OggEncoder();
    ~OggEncoder() override;
    OggEncoder(const OggEncoder&) = delete;
    OggEncoder& operator=(const OggEncoder&) = delete;
    OggEncoder(OggEncoder&&) = delete;
    OggEncoder& operator=(OggEncoder&&) = delete;

    bool open(const std::filesystem::path& path, const EncodeSettings& settings,
              std::string& error) override;
    bool write(std::span<const float> interleaved, std::string& error) override;
    bool finish(std::string& error) override;

private:
    struct State;
    bool drain(bool flush, std::string& error);

    std::unique_ptr<State> m_state;
    std::FILE* m_file{nullptr};
    std::uint32_t m_channels{2};
    bool m_started{false};
};

} // namespace adx::render
