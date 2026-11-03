// adx-thread: main
#include "engine/plugin/Bridge/HostProcess.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

namespace adx::plugin::bridge {

namespace {

constexpr std::uint32_t kHelloMillis = 10'000;
/// A plugin may take a while to load - a sampler reading its library - but not forever.
constexpr std::uint32_t kLoadMillis = 30'000;
constexpr std::uint32_t kCallMillis = 5'000;
constexpr auto kWatchPeriod = std::chrono::milliseconds(20);

} // namespace

std::uint32_t nextInstanceId() noexcept {
    static std::atomic<std::uint32_t> next{1};
    return next.fetch_add(1);
}

// --- RemoteInstance ------------------------------------------------------------------

RemoteInstance::RemoteInstance(std::uint32_t id, PluginId plugin, std::uint32_t sampleRate,
                               std::uint32_t maxFrames)
    : m_id(id), m_plugin(std::move(plugin)), m_sampleRate(sampleRate),
      m_maxFrames(std::min(maxFrames, kMaxBlockFrames)) {}

RemoteInstance::~RemoteInstance() {
    // Teardown never throws: a failed unload leaves a child the process exit cleans up.
    try {
        const LinkState last = m_health.state.exchange(LinkState::Idle);
        if (const std::shared_ptr<HostProcess> owner = host()) {
            // A mute or a late bypass the watchdog had not reported yet - the render that
            // hit it ended first - is still the user's to know about.
            if (m_health.stateChanged.exchange(false)) {
                if (last == LinkState::Muted) {
                    owner->notify(Notice::Kind::NanMuted, *this,
                                  "produced an invalid sample (NaN or infinity) and was muted");
                } else if (last == LinkState::LateBypassed) {
                    owner->notify(Notice::Kind::LateBypassed, *this,
                                  "missed its deadline too often and was bypassed");
                }
            }
            owner->unload(m_id);
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): nothing to do in a destructor
    }
}

bool RemoteInstance::createObjects() {
    m_regionName = uniqueName("adx-plugin-region");
    m_goName = uniqueName("adx-plugin-go");
    m_doneName = uniqueName("adx-plugin-done");
    if (!m_region.create(m_regionName, sizeof(SharedAudio)) || !m_go.create(m_goName) ||
        !m_done.create(m_doneName)) {
        return false;
    }
    auto* shared = static_cast<SharedAudio*>(m_region.data());
    shared->control.magic = kRegionMagic;
    shared->control.version = kRegionVersion;
    return true;
}

AudioLink RemoteInstance::link() noexcept {
    return AudioLink{.shared = static_cast<SharedAudio*>(m_region.data()),
                     .go = m_go.handle(),
                     .done = m_done.handle(),
                     .timer = m_done.timer(),
                     .health = &m_health};
}

std::uint32_t RemoteInstance::latency() const noexcept {
    const auto* shared = static_cast<const SharedAudio*>(m_region.data());
    return shared == nullptr ? 0 : shared->control.latency.load();
}

std::shared_ptr<HostProcess> RemoteInstance::host() const {
    const std::scoped_lock lock(m_hostMutex);
    return m_host;
}

bool RemoteInstance::saveState(StateBlob& out) {
    const std::shared_ptr<HostProcess> owner = host();
    if (!owner) {
        return false;
    }
    ByteWriter payload;
    payload.u32(m_id);
    Status status = Status::Failed;
    std::vector<std::uint8_t> reply;
    if (!owner->call(Op::GetState, payload, status, reply, kCallMillis) || status != Status::Ok) {
        // A dead child still has the last good state: that is what a restart restores.
        out = m_lastState;
        return false;
    }
    ByteReader in(reply);
    out = in.bytes();
    m_lastState = out;
    m_hasState = true;
    return in.ok();
}

bool RemoteInstance::loadState(std::span<const std::uint8_t> bytes) {
    m_lastState.assign(bytes.begin(), bytes.end());
    m_hasState = true;
    const std::shared_ptr<HostProcess> owner = host();
    if (!owner) {
        return false;
    }
    ByteWriter payload;
    payload.u32(m_id);
    payload.bytes(bytes);
    Status status = Status::Failed;
    std::vector<std::uint8_t> reply;
    return owner->call(Op::SetState, payload, status, reply, kLoadMillis) && status == Status::Ok;
}

bool RemoteInstance::paramValues(std::vector<std::pair<std::uint32_t, double>>& out) const {
    out.clear();
    const std::shared_ptr<HostProcess> owner = host();
    if (!owner) {
        return false;
    }
    ByteWriter payload;
    payload.u32(m_id);
    Status status = Status::Failed;
    std::vector<std::uint8_t> reply;
    if (!owner->call(Op::GetParamValues, payload, status, reply, kCallMillis) ||
        status != Status::Ok) {
        return false;
    }
    ByteReader in(reply);
    const std::uint32_t count = in.u32();
    for (std::uint32_t i = 0; i < count && in.ok(); ++i) {
        const std::uint32_t id = in.u32();
        const double value = in.f64();
        out.emplace_back(id, value);
    }
    return in.ok();
}

bool RemoteInstance::paramText(std::uint32_t id, double value, std::string& out) const {
    const std::shared_ptr<HostProcess> owner = host();
    if (!owner) {
        return false;
    }
    ByteWriter payload;
    payload.u32(m_id);
    payload.u32(id);
    payload.f64(value);
    Status status = Status::Failed;
    std::vector<std::uint8_t> reply;
    if (!owner->call(Op::ParamText, payload, status, reply, kCallMillis) || status != Status::Ok) {
        return false;
    }
    ByteReader in(reply);
    out = in.str();
    return in.ok();
}

bool RemoteInstance::openEditor(std::uint64_t& window, std::uint32_t& width,
                                std::uint32_t& height) const {
    const std::shared_ptr<HostProcess> owner = host();
    if (!owner) {
        return false;
    }
    ByteWriter payload;
    payload.u32(m_id);
    Status status = Status::Failed;
    std::vector<std::uint8_t> reply;
    if (!owner->call(Op::OpenEditor, payload, status, reply, kLoadMillis) || status != Status::Ok) {
        return false;
    }
    ByteReader in(reply);
    window = in.u64();
    width = in.u32();
    height = in.u32();
    return in.ok() && window != 0;
}

void RemoteInstance::closeEditor() const {
    if (const std::shared_ptr<HostProcess> owner = host()) {
        ByteWriter payload;
        payload.u32(m_id);
        Status status = Status::Failed;
        std::vector<std::uint8_t> reply;
        static_cast<void>(owner->call(Op::CloseEditor, payload, status, reply, kCallMillis));
    }
}

void RemoteInstance::reenable() noexcept {
    LinkState expected = m_health.state.load();
    if (expected == LinkState::LateBypassed || expected == LinkState::Muted) {
        m_health.generation.fetch_add(1);
        m_health.state.compare_exchange_strong(expected, LinkState::Running);
    }
}

bool measureLatency(RemoteInstance& instance, std::uint32_t& measured) {
    constexpr std::uint32_t kBlock = 512;
    constexpr std::uint32_t kWindow = 16384;
    const AudioLink link = instance.link();
    std::vector<float> inLeft(kBlock, 0.0F);
    std::vector<float> inRight(kBlock, 0.0F);
    std::vector<float> outLeft(kBlock, 0.0F);
    std::vector<float> outRight(kBlock, 0.0F);
    std::vector<float> response;
    response.reserve(kWindow);
    std::vector<WireParam> echoes(kMaxOutParams);
    const std::array<const float*, 2> inputs{inLeft.data(), inRight.data()};
    const std::array<float*, 2> outputs{outLeft.data(), outRight.data()};
    for (std::uint32_t done = 0; done < kWindow; done += kBlock) {
        inLeft[0] = done == 0 ? 1.0F : 0.0F;
        inRight[0] = inLeft[0];
        BlockRequest request;
        request.inputs = inputs;
        request.frames = kBlock;
        request.transport.tempo = 120.0;
        request.transport.numerator = 4;
        request.transport.denominator = 4;
        request.deadlineMicros = deadlineMicros(instance.sampleRate(), kBlock, true);
        std::uint32_t count = 0;
        if (runBlock(link, request, outputs, echoes, count) != BlockResult::Done) {
            return false;
        }
        response.insert(response.end(), outLeft.begin(), outLeft.end());
    }
    float peak = 0.0F;
    for (const float sample : response) {
        peak = std::max(peak, std::abs(sample));
    }
    if (peak <= 1e-6F) {
        return false;
    }
    for (std::size_t i = 0; i < response.size(); ++i) {
        if (std::abs(response[i]) >= 0.5F * peak) {
            measured = static_cast<std::uint32_t>(i);
            return true;
        }
    }
    return false;
}

// --- HostProcess -------------------------------------------------------------------

HostProcess::HostProcess(std::string executable, NoticeSink sink)
    : m_executable(std::move(executable)), m_sink(std::move(sink)) {}

HostProcess::~HostProcess() {
    try {
        m_stop = true;
        if (m_watchdog.joinable()) {
            if (m_watchdog.get_id() == std::this_thread::get_id()) {
                m_watchdog.detach();
            } else {
                m_watchdog.join();
            }
        }
        if (m_child.running()) {
            Status status = Status::Failed;
            std::vector<std::uint8_t> reply;
            static_cast<void>(call(Op::Quit, ByteWriter{}, status, reply, 1000));
            if (!m_child.waitExit(2000)) {
                m_child.kill();
            }
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): nothing to do in a destructor
    }
}

bool HostProcess::spawnChild(std::string& error) {
    if (!m_child.spawn(m_executable, {"--serve"}, error)) {
        return false;
    }
    Status status = Status::Failed;
    std::vector<std::uint8_t> reply;
    if (!call(Op::Hello, ByteWriter{}, status, reply, kHelloMillis) || status != Status::Ok) {
        error = "the plugin host did not answer (" + m_executable + ")";
        m_child.kill();
        return false;
    }
    ByteReader in(reply);
    if (in.u32() != kProtocolVersion) {
        error = "the plugin host speaks a different protocol version";
        m_child.kill();
        return false;
    }
    m_crashed = false;
    return true;
}

bool HostProcess::start(std::string& error) {
    if (!spawnChild(error)) {
        return false;
    }
    if (!m_watchdog.joinable()) {
        m_watchdog = std::thread([this] { watch(); });
    }
    return true;
}

bool HostProcess::alive() const noexcept {
    return m_child.running();
}

bool HostProcess::call(Op op, const ByteWriter& payload, Status& status,
                       std::vector<std::uint8_t>& reply, std::uint32_t timeoutMillis) {
    const std::scoped_lock lock(m_callMutex);
    status = Status::Failed;
    reply.clear();
    if (!m_child.running()) {
        return false;
    }
    const std::uint32_t id = m_nextRequest++;
    if (!m_child.write(frame(op, id, payload.data()))) {
        return false;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMillis);
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            break;
        }
        std::array<std::uint8_t, 4> header{};
        if (!m_child.read(header, static_cast<std::uint32_t>(left.count()))) {
            break;
        }
        std::uint32_t length = 0;
        std::memcpy(&length, header.data(), 4);
        if (length < 12 || length > kMaxFrameBytes) {
            break; // corrupt: the child is not to be trusted any more
        }
        std::vector<std::uint8_t> body(length);
        if (!m_child.read(
                body, std::max<std::uint32_t>(1000, static_cast<std::uint32_t>(left.count())))) {
            break;
        }
        ByteReader in(body);
        const auto replyOp = static_cast<Op>(in.u32());
        const std::uint32_t replyId = in.u32();
        const auto replyStatus = static_cast<Status>(in.u32());
        if (replyOp != Op::Reply || replyId != id) {
            continue; // a late answer to a request that already timed out
        }
        status = replyStatus;
        reply.assign(body.begin() + 12, body.end());
        return true;
    }
    // No answer in time: the child is hung (or lying). Kill it; the watchdog reports
    // the hang and restarts it.
    if (m_child.running() && op != Op::Quit) {
        m_hungKill = true;
        m_child.kill();
    }
    return false;
}

bool HostProcess::loadInto(RemoteInstance& instance, const StateBlob* state, std::string& error) {
    LoadRequest request;
    request.instance = instance.m_id;
    request.format = instance.m_plugin.format;
    request.path = instance.m_plugin.path;
    request.uid = instance.m_plugin.uid;
    request.sampleRate = instance.m_sampleRate;
    request.maxFrames = instance.m_maxFrames;
    request.region = instance.m_regionName;
    request.goEvent = instance.m_goName;
    request.doneEvent = instance.m_doneName;
    ByteWriter payload;
    request.write(payload);
    Status status = Status::Failed;
    std::vector<std::uint8_t> reply;
    if (!call(Op::Load, payload, status, reply, kLoadMillis)) {
        error = "the plugin host did not answer the load of " + instance.m_plugin.name;
        return false;
    }
    ByteReader in(reply);
    LoadReply result;
    if (!result.read(in)) {
        error = "malformed load reply";
        return false;
    }
    if (status != Status::Ok) {
        error = result.error.empty() ? "the plugin failed to load" : result.error;
        return false;
    }
    instance.m_descriptor = std::move(result.descriptor);
    // Whatever the old child left outstanding is forgotten: the next block the audio
    // thread sends is the first this child sees.
    auto* shared = static_cast<SharedAudio*>(instance.m_region.data());
    shared->control.reply.store(shared->control.request.load(std::memory_order_acquire),
                                std::memory_order_release);
    shared->control.status.store(0);
    if (state != nullptr && !state->empty()) {
        ByteWriter statePayload;
        statePayload.u32(instance.m_id);
        statePayload.bytes(*state);
        Status stateStatus = Status::Failed;
        std::vector<std::uint8_t> stateReply;
        const bool answered =
            call(Op::SetState, statePayload, stateStatus, stateReply, kLoadMillis);
        if (!answered) {
            error = "the plugin host did not answer the state load";
            return false;
        }
        if (stateStatus != Status::Ok) {
            notify(Notice::Kind::StateRejected, instance,
                   "the saved state was rejected; the plugin starts from its defaults");
        }
        instance.m_lastState = *state;
        instance.m_hasState = true;
    }
    instance.m_health.waitingSince.store(0);
    instance.m_health.generation.fetch_add(1);
    instance.m_health.state.store(LinkState::Running, std::memory_order_release);
    return true;
}

std::shared_ptr<RemoteInstance> HostProcess::load(const PluginId& plugin, const StateBlob* state,
                                                  std::uint32_t sampleRate, std::uint32_t maxFrames,
                                                  std::string& error) {
    auto instance =
        std::make_shared<RemoteInstance>(nextInstanceId(), plugin, sampleRate, maxFrames);
    if (!instance->createObjects()) {
        error = "could not create the plugin's shared memory";
        return nullptr;
    }
    {
        const std::scoped_lock lock(instance->m_hostMutex);
        instance->m_host = shared_from_this();
    }
    if (!loadInto(*instance, state, error)) {
        const std::scoped_lock lock(instance->m_hostMutex);
        instance->m_host.reset();
        return nullptr;
    }
    {
        const std::scoped_lock lock(m_listMutex);
        m_instances.push_back(instance);
    }
    return instance;
}

bool HostProcess::adopt(const std::shared_ptr<RemoteInstance>& instance, std::string& error) {
    {
        const std::scoped_lock lock(instance->m_hostMutex);
        instance->m_host = shared_from_this();
    }
    const StateBlob* state = instance->m_hasState ? &instance->m_lastState : nullptr;
    if (!loadInto(*instance, state, error)) {
        return false;
    }
    const std::scoped_lock lock(m_listMutex);
    m_instances.push_back(instance);
    return true;
}

void HostProcess::unload(std::uint32_t id) {
    {
        const std::scoped_lock lock(m_listMutex);
        std::erase_if(m_instances, [id](const std::weak_ptr<RemoteInstance>& weak) {
            const auto locked = weak.lock();
            return !locked || locked->id() == id;
        });
    }
    if (m_child.running()) {
        ByteWriter payload;
        payload.u32(id);
        Status status = Status::Failed;
        std::vector<std::uint8_t> reply;
        static_cast<void>(call(Op::Unload, payload, status, reply, kCallMillis));
    }
}

std::vector<std::shared_ptr<RemoteInstance>> HostProcess::live() const {
    std::vector<std::shared_ptr<RemoteInstance>> out;
    const std::scoped_lock lock(m_listMutex);
    for (const auto& weak : m_instances) {
        if (auto locked = weak.lock()) {
            out.push_back(std::move(locked));
        }
    }
    return out;
}

std::size_t HostProcess::instanceCount() const {
    return live().size();
}

bool HostProcess::respawn(std::string& error,
                          std::vector<std::shared_ptr<RemoteInstance>>* isolated) {
    const std::scoped_lock lock(m_callMutex);
    m_respawning = true;
    const auto instances = live();
    for (const auto& instance : instances) {
        instance->m_health.state.store(LinkState::Idle, std::memory_order_release);
    }
    if (m_child.running()) {
        m_child.kill();
    }
    m_hungKill = false;
    bool ok = spawnChild(error);
    if (ok) {
        for (const auto& instance : instances) {
            if (isolated != nullptr && instance->isolated() && instances.size() > 1) {
                {
                    const std::scoped_lock listLock(m_listMutex);
                    std::erase_if(m_instances, [&instance](const std::weak_ptr<RemoteInstance>& w) {
                        return w.lock() == instance;
                    });
                }
                isolated->push_back(instance);
                continue;
            }
            std::string why;
            const StateBlob* state = instance->m_hasState ? &instance->m_lastState : nullptr;
            if (loadInto(*instance, state, why)) {
                notify(Notice::Kind::Restarted, *instance, "restarted with its last state");
            } else {
                instance->m_health.state.store(LinkState::Crashed);
                notify(Notice::Kind::Crashed, *instance, "could not be reloaded: " + why);
                ok = false;
                error = why;
            }
        }
    } else {
        m_crashed = true;
        for (const auto& instance : instances) {
            instance->m_health.state.store(LinkState::Crashed);
        }
    }
    m_respawning = false;
    return ok;
}

void HostProcess::notify(Notice::Kind kind, const RemoteInstance& instance, std::string message) {
    if (m_sink) {
        m_sink(Notice{.kind = kind,
                      .instance = instance.id(),
                      .plugin = instance.plugin().name,
                      .message = std::move(message)});
    }
}

void HostProcess::watch() {
    while (!m_stop) {
        std::this_thread::sleep_for(kWatchPeriod);
        if (m_respawning) {
            continue;
        }
        const auto instances = live();
        if (!m_child.running()) {
            if (m_crashed) {
                continue;
            }
            if (m_hungKill.exchange(false)) {
                // Killed for hanging: restart it with its state (§4.4).
                for (const auto& instance : instances) {
                    notify(Notice::Kind::Hung, *instance, "stopped answering and was restarted");
                }
                std::string error;
                static_cast<void>(respawn(error));
                continue;
            }
            m_crashed = true;
            for (const auto& instance : instances) {
                instance->m_health.state.store(LinkState::Crashed, std::memory_order_release);
                notify(Notice::Kind::Crashed, *instance,
                       "the plugin crashed and is bypassed; restart it to bring it back");
            }
            continue;
        }
        const std::int64_t now = nowMicros();
        for (const auto& instance : instances) {
            LinkHealth& health = instance->m_health;
            const std::int64_t since = health.waitingSince.load();
            const LinkState state = health.state.load();
            if (since != 0 && now - since > kHangMicros &&
                (state == LinkState::Running || state == LinkState::LateBypassed)) {
                m_hungKill = true;
                m_child.kill();
                break;
            }
            if (health.stateChanged.exchange(false)) {
                if (state == LinkState::LateBypassed) {
                    notify(Notice::Kind::LateBypassed, *instance,
                           "missed its deadline " + std::to_string(kLateLimit) +
                               " times in a second and is bypassed");
                } else if (state == LinkState::Muted) {
                    notify(Notice::Kind::NanMuted, *instance,
                           "produced an invalid sample (NaN or infinity) and is muted");
                }
            }
            auto* shared = static_cast<SharedAudio*>(instance->m_region.data());
            const std::uint32_t status = shared->control.status.exchange(0);
            if ((status & kLatencyChanged) != 0) {
                notify(Notice::Kind::LatencyChanged, *instance,
                       "latency is now " + std::to_string(shared->control.latency.load()) +
                           " samples");
            }
        }
    }
}

} // namespace adx::plugin::bridge
