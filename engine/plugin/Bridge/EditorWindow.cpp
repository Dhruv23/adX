// adx-thread: main
#include "engine/plugin/Bridge/EditorWindow.h"

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace adx::plugin::bridge {

namespace {

constexpr const wchar_t* kClassName = L"adxPluginEditor";

LRESULT CALLBACK editorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CLOSE) {
        // Closing hides; the host closes the editor through the protocol.
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool registerClass() {
    static const bool kRegistered = [] {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof cls;
        cls.lpfnWndProc = editorProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        cls.lpszClassName = kClassName;
        return RegisterClassExW(&cls) != 0;
    }();
    return kRegistered;
}

} // namespace

EditorWindow::~EditorWindow() {
    destroy();
}

bool EditorWindow::create(const char* title, std::uint32_t width, std::uint32_t height) {
    if (!registerClass()) {
        return false;
    }
    const std::string text = title != nullptr ? title : "Plugin";
    const std::wstring wide(text.begin(), text.end());
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rect, style, FALSE);
    m_window = CreateWindowExW(0, kClassName, wide.c_str(), style | WS_CLIPCHILDREN, CW_USEDEFAULT,
                               CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    return m_window != nullptr;
}

void EditorWindow::resize(std::uint32_t width, std::uint32_t height) noexcept {
    if (m_window == nullptr) {
        return;
    }
    auto* window = static_cast<HWND>(m_window);
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rect, style, FALSE);
    SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void EditorWindow::show() noexcept {
    if (m_window != nullptr) {
        ShowWindow(static_cast<HWND>(m_window), SW_SHOW);
    }
}

void EditorWindow::destroy() noexcept {
    if (m_window != nullptr) {
        DestroyWindow(static_cast<HWND>(m_window));
        m_window = nullptr;
    }
}

void pumpMessages() noexcept {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace adx::plugin::bridge
