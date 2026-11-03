// adx-thread: main
//
// The encoder interface: one per export format (phase_8.md §3.5, FINAL_PLAN §5.7).
//
// Streaming - open, write any number of interleaved blocks, finish - so an export
// does not have to hold a whole encoded file, and so the WAV writer can patch its
// header at the end the way a recorder does. Input is always float; an integer format
// rounds, and dither (Dither.h) is the caller's, applied before the samples arrive, so
// what reaches an encoder is already exactly representable at its depth.
//
// Main thread or an export worker; never the audio thread.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace adx::render {

enum class FileFormat : std::uint8_t {
    Wav16,
    Wav24,
    Wav32f,
    Mp3,
    Flac,
    Ogg,
};

[[nodiscard]] const char* toString(FileFormat format) noexcept;
/// `wav16`, `wav24`, `wav32f` (and `wav` for 24), `mp3`, `flac`, `ogg`.
[[nodiscard]] bool fileFormatFromString(std::string_view name, FileFormat& out) noexcept;
[[nodiscard]] const char* extensionOf(FileFormat format) noexcept;

/// Integer bits a format stores, or 0 for float and the lossy formats - which is what
/// decides whether dither applies (phase_8.md §3.5: "only when reducing bit depth").
[[nodiscard]] std::uint32_t integerBits(FileFormat format, std::uint32_t flacBits) noexcept;

struct EncodeSettings {
    FileFormat format{FileFormat::Wav24};
    std::uint32_t sampleRate{48000};
    std::uint32_t channels{2};
    /// MP3, kbps. shine is constant-bitrate only (phase_8.md §11).
    std::uint32_t mp3Bitrate{320};
    /// FLAC: compression level 0..8, and 16 or 24 bits.
    std::uint32_t flacLevel{5};
    std::uint32_t flacBits{24};
    /// OGG: libvorbis quality, -0.1 .. 1.0 (about 45 .. 500 kbps for stereo at 44.1).
    float oggQuality{0.6F};
    /// WAV: write RF64 past this many data bytes. The real limit is 4 GiB less the
    /// header; a test lowers it to exercise the switch without writing 4 GiB.
    std::uint64_t rf64Threshold{0xFFFFFFFFULL - 1024};
};

class Encoder {
public:
    Encoder() = default;
    virtual ~Encoder() = default;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    Encoder(Encoder&&) = delete;
    Encoder& operator=(Encoder&&) = delete;

    /// Creates the file. False with `error` set when it cannot be created or the
    /// settings are not ones the format supports.
    virtual bool open(const std::filesystem::path& path, const EncodeSettings& settings,
                      std::string& error) = 0;
    /// `interleaved.size()` must be a multiple of the channel count.
    virtual bool write(std::span<const float> interleaved, std::string& error) = 0;
    /// Flushes and closes. The file is complete only after this returns true.
    virtual bool finish(std::string& error) = 0;
};

[[nodiscard]] std::unique_ptr<Encoder> makeEncoder(FileFormat format);

/// A whole buffer to a file, in one call.
[[nodiscard]] bool encodeFile(const std::filesystem::path& path, std::span<const float> interleaved,
                              const EncodeSettings& settings, std::string& error);

/// Float to a signed integer of `bits`: round to nearest, saturate at the ends. The
/// scale is 2^(bits-1), so a value that came from an integer decode round-trips.
[[nodiscard]] std::int32_t toInteger(float sample, std::uint32_t bits) noexcept;

} // namespace adx::render
