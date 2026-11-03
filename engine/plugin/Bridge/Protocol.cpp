// adx-thread: main
#include "engine/plugin/Bridge/Protocol.h"

#include <array>
#include <cstring>

namespace adx::plugin::bridge {

void ByteWriter::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        m_data.push_back(
            static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

void ByteWriter::u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        m_data.push_back(
            static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

void ByteWriter::f64(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    u64(bits);
}

void ByteWriter::str(std::string_view text) {
    u32(static_cast<std::uint32_t>(text.size()));
    m_data.insert(m_data.end(), text.begin(), text.end());
}

void ByteWriter::bytes(std::span<const std::uint8_t> data) {
    u32(static_cast<std::uint32_t>(data.size()));
    m_data.insert(m_data.end(), data.begin(), data.end());
}

bool ByteReader::take(void* out, std::size_t size) noexcept {
    if (!m_ok || m_data.size() - m_at < size) {
        m_ok = false;
        std::memset(out, 0, size);
        return false;
    }
    std::memcpy(out, m_data.data() + m_at, size);
    m_at += size;
    return true;
}

std::uint32_t ByteReader::u32() noexcept {
    std::array<std::uint8_t, 4> raw{};
    take(raw.data(), raw.size());
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        value |= static_cast<std::uint32_t>(raw.at(i)) << (8U * i);
    }
    return value;
}

std::uint64_t ByteReader::u64() noexcept {
    std::array<std::uint8_t, 8> raw{};
    take(raw.data(), raw.size());
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        value |= static_cast<std::uint64_t>(raw.at(i)) << (8U * i);
    }
    return value;
}

double ByteReader::f64() noexcept {
    const std::uint64_t bits = u64();
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

std::string ByteReader::str() {
    const std::uint32_t size = u32();
    if (!m_ok || m_data.size() - m_at < size) {
        m_ok = false;
        return {};
    }
    std::string out(reinterpret_cast<const char*>(m_data.data() + m_at), size);
    m_at += size;
    return out;
}

std::vector<std::uint8_t> ByteReader::bytes() {
    const std::uint32_t size = u32();
    if (!m_ok || m_data.size() - m_at < size) {
        m_ok = false;
        return {};
    }
    std::vector<std::uint8_t> out(m_data.begin() + static_cast<std::ptrdiff_t>(m_at),
                                  m_data.begin() + static_cast<std::ptrdiff_t>(m_at + size));
    m_at += size;
    return out;
}

void LoadRequest::write(ByteWriter& out) const {
    out.u32(instance);
    out.u32(static_cast<std::uint32_t>(format));
    out.str(path);
    out.str(uid);
    out.u32(sampleRate);
    out.u32(maxFrames);
    out.str(region);
    out.str(goEvent);
    out.str(doneEvent);
}

bool LoadRequest::read(ByteReader& in) {
    instance = in.u32();
    format = in.u32() == static_cast<std::uint32_t>(PluginFormat::Vst3) ? PluginFormat::Vst3
                                                                        : PluginFormat::Clap;
    path = in.str();
    uid = in.str();
    sampleRate = in.u32();
    maxFrames = in.u32();
    region = in.str();
    goEvent = in.str();
    doneEvent = in.str();
    return in.ok();
}

void writeDescriptor(ByteWriter& out, const PluginDescriptor& d) {
    out.u32(static_cast<std::uint32_t>(d.id.format));
    out.str(d.id.uid);
    out.str(d.id.path);
    out.str(d.id.name);
    out.str(d.id.version);
    out.str(d.vendor);
    out.u32(d.instrument ? 1 : 0);
    out.u32(d.audioInputs);
    out.u32(d.audioOutputs);
    out.u32(d.hasEditor ? 1 : 0);
    out.u32(d.latency);
    out.u32(static_cast<std::uint32_t>(d.params.size()));
    for (const ParamInfo& p : d.params) {
        out.u32(p.id);
        out.str(p.key);
        out.str(p.name);
        out.f64(p.minimum);
        out.f64(p.maximum);
        out.f64(p.defaultValue);
        out.u32(p.steps);
        out.u32((p.automatable ? 1U : 0U) | (p.hidden ? 2U : 0U));
    }
}

bool readDescriptor(ByteReader& in, PluginDescriptor& d) {
    d = PluginDescriptor{};
    d.id.format = in.u32() == static_cast<std::uint32_t>(PluginFormat::Vst3) ? PluginFormat::Vst3
                                                                             : PluginFormat::Clap;
    d.id.uid = in.str();
    d.id.path = in.str();
    d.id.name = in.str();
    d.id.version = in.str();
    d.vendor = in.str();
    d.instrument = in.u32() != 0;
    d.audioInputs = in.u32();
    d.audioOutputs = in.u32();
    d.hasEditor = in.u32() != 0;
    d.latency = in.u32();
    const std::uint32_t count = in.u32();
    // A count larger than the bytes left is corruption; do not reserve for it.
    for (std::uint32_t i = 0; i < count && in.ok(); ++i) {
        ParamInfo p;
        p.id = in.u32();
        p.key = in.str();
        p.name = in.str();
        p.minimum = in.f64();
        p.maximum = in.f64();
        p.defaultValue = in.f64();
        p.steps = in.u32();
        const std::uint32_t flags = in.u32();
        p.automatable = (flags & 1U) != 0;
        p.hidden = (flags & 2U) != 0;
        d.params.push_back(std::move(p));
    }
    return in.ok();
}

void LoadReply::write(ByteWriter& out) const {
    out.str(error);
    writeDescriptor(out, descriptor);
}

bool LoadReply::read(ByteReader& in) {
    error = in.str();
    return readDescriptor(in, descriptor) && in.ok();
}

std::vector<std::uint8_t> frame(Op op, std::uint32_t requestId,
                                std::span<const std::uint8_t> payload) {
    ByteWriter out;
    out.u32(static_cast<std::uint32_t>(8 + payload.size()));
    out.u32(static_cast<std::uint32_t>(op));
    out.u32(requestId);
    std::vector<std::uint8_t> bytes = out.take();
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> replyFrame(std::uint32_t requestId, Status status,
                                     std::span<const std::uint8_t> payload) {
    ByteWriter out;
    out.u32(static_cast<std::uint32_t>(12 + payload.size()));
    out.u32(static_cast<std::uint32_t>(Op::Reply));
    out.u32(requestId);
    out.u32(static_cast<std::uint32_t>(status));
    std::vector<std::uint8_t> bytes = out.take();
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

} // namespace adx::plugin::bridge
