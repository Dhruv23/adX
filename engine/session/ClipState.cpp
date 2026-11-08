// adx-thread: realtime
#include "engine/session/ClipState.h"

namespace adx::session {

ClipStep step(ClipState from, ClipTrigger trigger, bool legato) noexcept {
    switch (from) {
    case ClipState::Stopped:
        if (trigger == ClipTrigger::Launch) {
            return {.next = ClipState::Queued};
        }
        return {.next = ClipState::Stopped};
    case ClipState::Queued:
        switch (trigger) {
        case ClipTrigger::Launch:
            return {.next = ClipState::Queued}; // the later request replaces the earlier
        case ClipTrigger::Stop:
            return {.next = ClipState::Stopped}; // cancelled before it began
        case ClipTrigger::Boundary:
            return {.next = ClipState::Playing, .start = true};
        case ClipTrigger::End:
            return {.next = ClipState::Queued};
        }
        break;
    case ClipState::Playing:
        switch (trigger) {
        case ClipTrigger::Launch:
            return {.next = ClipState::Retriggered};
        case ClipTrigger::Stop:
            return {.next = ClipState::QueuedStop};
        case ClipTrigger::Boundary:
            return {.next = ClipState::Playing};
        case ClipTrigger::End:
            return {.next = ClipState::Stopped, .stop = true, .follow = true};
        }
        break;
    case ClipState::QueuedStop:
        switch (trigger) {
        case ClipTrigger::Launch:
            return {.next = ClipState::Retriggered}; // launched again before it stopped
        case ClipTrigger::Stop:
            return {.next = ClipState::QueuedStop};
        case ClipTrigger::Boundary:
        case ClipTrigger::End:
            return {.next = ClipState::Stopped, .stop = true};
        }
        break;
    case ClipState::Retriggered:
        switch (trigger) {
        case ClipTrigger::Launch:
            return {.next = ClipState::Retriggered};
        case ClipTrigger::Stop:
            return {.next = ClipState::QueuedStop};
        case ClipTrigger::Boundary:
            // Legato carries on in phase: nothing restarts.
            return {.next = ClipState::Playing, .start = !legato};
        case ClipTrigger::End:
            return {.next = ClipState::Retriggered};
        }
        break;
    }
    return {.next = from};
}

const char* toString(ClipState state) noexcept {
    switch (state) {
    case ClipState::Stopped:
        return "stopped";
    case ClipState::Queued:
        return "queued";
    case ClipState::Playing:
        return "playing";
    case ClipState::QueuedStop:
        return "queued-stop";
    case ClipState::Retriggered:
        return "retriggered";
    }
    return "stopped";
}

} // namespace adx::session
