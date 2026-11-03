// adx-thread: main
//
// PluginCore's main-thread methods: everything that touches a string, the cache or the
// sandbox's control channel.
#include "engine/plugin/PluginBinding.h"

#include <algorithm>
#include <limits>

#include "engine/plugin/PluginCore.h"
#include "engine/plugin/PluginHost.h"
#include "engine/plugin/StateBlob.h"

namespace adx::plugin {

namespace {

constexpr std::uint32_t kNoId = 0xFFFFFFFFU;

} // namespace

PluginCore::~PluginCore() {
    delete m_binding; // NOLINT(cppcoreguidelines-owning-memory): opaque to the realtime header
}

void PluginCore::configure(std::string_view format, const project::PluginRef& ref,
                           bool instrument) {
    if (m_binding == nullptr) {
        m_binding = new PluginBinding(); // NOLINT(cppcoreguidelines-owning-memory)
    }
    PluginBinding& binding = *m_binding;
    static_cast<void>(formatFromString(format, binding.format));
    binding.ref = ref;
    binding.instrument = instrument;
    binding.described = false;
    if (!ref.empty()) {
        if (const PluginDescriptor* found = PluginHost::instance().describe(binding.id())) {
            binding.descriptor = *found;
            binding.described = true;
        }
    }
}

void PluginCore::prepare(const graph::PrepareInfo& info) {
    m_sampleRate = info.sampleRate;
    m_offline = info.offline;
    m_late.reset(info.sampleRate);
    const std::size_t declared = paramCount();
    m_ids.allocate(std::max<std::size_t>(declared, 1));
    m_sent.allocate(std::max<std::size_t>(declared, 1));
    std::ranges::fill(m_ids.view(), kNoId);
    std::ranges::fill(m_sent.view(), std::numeric_limits<float>::quiet_NaN());
    // Every declared value can change in one block, plus the control-grid steps of the
    // moving ones; the shared region's own limit caps what is actually sent.
    m_changes.allocate(bridge::kMaxParamChanges);
    m_outParams.allocate(bridge::kMaxOutParams);
    if (m_binding == nullptr || m_binding->ref.empty() || m_binding->instance) {
        return;
    }
    PluginBinding& binding = *m_binding;
    StateBlob state;
    const bool hasState = !binding.ref.state.empty() && decodeBase64(binding.ref.state, state);
    if (!binding.ref.state.empty() && !hasState) {
        PluginHost::instance().notice(bridge::Notice{
            .kind = bridge::Notice::Kind::StateRejected,
            .instance = 0,
            .plugin = binding.ref.name,
            .message = "the saved state is not valid base64; the plugin starts from its defaults"});
    }
    binding.instance =
        PluginHost::instance().create(binding.id(), hasState ? &state : nullptr, info.sampleRate,
                                      std::min(info.maxBlockFrames, bridge::kMaxBlockFrames),
                                      binding.ref.isolated, binding.error);
    if (!binding.instance) {
        PluginHost::instance().notice(
            bridge::Notice{.kind = bridge::Notice::Kind::Crashed,
                           .instance = 0,
                           .plugin = binding.ref.name,
                           .message = "could not be loaded: " + binding.error});
        return;
    }
    // The cache describes the layout; the live plugin says which ids those keys are now.
    const PluginDescriptor& live = binding.instance->descriptor();
    if (!binding.described) {
        binding.descriptor = live;
        binding.described = true;
        m_ids.allocate(std::max<std::size_t>(live.params.size(), 1));
        m_sent.allocate(std::max<std::size_t>(live.params.size(), 1));
        std::ranges::fill(m_ids.view(), kNoId);
        std::ranges::fill(m_sent.view(), std::numeric_limits<float>::quiet_NaN());
    }
    const std::span<std::uint32_t> ids = m_ids.view();
    for (std::size_t i = 0; i < binding.descriptor.params.size() && i < ids.size(); ++i) {
        if (const ParamInfo* param = live.findParam(binding.descriptor.params[i].key)) {
            ids[i] = param->id;
        }
    }
    m_link = binding.instance->link();
    m_generation = m_link.health->generation.load() - 1; // resend everything first block
}

bool PluginCore::matches(std::string_view format, const project::PluginRef& ref) const noexcept {
    if (m_binding == nullptr) {
        return ref.empty();
    }
    PluginFormat parsed = PluginFormat::Clap;
    return formatFromString(format, parsed) && parsed == m_binding->format &&
           m_binding->ref.uid == ref.uid && m_binding->ref.path == ref.path &&
           m_binding->ref.isolated == ref.isolated;
}

void PluginCore::copyConfigTo(PluginCore& fresh) const {
    if (m_binding == nullptr) {
        return;
    }
    project::PluginRef ref = m_binding->ref;
    // A copy renders what the live plugin holds now, not what the file said at load.
    if (m_binding->instance) {
        StateBlob state;
        if (m_binding->instance->saveState(state)) {
            ref.state = encodeBase64(state);
        }
    }
    fresh.configure(toString(m_binding->format), ref, m_binding->instrument);
}

std::uint32_t PluginCore::paramCount() const noexcept {
    return m_binding == nullptr ? 0
                                : static_cast<std::uint32_t>(m_binding->descriptor.params.size());
}

std::string_view PluginCore::paramKey(std::uint32_t index) const noexcept {
    if (m_binding == nullptr || index >= m_binding->descriptor.params.size()) {
        return {};
    }
    return m_binding->descriptor.params[index].key;
}

std::uint32_t PluginCore::latency() const noexcept {
    return m_binding != nullptr && m_binding->instance ? m_binding->instance->latency() : 0;
}

std::string_view PluginCore::formatName() const noexcept {
    return m_binding == nullptr ? std::string_view{"clap"} : toString(m_binding->format);
}

std::uint32_t PluginCore::instanceId() const noexcept {
    return m_binding != nullptr && m_binding->instance ? m_binding->instance->id() : 0;
}

std::uint32_t indexOfParamId(const PluginCore& core, std::uint32_t id) noexcept {
    const PluginBinding* binding = core.binding();
    if (binding == nullptr) {
        return kNoId;
    }
    for (std::size_t i = 0; i < binding->descriptor.params.size(); ++i) {
        if (binding->descriptor.params[i].id == id) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return kNoId;
}

} // namespace adx::plugin
