// adx-thread: realtime
#include "engine/midi/LiveOutputs.h"

namespace adx::midi {

LiveOutputs& liveOutputs() noexcept {
    static LiveOutputs table;
    return table;
}

} // namespace adx::midi
