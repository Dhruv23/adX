// adx-thread: main
#include "engine/plugin/Bridge/Platform.h"

#include "engine/plugin/Bridge/AudioLink.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <io.h>

namespace adx::plugin::bridge {

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::string lastError(const char* what) {
    const DWORD code = GetLastError();
    return std::string(what) + " failed (Win32 error " + std::to_string(code) + ")";
}

void closeHandle(void*& handle) noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}

/// CommandLineToArgvW's rules, inverted: quote when needed, double the backslashes
/// that precede a quote.
std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append((backslashes * 2) + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(arg[i]);
    }
    out.push_back(L'"');
    return out;
}

} // namespace

std::string uniqueName(const char* prefix) {
    static std::atomic<std::uint32_t> counter{0};
    return std::string("Local\\") + prefix + "-" + std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(counter.fetch_add(1)) + "-" + std::to_string(GetTickCount64());
}

// --- SharedRegion ------------------------------------------------------------------

SharedRegion::~SharedRegion() {
    close();
}

SharedRegion::SharedRegion(SharedRegion&& other) noexcept
    : m_mapping(other.m_mapping), m_view(other.m_view) {
    other.m_mapping = nullptr;
    other.m_view = nullptr;
}

SharedRegion& SharedRegion::operator=(SharedRegion&& other) noexcept {
    if (this != &other) {
        close();
        m_mapping = other.m_mapping;
        m_view = other.m_view;
        other.m_mapping = nullptr;
        other.m_view = nullptr;
    }
    return *this;
}

bool SharedRegion::create(const std::string& name, std::size_t size) {
    close();
    const auto high = static_cast<DWORD>(static_cast<std::uint64_t>(size) >> 32U);
    const auto low = static_cast<DWORD>(size & 0xFFFFFFFFU);
    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, high, low,
                                   widen(name).c_str());
    if (m_mapping == nullptr) {
        return false;
    }
    m_view = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (m_view == nullptr) {
        close();
        return false;
    }
    // Committed and zeroed by the OS; touch it so the audio thread never page-faults
    // into it for the first time.
    std::memset(m_view, 0, size);
    return true;
}

bool SharedRegion::open(const std::string& name, std::size_t size) {
    close();
    m_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, widen(name).c_str());
    if (m_mapping == nullptr) {
        return false;
    }
    m_view = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (m_view == nullptr) {
        close();
        return false;
    }
    return true;
}

void SharedRegion::close() noexcept {
    if (m_view != nullptr) {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    closeHandle(m_mapping);
}

// --- Event -------------------------------------------------------------------------

Event::~Event() {
    close();
}

Event::Event(Event&& other) noexcept : m_handle(other.m_handle), m_timer(other.m_timer) {
    other.m_handle = nullptr;
    other.m_timer = nullptr;
}

Event& Event::operator=(Event&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = other.m_handle;
        m_timer = other.m_timer;
        other.m_handle = nullptr;
        other.m_timer = nullptr;
    }
    return *this;
}

bool Event::create(const std::string& name) {
    close();
    m_handle = CreateEventW(nullptr, FALSE, FALSE, widen(name).c_str());
    // The waiting side's timer, made here on the main thread so the audio thread's
    // first timed wait creates nothing. Null before Windows 10 1803; wait() copes.
    m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                     TIMER_ALL_ACCESS);
    return m_handle != nullptr;
}

bool Event::open(const std::string& name) {
    close();
    m_handle = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, widen(name).c_str());
    return m_handle != nullptr;
}

void Event::close() noexcept {
    closeHandle(m_handle);
    closeHandle(m_timer);
}

void Event::set() const noexcept {
    if (m_handle != nullptr) {
        SetEvent(m_handle);
    }
}

bool Event::wait(std::uint32_t timeoutMicros) const noexcept {
    return waitHandle(m_handle, m_timer, timeoutMicros);
}

void signalHandle(void* event) noexcept {
    if (event != nullptr) {
        SetEvent(event);
    }
}

bool waitHandle(void* event, void* timer, std::uint32_t timeoutMicros) noexcept {
    if (event == nullptr) {
        return false;
    }
    if (timeoutMicros == 0) {
        return WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    }
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(timeoutMicros) * 10; // relative, 100 ns units
    if (timer == nullptr || SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) == FALSE) {
        // No high-resolution timer (before Windows 10 1803): round up to milliseconds.
        return WaitForSingleObject(event, (timeoutMicros + 999U) / 1000U) == WAIT_OBJECT_0;
    }
    const std::array<HANDLE, 2> handles{event, timer};
    const DWORD result = WaitForMultipleObjects(2, handles.data(), FALSE, INFINITE);
    if (result == WAIT_OBJECT_0) {
        CancelWaitableTimer(timer);
        return true;
    }
    return false;
}

int waitAny(std::span<void* const> handles, std::uint32_t timeoutMillis) noexcept {
    if (handles.empty() || handles.size() > MAXIMUM_WAIT_OBJECTS) {
        Sleep(timeoutMillis);
        return -1;
    }
    const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(),
                                                FALSE, timeoutMillis);
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handles.size()) {
        return static_cast<int>(result - WAIT_OBJECT_0);
    }
    return -1;
}

// --- ChildProcess --------------------------------------------------------------------

ChildProcess::~ChildProcess() {
    if (running()) {
        kill();
    }
    closeHandles();
}

void ChildProcess::closeHandles() noexcept {
    closeHandle(m_stdinWrite);
    closeHandle(m_stdoutRead);
    closeHandle(m_process);
}

bool ChildProcess::spawn(const std::string& exe, const std::vector<std::string>& args,
                         std::string& error) {
    closeHandles();
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof inherit;
    inherit.bInheritHandle = TRUE;

    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    if (CreatePipe(&stdinRead, &stdinWrite, &inherit, 1U << 16U) == FALSE ||
        CreatePipe(&stdoutRead, &stdoutWrite, &inherit, 1U << 16U) == FALSE) {
        error = lastError("CreatePipe");
        return false;
    }
    // Our ends stay ours.
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring commandLine = quoteArg(widen(exe));
    for (const std::string& arg : args) {
        commandLine += L" ";
        commandLine += quoteArg(widen(arg));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdinRead;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    // Only the two pipe ends and stderr are inherited, not every inheritable handle
    // this process happens to hold (a Python host has plenty).
    std::array<HANDLE, 3> inherited{stdinRead, stdoutWrite, startup.hStdError};
    std::size_t inheritedCount = 2;
    if (startup.hStdError != nullptr && startup.hStdError != INVALID_HANDLE_VALUE &&
        SetHandleInformation(startup.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) !=
            FALSE) {
        inheritedCount = 3;
    } else {
        startup.hStdError = nullptr;
    }
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<std::uint8_t> attributeStorage(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize);
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                              static_cast<void*>(inherited.data()), inheritedCount * sizeof(HANDLE),
                              nullptr, nullptr);
    STARTUPINFOEXW startupEx{};
    startupEx.StartupInfo = startup;
    startupEx.StartupInfo.cb = sizeof startupEx;
    startupEx.lpAttributeList = attributes;

    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                        nullptr, &startupEx.StartupInfo, &info);
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    if (created == FALSE) {
        error = lastError("CreateProcess") + " for " + exe;
        CloseHandle(stdinWrite);
        CloseHandle(stdoutRead);
        return false;
    }
    CloseHandle(info.hThread);
    m_process = info.hProcess;
    m_stdinWrite = stdinWrite;
    m_stdoutRead = stdoutRead;
    return true;
}

bool ChildProcess::running() const noexcept {
    return m_process != nullptr && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
}

std::uint32_t ChildProcess::exitCode() const noexcept {
    DWORD code = 0;
    if (m_process == nullptr || GetExitCodeProcess(m_process, &code) == FALSE) {
        return 0;
    }
    return code;
}

void ChildProcess::kill() noexcept {
    if (m_process != nullptr) {
        TerminateProcess(m_process, 0xDEAD);
        WaitForSingleObject(m_process, 5000);
    }
}

bool ChildProcess::waitExit(std::uint32_t timeoutMillis) const noexcept {
    return m_process == nullptr || WaitForSingleObject(m_process, timeoutMillis) == WAIT_OBJECT_0;
}

bool ChildProcess::write(std::span<const std::uint8_t> bytes) noexcept {
    return writeAll(m_stdinWrite, bytes);
}

bool ChildProcess::read(std::span<std::uint8_t> out, std::uint32_t timeoutMillis) noexcept {
    const ULONGLONG deadline = GetTickCount64() + timeoutMillis;
    std::size_t got = 0;
    DWORD idleSleep = 0;
    while (got < out.size()) {
        DWORD available = 0;
        if (PeekNamedPipe(m_stdoutRead, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
            return false; // the child closed its end
        }
        if (available == 0) {
            if (GetTickCount64() >= deadline) {
                return false;
            }
            // Spin a little, then back off: replies are usually immediate.
            Sleep(idleSleep);
            idleSleep = idleSleep < 1 ? idleSleep + 1 : 1;
            continue;
        }
        const auto want = static_cast<DWORD>(std::min<std::size_t>(out.size() - got, available));
        DWORD read = 0;
        if (ReadFile(m_stdoutRead, out.data() + got, want, &read, nullptr) == FALSE) {
            return false;
        }
        got += read;
        idleSleep = 0;
    }
    return true;
}

bool ChildProcess::readAll(std::string& out, std::uint32_t timeoutMillis) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMillis;
    std::array<char, 4096> buffer{};
    while (true) {
        DWORD available = 0;
        if (PeekNamedPipe(m_stdoutRead, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
            return true; // closed: the child has exited
        }
        if (available == 0) {
            if (GetTickCount64() >= deadline) {
                return false;
            }
            Sleep(2);
            continue;
        }
        DWORD read = 0;
        const auto want = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available));
        if (ReadFile(m_stdoutRead, buffer.data(), want, &read, nullptr) == FALSE) {
            return true;
        }
        out.append(buffer.data(), read);
    }
}

ChildStdio claimStdio() noexcept {
    ChildStdio stdio;
    // Our own copies: redirecting the CRT's descriptors below closes the handles they
    // wrapped, and these must outlive that.
    HANDLE self = GetCurrentProcess();
    DuplicateHandle(self, GetStdHandle(STD_INPUT_HANDLE), self, &stdio.in, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    DuplicateHandle(self, GetStdHandle(STD_OUTPUT_HANDLE), self, &stdio.out, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    // Whatever a plugin prints - through any CRT, or straight to the handle - goes to
    // stderr from here on. Nothing may read our protocol pipe but us.
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    SetStdHandle(STD_OUTPUT_HANDLE, err != nullptr ? err : nul);
    SetStdHandle(STD_INPUT_HANDLE, nul);
    const int errFd = _fileno(stderr);
    if (errFd >= 0) {
        _dup2(errFd, _fileno(stdout));
    }
    return stdio;
}

bool readExact(void* pipe, std::span<std::uint8_t> out) noexcept {
    std::size_t got = 0;
    while (got < out.size()) {
        DWORD read = 0;
        if (ReadFile(pipe, out.data() + got, static_cast<DWORD>(out.size() - got), &read,
                     nullptr) == FALSE ||
            read == 0) {
            return false;
        }
        got += read;
    }
    return true;
}

bool writeAll(void* pipe, std::span<const std::uint8_t> bytes) noexcept {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        DWORD wrote = 0;
        if (WriteFile(pipe, bytes.data() + sent, static_cast<DWORD>(bytes.size() - sent), &wrote,
                      nullptr) == FALSE) {
            return false;
        }
        sent += wrote;
    }
    return true;
}

// --- Library -----------------------------------------------------------------------

Library::~Library() {
    if (m_module != nullptr) {
        FreeLibrary(static_cast<HMODULE>(m_module));
    }
}

bool Library::load(const std::string& path, std::string& error) {
    // The plugin's own directory first for its dependencies, as a DAW does.
    m_module = LoadLibraryExW(widen(path).c_str(), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (m_module == nullptr) {
        error = lastError("LoadLibrary") + " for " + path;
        return false;
    }
    return true;
}

void* Library::symbol(const char* name) const noexcept {
    if (m_module == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_module), name));
}

bool fileStamp(const std::string& path, std::int64_t& mtime, std::uint64_t& size) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p{widen(path)};
    if (!fs::exists(p, ec)) {
        return false;
    }
    const auto stampOf = [](const fs::file_time_type time) {
        return static_cast<std::int64_t>(time.time_since_epoch().count());
    };
    if (!fs::is_directory(p, ec)) {
        mtime = stampOf(fs::last_write_time(p, ec));
        size = fs::file_size(p, ec);
        return !ec;
    }
    mtime = stampOf(fs::last_write_time(p, ec));
    size = 0;
    for (const auto& entry : fs::recursive_directory_iterator(p, ec)) {
        if (entry.is_regular_file(ec)) {
            mtime = std::max(mtime, stampOf(entry.last_write_time(ec)));
            size += entry.file_size(ec);
        }
    }
    return true;
}

std::string thisModuleDirectory() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisModuleDirectory), &module);
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD length =
            GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    return narrow(std::filesystem::path(path).parent_path().wstring());
}

std::int64_t nowMicros() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace adx::plugin::bridge
