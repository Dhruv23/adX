// adx-thread: main
//
// The operating-system pieces the sandbox is built from: a named shared region, a
// named auto-reset event, a child process with its stdin and stdout piped, and a
// loaded library. Win32 behind the interface; nothing outside Platform.cpp includes
// <windows.h>.
//
// Event::set and Event::wait are the two calls the host's audio thread makes (§4.3):
// one SetEvent and one bounded WaitForSingleObject. Neither allocates or locks.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace adx::plugin::bridge {

/// A unique-enough name for this process's next kernel object: prefix, pid, counter.
[[nodiscard]] std::string uniqueName(const char* prefix);

class SharedRegion {
public:
    SharedRegion() = default;
    ~SharedRegion();
    SharedRegion(const SharedRegion&) = delete;
    SharedRegion& operator=(const SharedRegion&) = delete;
    SharedRegion(SharedRegion&& other) noexcept;
    SharedRegion& operator=(SharedRegion&& other) noexcept;

    [[nodiscard]] bool create(const std::string& name, std::size_t size);
    [[nodiscard]] bool open(const std::string& name, std::size_t size);
    void close() noexcept;
    [[nodiscard]] void* data() const noexcept {
        return m_view;
    }

private:
    void* m_mapping{nullptr};
    void* m_view{nullptr};
};

class Event {
public:
    Event() = default;
    ~Event();
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&& other) noexcept;
    Event& operator=(Event&& other) noexcept;

    [[nodiscard]] bool create(const std::string& name);
    [[nodiscard]] bool open(const std::string& name);
    void close() noexcept;
    void set() const noexcept;
    /// True when signalled within `timeoutMicros`; 0 only polls. Microsecond-accurate on
    /// an event this process create()d (a high-resolution timer goes with it), rounded
    /// up to milliseconds on one it open()ed.
    [[nodiscard]] bool wait(std::uint32_t timeoutMicros) const noexcept;
    [[nodiscard]] void* handle() const noexcept {
        return m_handle;
    }
    [[nodiscard]] void* timer() const noexcept {
        return m_timer;
    }

private:
    void* m_handle{nullptr};
    void* m_timer{nullptr};
};

/// Waits on several events at once: the index of a signalled one, or -1 on timeout.
[[nodiscard]] int waitAny(std::span<void* const> handles, std::uint32_t timeoutMillis) noexcept;

class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    /// Starts `exe` with `args` (each quoted as needed), stdin and stdout piped to this
    /// process and stderr inherited.
    [[nodiscard]] bool spawn(const std::string& exe, const std::vector<std::string>& args,
                             std::string& error);
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint32_t exitCode() const noexcept;
    void kill() noexcept;
    /// True when the child exited within `timeoutMillis`.
    [[nodiscard]] bool waitExit(std::uint32_t timeoutMillis) const noexcept;

    [[nodiscard]] bool write(std::span<const std::uint8_t> bytes) noexcept;
    /// Reads exactly `out.size()` bytes, or fails at `deadlineMillis` from now, or when
    /// the child closes its end (it exited). Polls the pipe, so a hung child times out
    /// rather than blocking the caller.
    [[nodiscard]] bool read(std::span<std::uint8_t> out, std::uint32_t timeoutMillis) noexcept;
    /// Everything the child writes until it exits or `timeoutMillis` pass. False on
    /// timeout (the child is still running).
    [[nodiscard]] bool readAll(std::string& out, std::uint32_t timeoutMillis);

    [[nodiscard]] void* handle() const noexcept {
        return m_process;
    }

private:
    void closeHandles() noexcept;
    void* m_process{nullptr};
    void* m_stdinWrite{nullptr};
    void* m_stdoutRead{nullptr};
};

/// The child's side of its stdio: the protocol's two pipes, taken away from the CRT so
/// a plugin that prints cannot corrupt the protocol. Its stdout goes to stderr.
struct ChildStdio {
    void* in{nullptr};
    void* out{nullptr};
};
[[nodiscard]] ChildStdio claimStdio() noexcept;
[[nodiscard]] bool readExact(void* pipe, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] bool writeAll(void* pipe, std::span<const std::uint8_t> bytes) noexcept;

class Library {
public:
    Library() = default;
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    Library(Library&&) = delete;
    Library& operator=(Library&&) = delete;

    [[nodiscard]] bool load(const std::string& path, std::string& error);
    [[nodiscard]] void* symbol(const char* name) const noexcept;
    [[nodiscard]] void* native() const noexcept {
        return m_module;
    }

private:
    void* m_module{nullptr};
};

/// The file's last-write time (100 ns units) and size; false when it does not exist.
/// For a directory (a .vst3 bundle), the newest file inside it and the total size.
[[nodiscard]] bool fileStamp(const std::string& path, std::int64_t& mtime, std::uint64_t& size);

/// The directory holding the running executable or module containing this code.
[[nodiscard]] std::string thisModuleDirectory();

/// Monotonic microseconds.
[[nodiscard]] std::int64_t nowMicros() noexcept;

} // namespace adx::plugin::bridge
