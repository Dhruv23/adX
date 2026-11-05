// adx-thread: main
#include "engine/render/RenderWorkers.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <string_view>

#include "engine/audio/AudioThread.h"

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
// windows.h first: avrt.h uses its types.
#    include <avrt.h>
#endif

namespace adx::render {

std::uint32_t defaultRenderWorkers() {
    constexpr std::uint32_t kMost = graph::Crew::kMaxLanes - 1;
#if defined(_MSC_VER)
    // getenv is deprecated in MSVC's eyes; _dupenv_s is its own spelling of the same.
    char* raw = nullptr;
    std::size_t length = 0;
    std::string_view value;
    if (_dupenv_s(&raw, &length, "ADX_RENDER_WORKERS") == 0 && raw != nullptr) {
        value = raw;
    }
#else
    const char* raw = std::getenv("ADX_RENDER_WORKERS");
    const std::string_view value = raw != nullptr ? std::string_view{raw} : std::string_view{};
#endif
    std::uint32_t chosen = 0;
    bool set = false;
    if (!value.empty()) {
        std::uint32_t parsed = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error == std::errc{} && end == value.data() + value.size()) {
            chosen = std::min(parsed, kMost);
            set = true;
        }
    }
#if defined(_MSC_VER)
    std::free(raw); // NOLINT(cppcoreguidelines-no-malloc) - _dupenv_s's buffer
#endif
    if (set) {
        return chosen;
    }
    const unsigned cores = std::thread::hardware_concurrency();
    return cores > 1 ? std::min(static_cast<std::uint32_t>(cores - 1), kMost) : 0;
}

RenderWorkers::RenderWorkers(std::uint32_t helpers, bool realtime) {
    const std::uint32_t count = std::min(helpers, graph::Crew::kMaxLanes - 1);
    if (count == 0) {
        return;
    }
    const std::uint32_t lanes = count + 1;
    m_scratch.assign(static_cast<std::size_t>(lanes) * graph::Crew::kScratchFloats, 0.0F);
    // Each helper's arena as big as the audio thread's: a step's events and scratch must
    // fit whichever lane runs it.
    m_arenaStorage.resize(static_cast<std::size_t>(count) * audio::kBlockArenaBytes);
    m_arenas.reserve(count);
    for (std::uint32_t h = 0; h < count; ++h) {
        m_arenas.emplace_back(m_arenaStorage.data() +
                                  (static_cast<std::size_t>(h) * audio::kBlockArenaBytes),
                              audio::kBlockArenaBytes);
    }
    m_crew.configure(lanes, m_scratch, m_arenas);
    m_threads.reserve(count);
    for (std::uint32_t lane = 1; lane < lanes; ++lane) {
        m_threads.emplace_back([this, lane, realtime] { work(lane, realtime); });
    }
}

RenderWorkers::~RenderWorkers() {
    m_stopping.store(true, std::memory_order_release);
    m_crew.wakeAll();
    for (std::thread& thread : m_threads) {
        thread.join();
    }
}

void RenderWorkers::work(std::uint32_t lane, bool realtime) {
#if defined(_WIN32)
    SetThreadDescription(GetCurrentThread(), L"adX render helper");
    HANDLE task = nullptr;
    if (realtime) {
        DWORD taskIndex = 0;
        task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (task != nullptr) {
            AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
        } else {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        }
    }
#else
    static_cast<void>(realtime);
#endif
    std::uint64_t seen = m_crew.jobs();
    while (!m_stopping.load(std::memory_order_acquire)) {
        seen = m_crew.helpOnce(lane, seen, m_stopping);
    }
#if defined(_WIN32)
    if (task != nullptr) {
        AvRevertMmThreadCharacteristics(task);
    }
#endif
}

} // namespace adx::render
