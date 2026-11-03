// adx-thread: main
#include "engine/render/WavEncoder.h"

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#include "engine/render/FileIo.h"

namespace adx::render {

namespace {

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
/// ds64's body: RIFF size, data size, sample count (8 bytes each), table length (4).
constexpr std::uint32_t kDs64Bytes = 28;
/// Where each patched field sits, given the layout open() writes.
constexpr long kRiffSizeAt = 4;
constexpr long kJunkAt = 12;
constexpr long kFmtAt = kJunkAt + 8 + kDs64Bytes;

void put(std::vector<std::uint8_t>& out, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(
            static_cast<std::uint8_t>((value >> (8U * static_cast<unsigned>(i))) & 0xFFU));
    }
}

void tag(std::vector<std::uint8_t>& out, std::string_view name) {
    out.insert(out.end(), name.begin(), name.begin() + 4);
}

} // namespace

WavEncoder::~WavEncoder() {
    if (m_file != nullptr) {
        static_cast<void>(std::fclose(m_file));
    }
}

bool WavEncoder::open(const std::filesystem::path& path, const EncodeSettings& settings,
                      std::string& error) {
    m_settings = settings;
    m_bytesPerSample = 4;
    if (settings.format == FileFormat::Wav16) {
        m_bytesPerSample = 2;
    } else if (settings.format == FileFormat::Wav24) {
        m_bytesPerSample = 3;
    }
    m_file = openForWriting(path);
    if (m_file == nullptr) {
        error = "could not create " + path.string();
        return false;
    }
    const bool isFloat = settings.format == FileFormat::Wav32f;
    std::vector<std::uint8_t> header;
    tag(header, "RIFF");
    put(header, 0, 4);
    tag(header, "WAVE");
    // Space for ds64, should the file outgrow RIFF (EBU TECH 3306 §3).
    tag(header, "JUNK");
    put(header, kDs64Bytes, 4);
    header.insert(header.end(), kDs64Bytes, 0);
    tag(header, "fmt ");
    put(header, 16, 4);
    put(header, isFloat ? kFormatFloat : kFormatPcm, 2);
    put(header, settings.channels, 2);
    put(header, settings.sampleRate, 4);
    put(header,
        static_cast<std::uint64_t>(settings.sampleRate) * settings.channels * m_bytesPerSample, 4);
    put(header, static_cast<std::uint64_t>(settings.channels) * m_bytesPerSample, 2);
    put(header, static_cast<std::uint64_t>(m_bytesPerSample) * 8, 2);
    if (isFloat) {
        tag(header, "fact");
        put(header, 4, 4);
        put(header, 0, 4);
    }
    tag(header, "data");
    put(header, 0, 4);
    if (std::fwrite(header.data(), 1, header.size(), m_file) != header.size()) {
        error = "could not write " + path.string();
        return false;
    }
    return true;
}

bool WavEncoder::write(std::span<const float> interleaved, std::string& error) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(interleaved.size() * m_bytesPerSample);
    for (const float sample : interleaved) {
        if (m_settings.format == FileFormat::Wav32f) {
            std::array<std::uint8_t, 4> raw{};
            std::memcpy(raw.data(), &sample, 4);
            bytes.insert(bytes.end(), raw.begin(), raw.end());
        } else {
            const std::int32_t value = toInteger(sample, m_bytesPerSample * 8);
            put(bytes, static_cast<std::uint32_t>(value), static_cast<int>(m_bytesPerSample));
        }
    }
    if (std::fwrite(bytes.data(), 1, bytes.size(), m_file) != bytes.size()) {
        error = "write failed (disk full?)";
        return false;
    }
    m_dataBytes += bytes.size();
    m_frames += interleaved.size() / std::max<std::uint32_t>(m_settings.channels, 1);
    return true;
}

bool WavEncoder::checkpoint(std::string& error) {
    if (m_file == nullptr) {
        error = "not open";
        return false;
    }
    const bool ok =
        patchHeader(false) && std::fseek(m_file, 0, SEEK_END) == 0 && std::fflush(m_file) == 0;
    if (!ok) {
        error = "could not update the WAV header";
    }
    return ok;
}

bool WavEncoder::finish(std::string& error) {
    if (m_file == nullptr) {
        error = "not open";
        return false;
    }
    // An odd-sized chunk is padded to an even boundary; the size field does not count
    // the pad.
    if ((m_dataBytes % 2) != 0) {
        static_cast<void>(std::fputc(0, m_file));
    }
    bool ok = patchHeader(true);
    ok = (std::fclose(m_file) == 0) && ok;
    m_file = nullptr;
    if (!ok) {
        error = "could not finish the WAV header";
    }
    return ok;
}

bool WavEncoder::patchHeader(bool padded) {
    const bool isFloat = m_settings.format == FileFormat::Wav32f;
    const long dataSizeAt = kFmtAt + 8 + 16 + (isFloat ? 12 : 0) + 4;
    const std::uint64_t headerBytes = static_cast<std::uint64_t>(dataSizeAt) + 4;
    const std::uint64_t riffSize = headerBytes - 8 + m_dataBytes + (padded ? (m_dataBytes % 2) : 0);
    const bool rf64 = m_dataBytes > m_settings.rf64Threshold;

    bool ok = true;
    const auto patch = [&](long at, std::uint64_t value, int bytes) {
        std::vector<std::uint8_t> field;
        put(field, value, bytes);
        ok = ok && seekTo(m_file, at) &&
             std::fwrite(field.data(), 1, field.size(), m_file) == field.size();
    };
    if (rf64) {
        ok = seekTo(m_file, 0) && std::fwrite("RF64", 1, 4, m_file) == 4;
        patch(kRiffSizeAt, 0xFFFFFFFFULL, 4);
        ok = ok && seekTo(m_file, kJunkAt) && std::fwrite("ds64", 1, 4, m_file) == 4;
        patch(kJunkAt + 8, riffSize, 8);
        patch(kJunkAt + 16, m_dataBytes, 8);
        patch(kJunkAt + 24, m_frames, 8);
        patch(kJunkAt + 32, 0, 4);
        if (isFloat) {
            patch(kFmtAt + 8 + 16 + 8, 0xFFFFFFFFULL, 4);
        }
        patch(dataSizeAt, 0xFFFFFFFFULL, 4);
    } else {
        patch(kRiffSizeAt, riffSize, 4);
        if (isFloat) {
            patch(kFmtAt + 8 + 16 + 8, m_frames, 4);
        }
        patch(dataSizeAt, m_dataBytes, 4);
    }
    return ok;
}

} // namespace adx::render
