// The audio thread's half of the render engine: the commit protocol's receiving end.
//
// FINAL_PLAN §4: a command mutates the main-thread project, then publishes an
// immutable render snapshot to the audio thread via the lock-free queue; the audio
// thread swaps the pointer between blocks and hands the retired snapshot to the
// reaper. This class is that audio-thread side - and nothing else in the engine is
// allowed to be.
//
// One queue, not one per message kind. A knob turn and the snapshot a structural
// edit produces must arrive in the order they were sent: a ParamChange posted after a
// snapshot was built has to land on *that* snapshot, not on the one it replaces and
// then be lost at the swap. Two queues cannot promise that; one can
// (phase_3.md §4.2, and §10 of that plan).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include "engine/audio/AudioBackend.h"
#include "engine/graph/Audition.h"
#include "engine/graph/Scheduler.h"
#include "engine/midi/MidiClock.h"
#include "engine/midi/MidiInput.h"
#include "engine/midi/MidiLearn.h"
#include "engine/midi/MidiOutput.h"
#include "engine/midi/MidiRouter.h"
#include "engine/midi/MidiSync.h"
#include "engine/midi/PadMap.h"
#include "engine/project/ParamRef.h"
#include "engine/project/Snapshot.h"
#include "engine/rt/BlockArena.h"
#include "engine/rt/Reaper.h"
#include "engine/rt/SpscRing.h"
#include "engine/session/SessionEngine.h"
#include "engine/transport/PlayState.h"
#include "engine/transport/TransportSet.h"

namespace adx::record {
class InputCapture;
}

namespace adx::graph {

enum class MessageKind : std::uint8_t {
    /// Swap in `snapshot` at the next block boundary. Ownership passes with it.
    Snapshot,
    /// A knob turn: write `value` to `param` in the current snapshot. No rebuild.
    Param,
    Seek,
    State,
    Loop,
    Rate,
    /// Play `tickA` (a pitch) at velocity `value` for `tickB` frames through channel
    /// `param.owner`, outside the arrangement (P5-1).
    Audition,
    /// Release every audition note.
    AuditionStop,
    /// The click: `flag` on or off, a beat every `tickA` ticks, `tickB` beats a bar.
    Metronome,
    /// One sample of `value` at arrangement sample `tickA`, straight to the output: the
    /// impulse a loopback calibration listens for (RecordSession.h).
    Ping,
    /// Live MIDI input plays channel `tickA` (a ChannelId value; kNone for none) from
    /// the next block (phase_9.md §4.1). Held notes still release where they started.
    MidiTarget,
    /// The arrangement follows an external sync source from the next block: `tickA`
    /// is a SyncFollow value (STATE.md P9-5).
    SyncFollow,
    /// A session request (phase_11.md §4.3): `tickA` is a SessionRequest::Kind, `tickB`
    /// the track in its high 32 bits and the scene row in its low.
    Session,
};

/// What drives the arrangement besides adX's own transport.
enum class SyncFollow : std::uint8_t {
    None,
    /// Incoming MIDI clock: start, stop, continue, song position, and a rate.
    Clock,
    /// Incoming MIDI time code (quarter frames): a position chased, and a rate.
    Mtc,
};

/// Everything that crosses from the main thread to the audio thread. Trivially
/// copyable, because the ring moves it by value.
struct EngineMessage {
    project::Snapshot* snapshot{nullptr};
    project::ParamRef param;
    float value{0.0F};
    std::uint32_t timeSource{0};
    /// Seek: the tick. Loop: the start tick.
    std::int64_t tickA{0};
    /// Loop: the end tick.
    std::int64_t tickB{0};
    double rate{1.0};
    MessageKind kind{MessageKind::Param};
    transport::PlayState state{transport::PlayState::Stopped};
    /// Loop: enabled.
    bool flag{false};
};

class EngineCore {
public:
    static constexpr std::size_t kQueueCapacity = 1024;

    EngineCore(transport::TransportSet& transport, rt::Reaper& reaper) noexcept
        : m_transport(transport), m_reaper(reaper) {
        m_router.setRoutes(m_routes);
    }
    EngineCore(const EngineCore&) = delete;
    EngineCore& operator=(const EngineCore&) = delete;
    EngineCore(EngineCore&&) = delete;
    EngineCore& operator=(EngineCore&&) = delete;
    ~EngineCore() = default;

    /// Main thread. False when the queue is full - the audio thread has stopped
    /// draining, or the main thread is outrunning it. Recorded as an Unbounded
    /// violation rather than left to each caller to notice (Phase 1 open issue P1-4);
    /// the caller keeps whatever it could not send and tries again.
    [[nodiscard]] bool post(const EngineMessage& message) noexcept;

    /// Audio thread. The per-block entry point: drain the queue, render, publish. `in`
    /// is the device input, or null.
    void process(float* out, const float* in, std::uint32_t frames, std::uint32_t channels,
                 rt::BlockArena& arena) noexcept;

    /// Main thread, before the stream opens: the input's channel count.
    void setInputChannels(std::uint32_t channels) noexcept {
        m_inputChannels = channels;
    }
    /// Main thread: where the input goes while recording, or null. The capture must
    /// outlive its installation; the release pairs with the audio thread's acquire.
    void setCapture(record::InputCapture* capture) noexcept {
        m_capture.store(capture, std::memory_order_release);
    }

    /// Most MIDI inputs open at once, and most live events one block takes.
    static constexpr std::size_t kMaxMidiInputs = 8;
    static constexpr std::size_t kMaxLiveEvents = 256;

    /// Main thread: a MIDI input in `slot`, drained at the top of every block, or null.
    /// Same contract as setCapture: the input outlives its installation, and is
    /// destroyed only after a block has passed since it was removed.
    /// Main thread: the pad map the input in `slot` triggers the session through, or
    /// null. The caller keeps the map alive until the engine is destroyed.
    void setPadMap(std::size_t slot, const midi::PadMap* map) noexcept {
        if (slot < kMaxMidiInputs) {
            m_pads.at(slot).store(map, std::memory_order_release);
        }
    }
    /// Pad presses that did something. Any thread.
    [[nodiscard]] std::uint64_t padPresses() const noexcept {
        return m_padPresses.load(std::memory_order_acquire);
    }
    void setMidiInput(std::size_t slot, midi::MidiInput* input) noexcept {
        if (slot < kMaxMidiInputs) {
            m_midiInputs.at(slot).store(input, std::memory_order_release);
        }
    }
    /// Most MIDI outputs open at once.
    static constexpr std::size_t kMaxMidiOutputs = 4;
    /// Main thread: a MIDI output in `slot`, or null; setMidiInput's contract. Each
    /// block it is sent the clock and time code its sync() flags ask for.
    void setMidiOutput(std::size_t slot, midi::MidiOutput* output) noexcept {
        if (slot < kMaxMidiOutputs) {
            m_midiOutputs.at(slot).store(output, std::memory_order_release);
        }
    }
    /// Main thread: log what MIDI inputs play while the arrangement rolls (a take).
    void setRecordingMidi(bool on) noexcept {
        m_recordMidi.store(on, std::memory_order_release);
    }
    /// Main thread: the next logged take message, or false when none is waiting.
    [[nodiscard]] bool takeMidi(midi::TakeEvent& out) noexcept {
        return m_take.tryPop(out);
    }
    /// Take messages lost to a full log. Any thread.
    [[nodiscard]] std::uint64_t takeLost() const noexcept {
        return m_takeLost.load(std::memory_order_acquire);
    }
    /// The audio clock MIDI inputs map their arrival times through. Any thread.
    [[nodiscard]] const midi::AudioClock& midiClock() const noexcept {
        return m_midiClock;
    }
    /// The last controller message any input sent: what learn mode binds. Any thread.
    [[nodiscard]] const midi::LastControl& lastControl() const noexcept {
        return m_lastControl;
    }
    /// Main thread: one value a learned control wrote, or false when none is waiting.
    [[nodiscard]] bool takeLearned(midi::LearnedValue& out) noexcept {
        return m_learned.tryPop(out);
    }
    /// Live events lost because a block had more than kMaxLiveEvents. Any thread.
    [[nodiscard]] std::uint64_t liveOverflow() const noexcept {
        return m_liveOverflow.load(std::memory_order_acquire);
    }

    /// The AudioThread::ProcessStep trampoline. `user` is the EngineCore.
    static void processStep(void* user, float* out, const float* in, std::uint32_t frames,
                            std::uint32_t channels, const audio::StreamTime& time,
                            rt::BlockArena& arena) noexcept;

    /// Main thread, with no audio thread running this core: takes back the snapshot
    /// currently installed so its owner can destroy it. Drains any snapshots still
    /// queued into the reaper first, so nothing published is lost.
    [[nodiscard]] project::Snapshot* detach() noexcept;
    /// The snapshot being played. For tests driving an offline engine on their own
    /// thread; anyone else reads published state instead.
    [[nodiscard]] const project::Snapshot* current() const noexcept {
        return m_current;
    }

    /// The revision of the snapshot being rendered. Any thread.
    [[nodiscard]] std::uint64_t renderedRevision() const noexcept {
        return m_renderedRevision.load(std::memory_order_acquire);
    }

    /// Snapshots swapped in since construction. Any thread. What
    /// snapshot_swap_between_blocks and param_change_without_rebuild count.
    [[nodiscard]] std::uint64_t swapCount() const noexcept {
        return m_swaps.load(std::memory_order_acquire);
    }

    [[nodiscard]] Scheduler& scheduler() noexcept {
        return m_scheduler;
    }
    /// The session's clip runtime: its cell states and progress are readable from any
    /// thread (SessionEngine::state, progress).
    [[nodiscard]] const session::SessionEngine& session() const noexcept {
        return m_session;
    }
    /// The same, for the main thread's one reader of the performance log (takeLogged).
    [[nodiscard]] session::SessionEngine& session() noexcept {
        return m_session;
    }
    /// Main thread, before the stream opens (SessionEngine::prepare).
    void prepareSession() {
        m_session.prepare();
    }

private:
    void drain() noexcept;
    void install(project::Snapshot* snapshot) noexcept;
    /// A controller message through the snapshot's learned bindings.
    void learn(const midi::MidiEvent& event) noexcept;
    /// A message from a pad controller's input: a session request. True when the
    /// message is the pads' and plays nothing.
    bool padPress(const midi::MidiEvent& event) noexcept;
    /// Live pitch bend (P9-6): an input channel's bend, and the RPN 0 range that scales
    /// it, sent to the notes it holds as pitch glides.
    void liveController(const midi::MidiEvent& event) noexcept;
    /// The cents a bend message means on its channel.
    [[nodiscard]] float bendCents(const midi::MidiEvent& event) const noexcept;
    /// Logs this block's raw input to the take, at arrangement ticks. After the render.
    void logTake() noexcept;
    /// A system message from an input: clock and time code for following.
    void syncIn(const midi::MidiEvent& event) noexcept;
    /// Once a block, before the render: the followed source's rate and position.
    void follow() noexcept;
    /// Once a block, after the render: clock and time code to every output that wants them.
    void syncOut(const midi::SyncBlock& block) noexcept;
    /// This block's auditions followed by its live MIDI, routed, as one list.
    [[nodiscard]] std::span<const AuditionEvent>
    gatherLive(std::span<const AuditionEvent> auditions, std::uint32_t frames) noexcept;

    transport::TransportSet& m_transport;
    rt::Reaper& m_reaper;
    rt::SpscRing<EngineMessage, kQueueCapacity> m_queue;
    project::Snapshot* m_current{nullptr};
    Scheduler m_scheduler;
    Audition m_audition;
    Metronome m_metronome;
    std::atomic<std::uint64_t> m_renderedRevision{0};
    std::atomic<std::uint64_t> m_swaps{0};
    std::atomic<record::InputCapture*> m_capture{nullptr};
    std::uint32_t m_inputChannels{0};
    std::int64_t m_pingAt{-1};
    float m_pingLevel{0.0F};

    // Live MIDI. The router keeps a span over m_routes, so EngineCore never moves.
    std::array<std::atomic<midi::MidiInput*>, kMaxMidiInputs> m_midiInputs{};
    /// Each input's pad map, or null for an input that is not a pad controller.
    std::array<std::atomic<const midi::PadMap*>, kMaxMidiInputs> m_pads{};
    std::atomic<std::uint64_t> m_padPresses{0};
    midi::AudioClock m_midiClock;
    midi::MidiRouter m_router;
    std::array<midi::MidiRoute, 1> m_routes{midi::MidiRoute{.target = kNone}};
    std::array<midi::BlockMidi, kMaxLiveEvents> m_midiScratch{};
    std::array<AuditionEvent, kMaxLiveEvents + (Audition::kMaxVoices * 2)> m_live{};
    std::atomic<std::uint64_t> m_liveOverflow{0};
    midi::LastControl m_lastControl;
    rt::SpscRing<midi::LearnedValue, 256> m_learned;
    /// A toggle binding's state, by its index in the snapshot's list.
    std::array<bool, 128> m_toggled{};
    /// The stream frame the block being rendered starts at.
    std::uint64_t m_streamFrames{0};

    // Live bend and takes (P9-6).
    struct LiveBend {
        float cents{0.0F};
        float semitones{2.0F};
        std::uint16_t rpn{0x3FFF};
    };
    std::array<std::array<LiveBend, 16>, kMaxMidiInputs> m_bends{};
    struct HeldLive {
        std::uint32_t target{0};
        std::uint32_t noteId{0};
        std::uint8_t device{0};
        std::uint8_t channel{0};
        bool active{false};
    };
    std::array<HeldLive, 64> m_held{};
    std::atomic<bool> m_recordMidi{false};
    /// 32 KB, drained at UI rate: 2048 messages per 50 ms is far more than hands play.
    rt::SpscRing<midi::TakeEvent, 2048> m_take;
    std::atomic<std::uint64_t> m_takeLost{0};
    std::array<midi::BlockMidi, kMaxLiveEvents> m_blockRaw{};
    std::size_t m_blockRawCount{0};

    // The session (Phase 11).
    session::SessionEngine m_session;

    // Sync (P9-5).
    std::array<std::atomic<midi::MidiOutput*>, kMaxMidiOutputs> m_midiOutputs{};
    midi::ClockSender m_clockSender;
    midi::MtcSender m_mtcSender;
    std::array<midi::SyncMessage, 128> m_syncScratch{};
    SyncFollow m_follow{SyncFollow::None};
    std::uint32_t m_followRate{48000};
    midi::ClockFollower m_clockFollower{48000};
    midi::MtcFollower m_mtcFollower;
    bool m_mtcWasRunning{false};
};

} // namespace adx::graph
