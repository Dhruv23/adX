// The master tap's type: every output frame, written by the callback (CallbackCore) and
// read by the analyzers (AnalyzerHub) and the capture (CaptureTap). 32768 frames, so a
// reader may fall 680 ms behind at 48 kHz before it loses one.
#pragma once

#include <cstddef>

#include "engine/rt/OverwriteRing.h"

namespace adx::audio {

inline constexpr std::size_t kScopeTapFrames = 32768;
using ScopeTapRing = rt::OverwriteRing<rt::StereoFrame, kScopeTapFrames>;

} // namespace adx::audio
