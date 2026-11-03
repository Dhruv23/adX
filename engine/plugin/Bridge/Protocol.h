// adx-thread: main
//
// The control half of the sandbox's wire protocol (phase_9.md §4.3): everything that is
// not a block of audio. Requests go down the child's stdin, replies come back up its
// stdout, one at a time and in order: the host never has two requests in flight to one
// child.
//
//   frame   = u32 length (of what follows) , u32 op , u32 request id , payload
//   reply   = u32 length , u32 Op::Reply , u32 request id , u32 status , payload
//
// Integers are little-endian, strings and byte blobs are a u32 length and the bytes.
// Audio never crosses here; it is in the shared region (SharedAudio.h).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/plugin/PluginDescriptor.h"

namespace adx::plugin::bridge {

inline constexpr std::uint32_t kProtocolVersion = 1;
/// A larger frame is corruption, not a state blob.
inline constexpr std::uint32_t kMaxFrameBytes = 256U * 1024U * 1024U;

enum class Op : std::uint32_t {
    Reply = 0,
    /// -> protocol version. The first request on every child.
    Hello,
    /// instance, format, path, uid, sample rate, max frames, region name, go event,
    /// done event -> LoadReply.
    Load,
    Unload,
    /// instance, bytes -> nothing. Status 1 when the plugin rejected the bytes.
    SetState,
    /// instance -> bytes.
    GetState,
    /// instance -> u32 count, (u32 id, f64 value)...
    GetParamValues,
    /// instance, u32 id, f64 value -> string.
    ParamText,
    /// instance -> u64 native window, u32 width, u32 height. Status 1: no editor.
    OpenEditor,
    CloseEditor,
    /// instance -> u32 latency. Re-activates a plugin that asked to restart.
    Restart,
    /// -> nothing, then the child exits.
    Quit,
};

enum class Status : std::uint32_t { Ok = 0, Failed = 1 };

class ByteWriter {
public:
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void f64(double value);
    void str(std::string_view text);
    void bytes(std::span<const std::uint8_t> data);
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept {
        return m_data;
    }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept {
        return std::move(m_data);
    }

private:
    std::vector<std::uint8_t> m_data;
};

/// Reads what a ByteWriter wrote. Every read past the end yields zero and sets ok()
/// false, so a short payload is one check at the end, not one per field.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) noexcept : m_data(data) {}
    std::uint32_t u32() noexcept;
    std::uint64_t u64() noexcept;
    double f64() noexcept;
    std::string str();
    std::vector<std::uint8_t> bytes();
    [[nodiscard]] bool ok() const noexcept {
        return m_ok;
    }

private:
    bool take(void* out, std::size_t size) noexcept;
    std::span<const std::uint8_t> m_data;
    std::size_t m_at{0};
    bool m_ok{true};
};

struct LoadRequest {
    std::uint32_t instance{0};
    PluginFormat format{PluginFormat::Clap};
    std::string path;
    std::string uid;
    std::uint32_t sampleRate{48000};
    std::uint32_t maxFrames{kMaxBlockFramesDefault};
    std::string region;
    std::string goEvent;
    std::string doneEvent;

    static constexpr std::uint32_t kMaxBlockFramesDefault = 4096;

    void write(ByteWriter& out) const;
    [[nodiscard]] bool read(ByteReader& in);
};

/// What a loaded instance is: the descriptor, as the live plugin reports it.
struct LoadReply {
    std::string error;
    PluginDescriptor descriptor;

    void write(ByteWriter& out) const;
    [[nodiscard]] bool read(ByteReader& in);
};

/// The descriptor's wire form, shared by LoadReply and nothing else - scans use JSON,
/// which a person can read in the cache file.
void writeDescriptor(ByteWriter& out, const PluginDescriptor& descriptor);
[[nodiscard]] bool readDescriptor(ByteReader& in, PluginDescriptor& out);

/// One frame: the header and the payload.
[[nodiscard]] std::vector<std::uint8_t> frame(Op op, std::uint32_t requestId,
                                              std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> replyFrame(std::uint32_t requestId, Status status,
                                                   std::span<const std::uint8_t> payload);

} // namespace adx::plugin::bridge
