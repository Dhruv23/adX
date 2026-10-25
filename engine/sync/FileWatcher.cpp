// adx-thread: main
#include "engine/sync/FileWatcher.h"

#include <array>
#include <cwctype>
#include <string>
#include <system_error>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace adx::sync {

namespace {

[[nodiscard]] std::int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

#if defined(_WIN32)

struct FileWatcher::Platform {
    HANDLE directory{INVALID_HANDLE_VALUE};
    HANDLE stop{nullptr};
    HANDLE ready{nullptr};
    std::wstring name;

    Platform() = default;
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&&) = delete;
    Platform& operator=(Platform&&) = delete;

    ~Platform() {
        if (directory != INVALID_HANDLE_VALUE) {
            CloseHandle(directory);
        }
        if (stop != nullptr) {
            CloseHandle(stop);
        }
        if (ready != nullptr) {
            CloseHandle(ready);
        }
    }

    [[nodiscard]] bool names(const wchar_t* text, std::size_t length) const noexcept {
        if (length != name.size()) {
            return false;
        }
        for (std::size_t i = 0; i < length; ++i) {
            if (std::towlower(text[i]) != std::towlower(name[i])) {
                return false;
            }
        }
        return true;
    }
};

FileWatcher::FileWatcher(std::filesystem::path file, std::chrono::milliseconds debounce)
    : m_file(std::move(file)), m_debounce(debounce), m_platform(std::make_unique<Platform>()) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(m_file, error);
    const std::filesystem::path directory = (error ? m_file : absolute).parent_path();
    m_platform->name = m_file.filename().wstring();
    m_platform->directory =
        CreateFileW(directory.wstring().c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    m_platform->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_platform->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_platform->directory == INVALID_HANDLE_VALUE || m_platform->stop == nullptr ||
        m_platform->ready == nullptr) {
        return;
    }
    m_thread = std::thread([this] { run(); });
    // Wait until the first ReadDirectoryChangesW is issued: a write made right after
    // construction must not slip in before the watch exists.
    WaitForSingleObject(m_platform->ready, 2000);
}

FileWatcher::~FileWatcher() {
    m_stop.store(true, std::memory_order_release);
    if (m_platform && m_platform->stop != nullptr) {
        SetEvent(m_platform->stop);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void FileWatcher::run() {
    // DWORD-aligned, as ReadDirectoryChangesW requires.
    alignas(DWORD) std::array<std::byte, std::size_t{64} * 1024> buffer{};
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        SetEvent(m_platform->ready);
        return;
    }
    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                              FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION;
    bool first = true;
    while (!m_stop.load(std::memory_order_acquire)) {
        ResetEvent(overlapped.hEvent);
        const BOOL issued = ReadDirectoryChangesW(m_platform->directory, buffer.data(),
                                                  static_cast<DWORD>(buffer.size()), FALSE, kFilter,
                                                  nullptr, &overlapped, nullptr);
        if (first) {
            m_watching.store(issued != FALSE, std::memory_order_release);
            SetEvent(m_platform->ready);
            first = false;
        }
        if (issued == FALSE) {
            break;
        }
        const std::array<HANDLE, 2> handles{overlapped.hEvent, m_platform->stop};
        const DWORD woke = WaitForMultipleObjects(2, handles.data(), FALSE, INFINITE);
        if (woke != WAIT_OBJECT_0) {
            CancelIoEx(m_platform->directory, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(m_platform->directory, &overlapped, &ignored, TRUE);
            break;
        }
        DWORD bytes = 0;
        if (GetOverlappedResult(m_platform->directory, &overlapped, &bytes, FALSE) == FALSE) {
            continue;
        }
        if (bytes == 0) {
            // The buffer overflowed: something changed and the details are lost, so
            // assume it was the file. A spurious reload is a no-op patch.
            touched();
            continue;
        }
        std::size_t offset = 0;
        for (;;) {
            const auto* info =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
            if (m_platform->names(info->FileName, info->FileNameLength / sizeof(wchar_t))) {
                touched();
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            offset += info->NextEntryOffset;
        }
    }
    CloseHandle(overlapped.hEvent);
}

#else

struct FileWatcher::Platform {
    std::filesystem::file_time_type time{};
    std::uintmax_t size{0};
    bool exists{false};
};

FileWatcher::FileWatcher(std::filesystem::path file, std::chrono::milliseconds debounce)
    : m_file(std::move(file)), m_debounce(debounce), m_platform(std::make_unique<Platform>()) {
    std::error_code error;
    m_platform->exists = std::filesystem::exists(m_file, error);
    if (m_platform->exists) {
        m_platform->time = std::filesystem::last_write_time(m_file, error);
        m_platform->size = std::filesystem::file_size(m_file, error);
    }
    m_watching.store(true, std::memory_order_release);
    m_thread = std::thread([this] { run(); });
}

FileWatcher::~FileWatcher() {
    m_stop.store(true, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void FileWatcher::run() {
    while (!m_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        std::error_code error;
        const bool exists = std::filesystem::exists(m_file, error);
        std::filesystem::file_time_type time{};
        std::uintmax_t size = 0;
        if (exists) {
            time = std::filesystem::last_write_time(m_file, error);
            size = std::filesystem::file_size(m_file, error);
        }
        if (exists != m_platform->exists || time != m_platform->time || size != m_platform->size) {
            m_platform->exists = exists;
            m_platform->time = time;
            m_platform->size = size;
            touched();
        }
    }
}

#endif

void FileWatcher::touched() noexcept {
    m_lastEventNs.store(nowNs(), std::memory_order_release);
    m_generation.fetch_add(1, std::memory_order_acq_rel);
}

bool FileWatcher::poll() {
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    if (generation == m_reported) {
        return false;
    }
    const std::int64_t quietNs = nowNs() - m_lastEventNs.load(std::memory_order_acquire);
    if (quietNs < std::chrono::duration_cast<std::chrono::nanoseconds>(m_debounce).count()) {
        return false;
    }
    m_reported = generation;
    return true;
}

} // namespace adx::sync
