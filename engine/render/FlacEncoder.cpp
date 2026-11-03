// adx-thread: main
#include "engine/render/FlacEncoder.h"

#include <algorithm>

#include <FLAC/stream_encoder.h>

#include "engine/render/FileIo.h"

namespace adx::render {

namespace {

FLAC__StreamEncoder* flac(void* encoder) noexcept {
    return static_cast<FLAC__StreamEncoder*>(encoder);
}

} // namespace

FlacEncoder::~FlacEncoder() {
    if (m_encoder != nullptr) {
        FLAC__stream_encoder_delete(flac(m_encoder));
    }
}

bool FlacEncoder::open(const std::filesystem::path& path, const EncodeSettings& settings,
                       std::string& error) {
    if (settings.flacBits != 16 && settings.flacBits != 24) {
        error = "FLAC export is 16 or 24 bits";
        return false;
    }
    m_encoder = FLAC__stream_encoder_new();
    if (m_encoder == nullptr) {
        error = "libFLAC could not allocate an encoder";
        return false;
    }
    m_channels = settings.channels;
    m_bits = settings.flacBits;
    FLAC__stream_encoder_set_channels(flac(m_encoder), settings.channels);
    FLAC__stream_encoder_set_bits_per_sample(flac(m_encoder), settings.flacBits);
    FLAC__stream_encoder_set_sample_rate(flac(m_encoder), settings.sampleRate);
    FLAC__stream_encoder_set_compression_level(flac(m_encoder), std::min(settings.flacLevel, 8U));
    // libFLAC opens the file itself: through a FILE* so a path the ANSI code page
    // cannot spell still works on Windows.
    std::FILE* file = openForWriting(path);
    if (file == nullptr) {
        error = "could not create " + path.string();
        return false;
    }
    const FLAC__StreamEncoderInitStatus status =
        FLAC__stream_encoder_init_FILE(flac(m_encoder), file, nullptr, nullptr);
    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        static_cast<void>(std::fclose(file));
        error =
            std::string("libFLAC could not start: ") + FLAC__StreamEncoderInitStatusString[status];
        return false;
    }
    return true;
}

bool FlacEncoder::write(std::span<const float> interleaved, std::string& error) {
    m_chunk.resize(interleaved.size());
    for (std::size_t i = 0; i < interleaved.size(); ++i) {
        m_chunk[i] = toInteger(interleaved[i], m_bits);
    }
    const auto frames = static_cast<std::uint32_t>(interleaved.size() / m_channels);
    if (FLAC__stream_encoder_process_interleaved(flac(m_encoder), m_chunk.data(), frames) == 0) {
        error = std::string("libFLAC: ") +
                FLAC__stream_encoder_get_resolved_state_string(flac(m_encoder));
        return false;
    }
    return true;
}

bool FlacEncoder::finish(std::string& error) {
    // finish() closes the FILE* it was given.
    const bool ok = FLAC__stream_encoder_finish(flac(m_encoder)) != 0;
    if (!ok) {
        error = std::string("libFLAC: ") +
                FLAC__stream_encoder_get_resolved_state_string(flac(m_encoder));
    }
    FLAC__stream_encoder_delete(flac(m_encoder));
    m_encoder = nullptr;
    return ok;
}

} // namespace adx::render
