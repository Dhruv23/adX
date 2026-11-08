#include "engine/render/RenderEngine.h"

#include <algorithm>
#include <utility>

#include "engine/project/Project.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/rt/RtConfig.h"

namespace adx::render {
namespace {

/// A message of `kind` with every other field at its default. Built this way rather
/// than with a partial designated initialiser, which clang reports field by field.
graph::EngineMessage messageOf(graph::MessageKind kind) noexcept {
    graph::EngineMessage message;
    message.kind = kind;
    return message;
}

} // namespace

RenderEngine::RenderEngine(std::unique_ptr<audio::AudioBackend> backend, EngineOptions options)
    : m_options(options), m_audio(std::move(backend)), m_core(m_transport, m_audio.reaper()),
      m_builder(options.sampleRate, rt::kMaxBlockFrames, options.offline),
      m_workers(options.renderWorkers < 0 ? defaultRenderWorkers()
                                          : static_cast<std::uint32_t>(options.renderWorkers),
                !options.offline) {
    m_audio.setProcessStep(&graph::EngineCore::processStep, &m_core);
    // Before the stream opens: the scheduler reads its crew on the audio thread.
    m_core.scheduler().setCrew(m_workers.crew());
    m_core.prepareSession();
    m_builder.setParallel(m_workers.crew() != nullptr);
}

RenderEngine::~RenderEngine() {
    m_audio.stop();
    m_audio.close();
    // The audio thread is gone; whatever it was rendering, and whatever was still
    // queued for it, comes back here to be destroyed on the main thread.
    project::destroySnapshot(m_core.detach());
    for (const graph::EngineMessage& message : m_backlog) {
        if (message.kind == graph::MessageKind::Snapshot) {
            project::destroySnapshot(message.snapshot);
        }
    }
    static_cast<void>(m_audio.drainReaper());
}

audio::Error RenderEngine::open() {
    audio::StreamConfig config;
    config.sampleRate = m_options.sampleRate;
    config.blockFrames = m_options.blockFrames;
    config.outputChannels = m_options.outputChannels;
    config.inputDeviceId = m_options.inputDeviceId;
    config.inputChannels = m_options.inputChannels;
    m_core.setInputChannels(m_options.inputChannels);
    audio::Error error = m_audio.open(config);
    m_open = error.code == audio::Error::Code::None;
    return error;
}

void RenderEngine::ping(std::int64_t at, float level) {
    graph::EngineMessage message = messageOf(graph::MessageKind::Ping);
    message.tickA = at;
    message.value = level;
    post(message);
}

CommitResult RenderEngine::setMonitor(const project::Project& project,
                                      core::PlaylistTrackId track) {
    m_builder.setMonitor(track);
    return publish(project, m_syncedRevision, project::dirty::kAll);
}

audio::Error RenderEngine::start() {
    return m_audio.start();
}

void RenderEngine::stop() noexcept {
    m_audio.stop();
}

bool RenderEngine::snapshotThrottled() const noexcept {
    return m_snapshotsPosted - m_core.swapCount() >= kMaxSnapshotsInFlight;
}

bool RenderEngine::send(const graph::EngineMessage& message) {
    if (message.kind == graph::MessageKind::Snapshot && snapshotThrottled()) {
        return false;
    }
    if (!m_core.post(message)) {
        return false;
    }
    if (message.kind == graph::MessageKind::Snapshot) {
        ++m_snapshotsPosted;
    }
    return true;
}

void RenderEngine::post(const graph::EngineMessage& message) {
    if (message.kind == graph::MessageKind::Snapshot) {
        // A newer snapshot supersedes every snapshot still waiting here, and every knob
        // turn waiting here too: it was built from the project *after* those commands
        // ran, so it already carries their values. Transport messages are not
        // superseded by anything and keep their place.
        std::erase_if(m_backlog, [](const graph::EngineMessage& waiting) {
            if (waiting.kind == graph::MessageKind::Snapshot) {
                project::destroySnapshot(waiting.snapshot);
                return true;
            }
            return waiting.kind == graph::MessageKind::Param;
        });
    }
    // Everything queues behind anything already waiting, so order is kept.
    if (!m_backlog.empty() || !send(message)) {
        m_backlog.push_back(message);
    }
}

void RenderEngine::flushBacklog() {
    std::size_t sent = 0;
    while (sent < m_backlog.size() && send(m_backlog[sent])) {
        ++sent;
    }
    m_backlog.erase(m_backlog.begin(), m_backlog.begin() + static_cast<std::ptrdiff_t>(sent));
}

void RenderEngine::levels(std::vector<StripLevel>& out) {
    out.clear();
    m_builder.nodes().forEachMeter([&out](core::InsertId insert, const graph::MeterNode& meter) {
        // A meter that has not reported yet reads as silence, so a UI gets one stable
        // row per strip from the first call.
        rt::LevelFrame frame;
        static_cast<void>(meter.ring().readLatest(&frame, 1));
        out.push_back(StripLevel{.insert = insert, .frame = frame});
    });
}

void RenderEngine::pump() {
    static_cast<void>(m_audio.drainReaper());
    flushBacklog();
}

CommitResult RenderEngine::publish(const project::Project& project, std::uint64_t revision,
                                   project::DirtyMask dirty) {
    project::SnapshotBuildResult built = m_builder.build(project, revision, dirty);
    if (built.snapshot == nullptr) {
        return CommitResult{.rebuilt = false, .error = std::move(built.error)};
    }
    ++m_snapshotsBuilt;
    graph::EngineMessage message = messageOf(graph::MessageKind::Snapshot);
    message.snapshot = built.snapshot;
    post(message);
    m_synced = true;
    m_syncedRevision = revision;
    return CommitResult{.rebuilt = true, .error = {}};
}

CommitResult RenderEngine::setProject(const project::Project& project, std::uint64_t revision) {
    return publish(project, revision, project::dirty::kAll);
}

CommitResult RenderEngine::commit(const project::Project& project,
                                  const project::CommandStack& stack) {
    if (m_synced && stack.revision() == m_syncedRevision) {
        // Nothing was edited, but an audio clip that was standing in for its rendered
        // buffer can now play the real one.
        if (m_builder.audioReady()) {
            return publish(project, m_syncedRevision, project::dirty::kPlaylist);
        }
        return CommitResult{};
    }
    const project::DirtyMask dirty =
        m_synced ? stack.dirtySince(m_syncedRevision) : project::dirty::kAll;
    return publish(project, stack.revision(), dirty);
}

namespace {

/// The value command that sets `ref`, or null for a ref that names nothing turnable.
std::unique_ptr<project::Command> commandFor(const project::Project& project, project::ParamRef ref,
                                             float value) {
    using project::ParamKind;
    const auto number = static_cast<double>(value);
    switch (ref.kind) {
    case ParamKind::ChannelVolume:
        return std::make_unique<project::SetChannelValue>(core::ChannelId{ref.owner},
                                                          project::ChannelField::Volume, number);
    case ParamKind::ChannelPan:
        return std::make_unique<project::SetChannelValue>(core::ChannelId{ref.owner},
                                                          project::ChannelField::Pan, number);
    case ParamKind::ChannelPitch:
        return std::make_unique<project::SetChannelValue>(
            core::ChannelId{ref.owner}, project::ChannelField::PitchCents, number);
    case ParamKind::ChannelInstrumentParam: {
        const project::Channel* channel = project.find(core::ChannelId{ref.owner});
        if (channel == nullptr || ref.index >= channel->instrument.params.size()) {
            return nullptr;
        }
        return std::make_unique<project::SetChannelParam>(
            channel->id, channel->instrument.params[ref.index].name, number);
    }
    case ParamKind::InsertGain:
        return std::make_unique<project::SetInsertValue>(core::InsertId{ref.owner},
                                                         project::InsertField::Gain, number);
    case ParamKind::InsertPan:
        return std::make_unique<project::SetInsertValue>(core::InsertId{ref.owner},
                                                         project::InsertField::Pan, number);
    case ParamKind::InsertWidth:
        return std::make_unique<project::SetInsertValue>(
            core::InsertId{ref.owner}, project::InsertField::StereoSeparation, number);
    case ParamKind::SlotMix:
        return std::make_unique<project::SetSlotValue>(core::SlotId{ref.owner},
                                                       project::SlotField::Mix, number);
    case ParamKind::SlotBypass:
        return std::make_unique<project::SetSlotValue>(core::SlotId{ref.owner},
                                                       project::SlotField::Bypass, number);
    case ParamKind::SlotParam: {
        const project::Slot* slot = project.mixer.findSlot(core::SlotId{ref.owner});
        if (slot == nullptr || ref.index >= slot->params.size()) {
            return nullptr;
        }
        return std::make_unique<project::SetSlotValue>(slot->id, slot->params[ref.index].name,
                                                       number);
    }
    case ParamKind::SendLevel:
        return std::make_unique<project::SetSendLevel>(core::SendId{ref.owner}, value);
    case ParamKind::ChannelArpGate:
    case ParamKind::None:
        return nullptr;
    }
    return nullptr;
}

} // namespace

bool RenderEngine::setParam(project::Project& project, project::CommandStack& stack,
                            project::ParamRef ref, float value) {
    std::unique_ptr<project::Command> command = commandFor(project, ref, value);
    if (!command) {
        return false;
    }
    // Only a knob turn on top of an already-synced project can skip the rebuild. If
    // structural edits are pending, the next commit rebuilds anyway and picks this
    // value up with them.
    const bool wasSynced = m_synced && stack.revision() == m_syncedRevision;
    stack.execute(std::move(command), project);
    graph::EngineMessage message = messageOf(graph::MessageKind::Param);
    message.param = ref;
    message.value = value;
    post(message);
    if (wasSynced) {
        m_syncedRevision = stack.revision();
    }
    return true;
}

void RenderEngine::play() {
    graph::EngineMessage message = messageOf(graph::MessageKind::State);
    message.state = transport::PlayState::Playing;
    post(message);
}

void RenderEngine::stopPlayback() {
    graph::EngineMessage message = messageOf(graph::MessageKind::State);
    message.state = transport::PlayState::Stopped;
    post(message);
}

void RenderEngine::seek(core::Ticks at) {
    graph::EngineMessage message = messageOf(graph::MessageKind::Seek);
    message.tickA = at.value;
    post(message);
}

void RenderEngine::audition(core::ChannelId channel, std::uint8_t pitch, std::uint8_t velocity,
                            double seconds) {
    graph::EngineMessage message = messageOf(graph::MessageKind::Audition);
    message.param.owner = channel.value;
    message.tickA = pitch;
    message.value = static_cast<float>(velocity);
    message.tickB = static_cast<std::int64_t>(std::max(0.0, seconds) * m_options.sampleRate);
    post(message);
}

void RenderEngine::stopAudition() {
    post(messageOf(graph::MessageKind::AuditionStop));
}

std::size_t RenderEngine::applyLearned(project::Project& project, project::CommandStack& stack) {
    std::size_t applied = 0;
    midi::LearnedValue learned{};
    while (m_core.takeLearned(learned)) {
        // setParam posts the value again; the audio thread already has it, and the
        // repeat is a no-op write.
        if (setParam(project, stack, learned.target, learned.value)) {
            ++applied;
        }
    }
    return applied;
}

void RenderEngine::setMidiTarget(core::ChannelId channel) {
    graph::EngineMessage message = messageOf(graph::MessageKind::MidiTarget);
    message.tickA = channel.valid() ? channel.value : graph::kNone;
    post(message);
}

namespace {

graph::EngineMessage sessionMessage(session::SessionRequest::Kind kind, std::uint32_t track,
                                    std::uint32_t row, float gain = 0.0F, double value = 0.0) {
    graph::EngineMessage message;
    message.kind = graph::MessageKind::Session;
    message.tickA = static_cast<std::int64_t>(kind);
    message.tickB = static_cast<std::int64_t>((static_cast<std::uint64_t>(track) << 32U) | row);
    message.value = gain;
    message.rate = value;
    return message;
}

} // namespace

void RenderEngine::launchClip(std::uint32_t track, std::uint32_t row, float gain) {
    post(sessionMessage(session::SessionRequest::Kind::Launch, track, row, gain));
}

void RenderEngine::setSessionRate(double rate) {
    post(sessionMessage(session::SessionRequest::Kind::Rate, 0, 0, 0.0F, rate));
}

void RenderEngine::tapTempo(double seconds) {
    post(sessionMessage(session::SessionRequest::Kind::Tap, 0, 0, 0.0F, seconds));
}

void RenderEngine::setPadMap(std::size_t slot, const midi::PadMap* map) {
    if (map == nullptr) {
        m_core.setPadMap(slot, nullptr);
        return;
    }
    m_padMaps.push_back(std::make_unique<midi::PadMap>(*map));
    m_core.setPadMap(slot, m_padMaps.back().get());
}

void RenderEngine::recordPerformance(bool on) {
    post(sessionMessage(session::SessionRequest::Kind::Record, 0, 0, 0.0F, on ? 1.0 : 0.0));
}

void RenderEngine::stopClip(std::uint32_t track) {
    post(sessionMessage(session::SessionRequest::Kind::Stop, track, 0));
}

void RenderEngine::fireScene(std::uint32_t row, float gain) {
    post(sessionMessage(session::SessionRequest::Kind::Scene, 0, row, gain));
}

void RenderEngine::stopAllClips() {
    post(sessionMessage(session::SessionRequest::Kind::StopAll, 0, 0));
}

void RenderEngine::setSyncFollow(graph::SyncFollow follow) {
    graph::EngineMessage message = messageOf(graph::MessageKind::SyncFollow);
    message.tickA = static_cast<std::int64_t>(follow);
    post(message);
}

void RenderEngine::setMetronome(bool enabled, core::Ticks beat, std::uint32_t beatsPerBar) {
    graph::EngineMessage message = messageOf(graph::MessageKind::Metronome);
    message.flag = enabled;
    message.tickA = beat.value;
    message.tickB = beatsPerBar;
    post(message);
}

void RenderEngine::setLoop(transport::LoopRegion loop) {
    graph::EngineMessage message = messageOf(graph::MessageKind::Loop);
    message.tickA = loop.start.value;
    message.tickB = loop.end.value;
    message.flag = loop.enabled;
    post(message);
}

void RenderEngine::setRate(double rate) {
    graph::EngineMessage message = messageOf(graph::MessageKind::Rate);
    message.rate = rate;
    post(message);
}

core::Ticks RenderEngine::positionTicks() const noexcept {
    return core::Ticks{m_transport.arrangement().publishedTicks()};
}

std::int64_t RenderEngine::positionSamples() const noexcept {
    return m_transport.arrangement().publishedSamples();
}

transport::PlayState RenderEngine::state() const noexcept {
    return m_transport.arrangement().publishedState();
}

} // namespace adx::render
