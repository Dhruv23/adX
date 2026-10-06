// Registers adX's scene-graph items with QML (phase_5.md §4.3).
//
// Not a qmldir plugin: Python loads this DLL with ctypes after PySide6 has loaded Qt,
// and calls adx_quick_register() once. That keeps the plugin free of Qt's plugin
// metadata checks - which refuse a debug-flagged plugin in a release Qt - and means the
// one thing that can go wrong, a Qt version mismatch, is checked here with a message
// that names both versions instead of failing as an unresolved import.
#include <QtCore/QLibraryInfo>
#include <QtCore/QVersionNumber>
#include <QtCore/QtGlobal>
#include <QtQml/qqml.h>

#include "bindings/qml/PianoRollItem.h"
#include "bindings/qml/WaveformItem.h"

namespace {

/// The Qt this plugin was compiled against, "major.minor.patch".
constexpr const char* kBuiltAgainst = QT_VERSION_STR;

} // namespace

extern "C" {

/// The Qt version this DLL was built against. Python compares it with qVersion().
// NOLINTNEXTLINE(readability-identifier-naming) - a C export; ctypes looks it up by name
__declspec(dllexport) const char* adx_quick_qt_version() {
    return kBuiltAgainst;
}

/// Registers `import Adx 1.0`: PianoRollItem and WaveformItem. Returns 0 on success,
/// 1 when the running Qt is older than the one this was built against (a plugin built
/// against a newer Qt is not loadable into an older one).
// NOLINTNEXTLINE(readability-identifier-naming) - a C export; ctypes looks it up by name
__declspec(dllexport) int adx_quick_register() {
    if (QT_VERSION_CHECK(QT_VERSION_MAJOR, QT_VERSION_MINOR, 0) >
        QT_VERSION_CHECK(QLibraryInfo::version().majorVersion(),
                         QLibraryInfo::version().minorVersion(), 0)) {
        return 1;
    }
    qmlRegisterType<adx::quick::PianoRollItem>("Adx", 1, 0, "PianoRollItem");
    qmlRegisterType<adx::quick::WaveformItem>("Adx", 1, 0, "WaveformItem");
    return 0;
}
}
