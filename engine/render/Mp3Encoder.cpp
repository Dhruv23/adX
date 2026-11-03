// adx-thread: main
#include "engine/render/Mp3Encoder.h"

#include <algorithm>

#include "engine/render/FileIo.h"

extern "C" {
#include <layer3.h>
}

namespace adx::render {

Mp3Encoder::~Mp3Encoder() {
    if (m_shine != nullptr) {
        shine_close(m_shine);
    }
    if (m_file != nullptr) {
        static_cast<void>(std::fclose(m_file));
    }
}

bool Mp3Encoder::supports(std::uint32_t rate, std::uint32_t kbps) noexcept {
    return shine_check_config(static_cast<int>(rate), static_cast<int>(kbps)) >= 0;
}

bool Mp3Encoder::open(const std::filesystem::path& path, const EncodeSettings& settings,
                      std::string& error) {
    if (settings.channels < 1 || settings.channels > 2) {
        error = "MP3 is mono or stereo";
        return false;
    }
    if (!supports(settings.sampleRate, settings.mp3Bitrate)) {
        error = "MP3 cannot encode " + std::to_string(settings.sampleRate) + " Hz at " +
                std::to_string(settings.mp3Bitrate) +
                " kbps; MPEG rates are 8-48 kHz, and the bitrate must be one the rate allows";
        return false;
    }
    shine_config_t config{};
    shine_set_config_mpeg_defaults(&config.mpeg);
    config.wave.channels = settings.channels == 1 ? PCM_MONO : PCM_STEREO;
    config.wave.samplerate = static_cast<int>(settings.sampleRate);
    // Plain stereo, the archive's choice: shine has no joint-stereo decision to make.
    config.mpeg.mode = settings.channels == 1 ? MONO : STEREO;
    config.mpeg.bitr = static_cast<int>(settings.mp3Bitrate);
    m_shine = shine_initialise(&config);
    if (m_shine == nullptr) {
        error = "shine could not initialise";
        return false;
    }
    m_file = openForWriting(path);
    if (m_file == nullptr) {
        error = "could not create " + path.string();
        return false;
    }
    m_channels = settings.channels;
    m_framesPerPass = static_cast<std::size_t>(shine_samples_per_pass(m_shine));
    m_pending.reserve(m_framesPerPass * m_channels);
    return true;
}

bool Mp3Encoder::encodePass(std::string& error) {
    int written = 0;
    unsigned char* data = shine_encode_buffer_interleaved(m_shine, m_pending.data(), &written);
    m_pending.clear();
    if (written > 0 && std::fwrite(data, 1, static_cast<std::size_t>(written), m_file) !=
                           static_cast<std::size_t>(written)) {
        error = "write failed (disk full?)";
        return false;
    }
    return true;
}

bool Mp3Encoder::write(std::span<const float> interleaved, std::string& error) {
    // shine comes out 9/8 hot - +1.022 dB, the same at every frequency from 25 Hz to
    // 1 kHz, at 32, 44.1 and 48 kHz and 128 to 320 kbps, measured against the decoder a
    // LAME file decodes within 0.5 dB through - so it is fed 8/9 of the signal.
    constexpr float kShineGain = 8.0F / 9.0F;
    const std::size_t pass = m_framesPerPass * m_channels;
    for (const float sample : interleaved) {
        m_pending.push_back(static_cast<std::int16_t>(toInteger(sample * kShineGain, 16)));
        if (m_pending.size() == pass && !encodePass(error)) {
            return false;
        }
    }
    return true;
}

bool Mp3Encoder::finish(std::string& error) {
    bool ok = true;
    if (!m_pending.empty()) {
        // The last pass is padded with silence to a whole granule.
        m_pending.resize(m_framesPerPass * m_channels, 0);
        ok = encodePass(error);
    }
    int written = 0;
    unsigned char* data = shine_flush(m_shine, &written);
    if (ok && written > 0) {
        ok = std::fwrite(data, 1, static_cast<std::size_t>(written), m_file) ==
             static_cast<std::size_t>(written);
    }
    shine_close(m_shine);
    m_shine = nullptr;
    ok = (std::fclose(m_file) == 0) && ok;
    m_file = nullptr;
    if (!ok && error.empty()) {
        error = "could not finish the MP3";
    }
    return ok;
}

} // namespace adx::render
