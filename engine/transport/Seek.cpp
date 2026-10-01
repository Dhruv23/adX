#include "engine/transport/Seek.h"

namespace adx::transport {

SeekEffects effectsOf(const BlockTransition& transition) noexcept {
    SeekEffects effects;
    if (transition.seeked) {
        // Seek while stopped skips the release: the stop already released everything
        // this source was playing.
        effects.releaseVoices = transition.wasRollingBeforeSeek;
        effects.resetPositionalDsp = true;
    }
    if (transition.stopped) {
        effects.releaseVoices = true;
    }
    return effects;
}

} // namespace adx::transport
