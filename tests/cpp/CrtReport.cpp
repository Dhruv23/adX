// Debug-CRT assertions in the test binary: printed with a stack, never a dialog.
//
// MSVC's debug runtime reports a failed library assertion - a span index out of
// range, an invalid iterator - in a modal message box. In an unattended ctest run
// that box waits forever and the run looks hung. This hook prints the report and a
// symbolised stack to stderr and ends the process, so the failure is a failed test
// with a location rather than a stalled CI job.
#if defined(_MSC_VER) && defined(_DEBUG)

#    include <array>
#    include <crtdbg.h>
#    include <cstdio>
#    include <cstdlib>
// clang-format off
#    include <windows.h>
#    include <dbghelp.h>
// clang-format on

#    pragma comment(lib, "dbghelp.lib")

namespace {

void printStack() {
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    std::array<void*, 64> frames{};
    const USHORT count = CaptureStackBackTrace(2, 64, frames.data(), nullptr);
    alignas(SYMBOL_INFO) std::array<char, sizeof(SYMBOL_INFO) + 512> buffer{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    for (USHORT i = 0; i < count; ++i) {
        const auto address = reinterpret_cast<DWORD64>(frames.at(i));
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        const bool named = SymFromAddr(process, address, &displacement, symbol) != FALSE;
        const bool located =
            SymGetLineFromAddr64(process, address, &lineDisplacement, &line) != FALSE;
        std::fprintf(stderr, "  #%u %s  %s:%lu\n", static_cast<unsigned>(i),
                     named ? symbol->Name : "?", located ? line.FileName : "?",
                     located ? line.LineNumber : 0UL);
    }
}

// _CRT_REPORT_HOOK's signature, which takes a non-const message.
// NOLINTNEXTLINE(readability-non-const-parameter)
int report(int type, char* message, int* returnValue) {
    if (type == _CRT_WARN) {
        return FALSE;
    }
    std::fprintf(stderr, "\nDebug CRT %s: %s\n", type == _CRT_ASSERT ? "assertion" : "error",
                 message != nullptr ? message : "");
    printStack();
    std::fflush(stderr);
    *returnValue = 0;
    std::_Exit(3);
}

const int kInstalled = [] {
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, report);
    return 0;
}();

} // namespace

#endif
