// adx-thread: main
//
// The child's window for a plugin's own editor (phase_9.md §4.6): a plain Win32 window
// the plugin's view is parented into. The host embeds it by its handle; when it does
// not (or cannot), the window still works on its own, floating.
#pragma once

#include <cstdint>

namespace adx::plugin::bridge {

class EditorWindow {
public:
    EditorWindow() = default;
    ~EditorWindow();
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;
    EditorWindow(EditorWindow&&) = delete;
    EditorWindow& operator=(EditorWindow&&) = delete;

    /// A hidden window of `width` x `height` client pixels. Main thread.
    [[nodiscard]] bool create(const char* title, std::uint32_t width, std::uint32_t height);
    void resize(std::uint32_t width, std::uint32_t height) noexcept;
    void show() noexcept;
    void destroy() noexcept;
    [[nodiscard]] void* handle() const noexcept {
        return m_window;
    }

private:
    void* m_window{nullptr};
};

/// Dispatches every pending window message of this thread. The child's main loop.
void pumpMessages() noexcept;

} // namespace adx::plugin::bridge
