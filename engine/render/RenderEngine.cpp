#include "engine/render/RenderEngine.h"

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
      m_builder(options.sampleRate, rt::kMaxBlockFrames) {
    m_audio.setProcessStep(&graph::EngineCore::processStep, &m_core);
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
    audio::Error error = m_audio.open(config);
    m_open = error.code == audio::Error::Code::None;
    return error;
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
