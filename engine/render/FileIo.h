// adx-thread: main
//
// The two things std::fopen gets wrong on Windows for an exporter: a path with
// characters outside the ANSI code page, and a seek past 2 GiB. Both matter here - a
// project in a folder named in Japanese, and a WAV header patched at the end of a long
// render.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace adx::render {

/// Opens `path` for binary writing, truncating it. Null on failure.
[[nodiscard]] inline std::FILE* openForWriting(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path.c_str(), L"wb") == 0 ? file : nullptr;
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

/// Seeks to an absolute byte offset, 64-bit.
[[nodiscard]] inline bool seekTo(std::FILE* file, std::int64_t offset) {
#if defined(_WIN32)
    return _fseeki64(file, offset, SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

} // namespace adx::render
