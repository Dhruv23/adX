#include "engine/project/ParamRegistry.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <vector>

#include "engine/project/Project.h"

namespace adx::project {
namespace {

struct Segment {
    std::string text;
    std::uint32_t offset{0};
    std::uint32_t length{0};
};

/// Splits a dotted path, honouring quoted segments so that
/// `channel."Hardstyle Kick".drive` is three segments and not four.
[[nodiscard]] bool splitPath(std::string_view path, std::vector<Segment>& out) {
    out.clear();
    std::size_t index = 0;
    while (index <= path.size()) {
        Segment segment;
        segment.offset = static_cast<std::uint32_t>(index);

        if (index < path.size() && path[index] == '"') {
            ++index;
            bool closed = false;
            while (index < path.size()) {
                if (path[index] == '\\' && index + 1 < path.size()) {
                    segment.text.push_back(path[index + 1]);
                    index += 2;
                    continue;
                }
                if (path[index] == '"') {
                    ++index;
                    closed = true;
                    break;
                }
                segment.text.push_back(path[index]);
                ++index;
            }
            if (!closed) {
                return false;
            }
        } else {
            while (index < path.size() && path[index] != '.') {
                segment.text.push_back(path[index]);
                ++index;
            }
        }

        segment.length = static_cast<std::uint32_t>(index) - segment.offset;
        out.push_back(std::move(segment));

        if (index >= path.size()) {
            break;
        }
        if (path[index] != '.') {
            return false;
        }
        ++index;
        if (index == path.size()) {
            // A trailing dot leaves an empty final segment, which resolve() rejects
            // as a malformed path rather than silently ignoring.
            out.push_back(Segment{.text = {}, .offset = static_cast<std::uint32_t>(index)});
            break;
        }
    }
    return !out.empty();
}

[[nodiscard]] std::string joinFrom(const std::vector<Segment>& segments, std::size_t first) {
    std::string out;
    for (std::size_t i = first; i < segments.size(); ++i) {
        if (i > first) {
            out.push_back('.');
        }
        out += segments[i].text;
    }
    return out;
}

[[nodiscard]] bool parseUnsigned(std::string_view text, std::uint32_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] ParamResolution failure(ResolveError error, const Segment& segment) {
    return ParamResolution{.ref = {},
                           .error = error,
                           .segmentOffset = segment.offset,
                           .segmentLength = segment.length};
}

[[nodiscard]] ParamResolution success(ParamKind kind, std::uint32_t owner, std::uint16_t index) {
    return ParamResolution{.ref = ParamRef{.owner = owner, .index = index, .kind = kind}};
}

constexpr ParamDescriptor kUnknownDescriptor{.name = "",
                                             .minimum = -1e9F,
                                             .maximum = 1e9F,
                                             .defaultValue = 0.0F,
                                             .unit = Unit::Normalized,
                                             .scale = ScaleKind::Linear};

/// Descriptors for the parameters that exist so far.
///
/// "So far" is the operative phrase: Phase 2 owns the channel and mixer parameters
/// plus the instrument parameters the v1 shim produces, because those are the only
/// ones any file can currently contain. Phase 4's instruments register their own,
/// which is the reason ParamRegistry is a class rather than three free functions.
// NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table:
// one row per line, columns aligned by position. Designating every field would
// triple its width and bury the values the table exists to show.
constexpr auto kNamedParams = std::to_array<ParamDescriptor>({
    {"env.attack", 0.0F, 10.0F, 0.01F, Unit::Seconds, ScaleKind::Logarithmic},
    {"env.decay", 0.0F, 10.0F, 0.2F, Unit::Seconds, ScaleKind::Logarithmic},
    {"env.sustain", 0.0F, 1.0F, 0.7F, Unit::Normalized, ScaleKind::Linear},
    {"env.release", 0.0F, 10.0F, 0.2F, Unit::Seconds, ScaleKind::Logarithmic},
    {"drive", 0.0F, 48.0F, 0.0F, Unit::Decibels, ScaleKind::Linear},
    {"filter.cutoff", 20.0F, 20000.0F, 20000.0F, Unit::Hertz, ScaleKind::Logarithmic},
    {"filter.lfoRate", 0.0F, 40.0F, 0.0F, Unit::Hertz, ScaleKind::Logarithmic},
    {"filter.lfoDepth", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"sub.level", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"sub.wave", 0.0F, 3.0F, 0.0F, Unit::Count, ScaleKind::Stepped},
    {"sub.dropSemitones", 0.0F, 48.0F, 0.0F, Unit::Semitones, ScaleKind::Linear},
    {"sub.dropMs", 0.0F, 1000.0F, 50.0F, Unit::Milliseconds, ScaleKind::Logarithmic},
    {"noise.level", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"noise.type", 0.0F, 1.0F, 0.0F, Unit::Count, ScaleKind::Stepped},
    {"resfilter.type", 0.0F, 2.0F, 0.0F, Unit::Count, ScaleKind::Stepped},
    {"resfilter.cutoff", 20.0F, 20000.0F, 20000.0F, Unit::Hertz, ScaleKind::Logarithmic},
    {"resfilter.resonance", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"resfilter.envAmount", -1.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"resfilter.keyTrack", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"filterEnv.attack", 0.0F, 10.0F, 0.005F, Unit::Seconds, ScaleKind::Logarithmic},
    {"filterEnv.decay", 0.0F, 10.0F, 0.15F, Unit::Seconds, ScaleKind::Logarithmic},
    {"filterEnv.sustain", 0.0F, 1.0F, 0.2F, Unit::Normalized, ScaleKind::Linear},
    {"filterEnv.release", 0.0F, 10.0F, 0.1F, Unit::Seconds, ScaleKind::Logarithmic},
    {"formant.vowelA", 0.0F, 4.0F, 0.0F, Unit::Count, ScaleKind::Stepped},
    {"formant.vowelB", 0.0F, 4.0F, 0.0F, Unit::Count, ScaleKind::Stepped},
    {"formant.morph", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"formant.amount", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"vibrato.rate", 0.0F, 20.0F, 0.0F, Unit::Hertz, ScaleKind::Logarithmic},
    {"vibrato.depthCents", 0.0F, 200.0F, 0.0F, Unit::Cents, ScaleKind::Linear},
    {"vibrato.delayMs", 0.0F, 5000.0F, 0.0F, Unit::Milliseconds, ScaleKind::Logarithmic},
    {"glide.ms", 0.0F, 5000.0F, 0.0F, Unit::Milliseconds, ScaleKind::Logarithmic},
    {"osc.wave", 0.0F, 3.0F, 0.0F, Unit::Count, ScaleKind::Stepped},
    {"osc.unison", 1.0F, 16.0F, 1.0F, Unit::Count, ScaleKind::Stepped},
    {"osc.detuneCents", 0.0F, 100.0F, 0.0F, Unit::Cents, ScaleKind::Linear},
    {"osc.pulseWidth", 0.0F, 1.0F, 0.5F, Unit::Normalized, ScaleKind::Linear},
    // Effect-slot parameters the v1 shim emits. Phase 4 owns what they do.
    {"mix", 0.0F, 1.0F, 1.0F, Unit::Normalized, ScaleKind::Linear},
    {"room", 0.0F, 1.0F, 0.5F, Unit::Normalized, ScaleKind::Linear},
    {"damp", 0.0F, 1.0F, 0.5F, Unit::Normalized, ScaleKind::Linear},
    {"amount", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"feedback", 0.0F, 1.0F, 0.3F, Unit::Normalized, ScaleKind::Linear},
    {"timeMs", 0.0F, 5000.0F, 375.0F, Unit::Milliseconds, ScaleKind::Logarithmic},
    {"releaseMs", 0.0F, 5000.0F, 120.0F, Unit::Milliseconds, ScaleKind::Logarithmic},
    {"bits", 1.0F, 24.0F, 16.0F, Unit::Count, ScaleKind::Stepped},
    {"rate", 0.0F, 48000.0F, 44100.0F, Unit::Hertz, ScaleKind::Logarithmic},
    {"depth", 0.0F, 1.0F, 0.5F, Unit::Normalized, ScaleKind::Linear},
    {"low", -24.0F, 24.0F, 0.0F, Unit::Decibels, ScaleKind::Linear},
    {"mid", -24.0F, 24.0F, 0.0F, Unit::Decibels, ScaleKind::Linear},
    {"high", -24.0F, 24.0F, 0.0F, Unit::Decibels, ScaleKind::Linear},
    {"enabled", 0.0F, 1.0F, 1.0F, Unit::Boolean, ScaleKind::Stepped},
    // The additive engine's sixteen overtone amplitudes. Spelled out rather than
    // pattern-matched because describeNamed returns a pointer into this table and a
    // synthesised descriptor would have nowhere to live.
    {"harmonic.1", 0.0F, 1.0F, 1.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.2", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.3", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.4", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.5", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.6", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.7", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.8", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.9", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.10", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.11", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.12", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.13", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.14", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.15", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"harmonic.16", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
});

/// One descriptor per fixed ParamKind, indexed by the enum's value.
constexpr std::array<ParamDescriptor, 13> kKindDescriptors{{
    {"", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"volume", 0.0F, 2.0F, 1.0F, Unit::Ratio, ScaleKind::Logarithmic},
    {"pan", -1.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"pitch", -4800.0F, 4800.0F, 0.0F, Unit::Cents, ScaleKind::Linear},
    {"arp.gate", 0.0F, 1.0F, 0.8F, Unit::Normalized, ScaleKind::Linear},
    {"", -1e9F, 1e9F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"gain", 0.0F, 2.0F, 1.0F, Unit::Ratio, ScaleKind::Logarithmic},
    {"pan", -1.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"width", 0.0F, 2.0F, 1.0F, Unit::Ratio, ScaleKind::Linear},
    {"mix", 0.0F, 1.0F, 1.0F, Unit::Normalized, ScaleKind::Linear},
    {"bypass", 0.0F, 1.0F, 0.0F, Unit::Boolean, ScaleKind::Stepped},
    {"", -1e9F, 1e9F, 0.0F, Unit::Normalized, ScaleKind::Linear},
    {"level", 0.0F, 1.0F, 0.0F, Unit::Normalized, ScaleKind::Linear},
}};
// NOLINTEND(modernize-use-designated-initializers)

[[nodiscard]] ParamResolution resolveChannel(const std::vector<Segment>& segments,
                                             const Project& project) {
    if (segments.size() < 3) {
        return failure(ResolveError::MalformedPath, segments.back());
    }
    const Channel* channel = project.findChannelByName(segments[1].text);
    if (channel == nullptr) {
        return failure(ResolveError::UnknownChannel, segments[1]);
    }

    if (segments.size() == 3) {
        const std::string_view leaf = segments[2].text;
        if (leaf == "volume") {
            return success(ParamKind::ChannelVolume, channel->id.value, 0);
        }
        if (leaf == "pan") {
            return success(ParamKind::ChannelPan, channel->id.value, 0);
        }
        if (leaf == "pitch") {
            return success(ParamKind::ChannelPitch, channel->id.value, 0);
        }
    }
    if (segments.size() == 4 && segments[2].text == "arp" && segments[3].text == "gate") {
        return success(ParamKind::ChannelArpGate, channel->id.value, 0);
    }

    const std::string name = joinFrom(segments, 2);
    const auto match = std::ranges::find(channel->instrument.params, name, &ParamValue::name);
    if (match == channel->instrument.params.end()) {
        return failure(ResolveError::UnknownParameter, segments[2]);
    }
    const auto index =
        static_cast<std::uint16_t>(std::distance(channel->instrument.params.begin(), match));
    return success(ParamKind::ChannelInstrumentParam, channel->id.value, index);
}

[[nodiscard]] ParamResolution resolveSlot(const std::vector<Segment>& segments,
                                          const Project& project) {
    std::uint32_t slotId = 0;
    if (!parseUnsigned(segments[3].text, slotId)) {
        return failure(ResolveError::MalformedPath, segments[3]);
    }
    const Slot* slot = project.mixer.findSlot(core::SlotId{slotId});
    if (slot == nullptr) {
        return failure(ResolveError::UnknownSlot, segments[3]);
    }
    if (segments.size() == 5 && segments[4].text == "mix") {
        return success(ParamKind::SlotMix, slotId, 0);
    }
    if (segments.size() == 5 && segments[4].text == "bypass") {
        return success(ParamKind::SlotBypass, slotId, 0);
    }

    const std::string name = joinFrom(segments, 4);
    const auto match = std::ranges::find(slot->params, name, &SlotParam::name);
    if (match == slot->params.end()) {
        return failure(ResolveError::UnknownParameter, segments[4]);
    }
    const auto index = static_cast<std::uint16_t>(std::distance(slot->params.begin(), match));
    return success(ParamKind::SlotParam, slotId, index);
}

[[nodiscard]] ParamResolution resolveInsert(const std::vector<Segment>& segments,
                                            const Project& project) {
    if (segments.size() < 3) {
        return failure(ResolveError::MalformedPath, segments.back());
    }
    std::uint32_t insertId = 0;
    if (!parseUnsigned(segments[1].text, insertId)) {
        return failure(ResolveError::MalformedPath, segments[1]);
    }
    if (project.mixer.find(core::InsertId{insertId}) == nullptr) {
        return failure(ResolveError::UnknownInsert, segments[1]);
    }

    if (segments.size() == 3) {
        const std::string_view leaf = segments[2].text;
        if (leaf == "gain") {
            return success(ParamKind::InsertGain, insertId, 0);
        }
        if (leaf == "pan") {
            return success(ParamKind::InsertPan, insertId, 0);
        }
        if (leaf == "width") {
            return success(ParamKind::InsertWidth, insertId, 0);
        }
        return failure(ResolveError::UnknownParameter, segments[2]);
    }

    if (segments[2].text == "slot" && segments.size() >= 5) {
        return resolveSlot(segments, project);
    }
    if (segments[2].text == "send" && segments.size() == 5 && segments[4].text == "level") {
        std::uint32_t sendId = 0;
        if (!parseUnsigned(segments[3].text, sendId)) {
            return failure(ResolveError::MalformedPath, segments[3]);
        }
        if (project.mixer.findSend(core::SendId{sendId}) == nullptr) {
            return failure(ResolveError::UnknownSend, segments[3]);
        }
        return success(ParamKind::SendLevel, sendId, 0);
    }
    return failure(ResolveError::UnknownParameter, segments[2]);
}

} // namespace

ParamResolution ParamRegistry::resolve(std::string_view path, const Project& project) {
    std::vector<Segment> segments;
    if (!splitPath(path, segments) || segments.empty() || segments.front().text.empty()) {
        return ParamResolution{.ref = {},
                               .error = ResolveError::MalformedPath,
                               .segmentOffset = 0,
                               .segmentLength = static_cast<std::uint32_t>(path.size())};
    }
    if (std::ranges::any_of(segments, [](const Segment& s) { return s.text.empty(); })) {
        return failure(ResolveError::MalformedPath, segments.back());
    }

    if (segments.front().text == "channel") {
        return resolveChannel(segments, project);
    }
    if (segments.front().text == "insert") {
        return resolveInsert(segments, project);
    }
    return failure(ResolveError::MalformedPath, segments.front());
}

std::string ParamRegistry::pathOf(ParamRef ref, const Project& project) {
    const auto channelPath = [&](std::string_view leaf) {
        const Channel* channel = project.find(core::ChannelId{ref.owner});
        if (channel == nullptr) {
            return std::string{};
        }
        return "channel." + quoteSegment(channel->name) + "." + std::string(leaf);
    };
    const auto insertPath = [&](core::InsertId id, std::string_view tail) {
        if (project.mixer.find(id) == nullptr) {
            return std::string{};
        }
        return "insert." + std::to_string(id.value) + "." + std::string(tail);
    };

    switch (ref.kind) {
    case ParamKind::None:
        return {};
    case ParamKind::ChannelVolume:
        return channelPath("volume");
    case ParamKind::ChannelPan:
        return channelPath("pan");
    case ParamKind::ChannelPitch:
        return channelPath("pitch");
    case ParamKind::ChannelArpGate:
        return channelPath("arp.gate");
    case ParamKind::ChannelInstrumentParam: {
        const Channel* channel = project.find(core::ChannelId{ref.owner});
        if (channel == nullptr || ref.index >= channel->instrument.params.size()) {
            return {};
        }
        return channelPath(channel->instrument.params[ref.index].name);
    }
    case ParamKind::InsertGain:
        return insertPath(core::InsertId{ref.owner}, "gain");
    case ParamKind::InsertPan:
        return insertPath(core::InsertId{ref.owner}, "pan");
    case ParamKind::InsertWidth:
        return insertPath(core::InsertId{ref.owner}, "width");
    case ParamKind::SlotMix:
    case ParamKind::SlotBypass:
    case ParamKind::SlotParam: {
        const core::SlotId slotId{ref.owner};
        const Slot* slot = project.mixer.findSlot(slotId);
        const core::InsertId insertId = project.mixer.ownerOfSlot(slotId);
        if (slot == nullptr || !insertId.valid()) {
            return {};
        }
        std::string leaf;
        if (ref.kind == ParamKind::SlotMix) {
            leaf = "mix";
        } else if (ref.kind == ParamKind::SlotBypass) {
            leaf = "bypass";
        } else if (ref.index < slot->params.size()) {
            leaf = slot->params[ref.index].name;
        } else {
            return {};
        }
        return insertPath(insertId, "slot." + std::to_string(slotId.value) + "." + leaf);
    }
    case ParamKind::SendLevel: {
        const core::SendId sendId{ref.owner};
        const core::InsertId insertId = project.mixer.ownerOfSend(sendId);
        if (!insertId.valid()) {
            return {};
        }
        return insertPath(insertId, "send." + std::to_string(sendId.value) + ".level");
    }
    }
    return {};
}

const ParamDescriptor& ParamRegistry::describe(ParamKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kKindDescriptors.size() ? kKindDescriptors.at(index) : kUnknownDescriptor;
}

const ParamDescriptor* ParamRegistry::describeNamed(std::string_view name) noexcept {
    const auto match = std::ranges::find(kNamedParams, name, &ParamDescriptor::name);
    return match == kNamedParams.end() ? nullptr : &*match;
}

std::string ParamRegistry::quoteSegment(std::string_view name) {
    const bool bare = !name.empty() && std::ranges::all_of(name, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
    if (bare) {
        return std::string(name);
    }
    std::string out;
    out.reserve(name.size() + 2);
    out.push_back('"');
    for (const char c : name) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

} // namespace adx::project
