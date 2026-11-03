// adx-thread: main
#include "engine/render/OggEncoder.h"

#include <algorithm>

#include <vorbis/vorbisenc.h>

#include "engine/render/FileIo.h"

namespace adx::render {

namespace {

/// "ADX!" - see the header for why it is fixed.
constexpr int kSerial = 0x41445821;
/// Frames handed to the analysis per call.
constexpr std::size_t kChunkFrames = 1024;

} // namespace

struct OggEncoder::State {
    vorbis_info info{};
    vorbis_comment comment{};
    vorbis_dsp_state dsp{};
    vorbis_block block{};
    ogg_stream_state stream{};
    bool infoInit{false};
    bool dspInit{false};
};

OggEncoder::OggEncoder() : m_state(std::make_unique<State>()) {}

OggEncoder::~OggEncoder() {
    State& s = *m_state;
    if (s.dspInit) {
        ogg_stream_clear(&s.stream);
        vorbis_block_clear(&s.block);
        vorbis_dsp_clear(&s.dsp);
    }
    if (s.infoInit) {
        vorbis_comment_clear(&s.comment);
        vorbis_info_clear(&s.info);
    }
    if (m_file != nullptr) {
        static_cast<void>(std::fclose(m_file));
    }
}

bool OggEncoder::open(const std::filesystem::path& path, const EncodeSettings& settings,
                      std::string& error) {
    State& s = *m_state;
    vorbis_info_init(&s.info);
    vorbis_comment_init(&s.comment);
    s.infoInit = true;
    const float quality = std::clamp(settings.oggQuality, -0.1F, 1.0F);
    if (vorbis_encode_init_vbr(&s.info, static_cast<long>(settings.channels),
                               static_cast<long>(settings.sampleRate), quality) != 0) {
        error = "libvorbis cannot encode " + std::to_string(settings.channels) + " channels at " +
                std::to_string(settings.sampleRate) + " Hz";
        return false;
    }
    vorbis_comment_add_tag(&s.comment, "ENCODER", "adX");
    vorbis_analysis_init(&s.dsp, &s.info);
    vorbis_block_init(&s.dsp, &s.block);
    ogg_stream_init(&s.stream, kSerial);
    s.dspInit = true;

    m_file = openForWriting(path);
    if (m_file == nullptr) {
        error = "could not create " + path.string();
        return false;
    }
    m_channels = settings.channels;

    // The three header packets, flushed so audio starts on a fresh page.
    ogg_packet header{};
    ogg_packet comments{};
    ogg_packet codebooks{};
    vorbis_analysis_headerout(&s.dsp, &s.comment, &header, &comments, &codebooks);
    ogg_stream_packetin(&s.stream, &header);
    ogg_stream_packetin(&s.stream, &comments);
    ogg_stream_packetin(&s.stream, &codebooks);
    ogg_page page{};
    while (ogg_stream_flush(&s.stream, &page) != 0) {
        if (std::fwrite(page.header, 1, static_cast<std::size_t>(page.header_len), m_file) !=
                static_cast<std::size_t>(page.header_len) ||
            std::fwrite(page.body, 1, static_cast<std::size_t>(page.body_len), m_file) !=
                static_cast<std::size_t>(page.body_len)) {
            error = "write failed (disk full?)";
            return false;
        }
    }
    m_started = true;
    return true;
}

bool OggEncoder::drain(bool flush, std::string& error) {
    State& s = *m_state;
    ogg_packet packet{};
    ogg_page page{};
    while (vorbis_analysis_blockout(&s.dsp, &s.block) == 1) {
        vorbis_analysis(&s.block, nullptr);
        vorbis_bitrate_addblock(&s.block);
        while (vorbis_bitrate_flushpacket(&s.dsp, &packet) != 0) {
            ogg_stream_packetin(&s.stream, &packet);
            while ((flush ? ogg_stream_flush(&s.stream, &page)
                          : ogg_stream_pageout(&s.stream, &page)) != 0) {
                if (std::fwrite(page.header, 1, static_cast<std::size_t>(page.header_len),
                                m_file) != static_cast<std::size_t>(page.header_len) ||
                    std::fwrite(page.body, 1, static_cast<std::size_t>(page.body_len), m_file) !=
                        static_cast<std::size_t>(page.body_len)) {
                    error = "write failed (disk full?)";
                    return false;
                }
                if (ogg_page_eos(&page) != 0) {
                    break;
                }
            }
        }
    }
    return true;
}

bool OggEncoder::write(std::span<const float> interleaved, std::string& error) {
    State& s = *m_state;
    const std::size_t frames = interleaved.size() / m_channels;
    for (std::size_t done = 0; done < frames; done += kChunkFrames) {
        const std::size_t count = std::min(kChunkFrames, frames - done);
        float** buffer = vorbis_analysis_buffer(&s.dsp, static_cast<int>(count));
        for (std::size_t i = 0; i < count; ++i) {
            for (std::uint32_t c = 0; c < m_channels; ++c) {
                buffer[c][i] = interleaved[((done + i) * m_channels) + c];
            }
        }
        vorbis_analysis_wrote(&s.dsp, static_cast<int>(count));
        if (!drain(false, error)) {
            return false;
        }
    }
    return true;
}

bool OggEncoder::finish(std::string& error) {
    bool ok = m_started;
    if (ok) {
        // Zero frames written is end of stream.
        vorbis_analysis_wrote(&m_state->dsp, 0);
        ok = drain(true, error);
    }
    ok = (std::fclose(m_file) == 0) && ok;
    m_file = nullptr;
    if (!ok && error.empty()) {
        error = "could not finish the Ogg stream";
    }
    return ok;
}

} // namespace adx::render
