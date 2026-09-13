// Which thread is the audio thread.
//
// Separate from RtSection because the two answer different questions: this one is
// "am I the thread the backend calls", which stays true for the thread's whole
// life, while an RT section is a span of execution and can be entered by a test on
// an ordinary thread.
#pragma once

namespace adx::rt {

/// Marks the calling thread as the audio thread. Called once by the backend, from
/// inside the audio thread, before the first callback.
void registerAudioThread() noexcept;

/// Undoes registerAudioThread for the calling thread. Called when a stream stops
/// so a backend that starts a fresh thread does not leave a stale claim behind.
void unregisterAudioThread() noexcept;

/// True on the thread that called registerAudioThread.
[[nodiscard]] bool isAudioThread() noexcept;

} // namespace adx::rt
