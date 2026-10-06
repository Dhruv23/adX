#include "engine/format/adx/Writer.h"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

#include "engine/format/adx/Lexer.h"
#include "engine/format/adx/NoteName.h"
#include "engine/format/adx/Value.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/Project.h"

namespace adx::format {
namespace {

using project::Project;

/// Accumulates lines and hands out the indentation.
///
/// Everything the writer emits goes through here, so "exactly one blank line
/// between sections, none within" is enforced in one place rather than by every
/// caller remembering to add one.
class Emitter {
public:
    explicit Emitter(std::string_view lineEnding) : m_lineEnding(lineEnding) {}

    void line(std::string_view text) {
        m_out += text;
        m_out += m_lineEnding;
        m_pendingBlank = false;
    }

    void indented(std::size_t depth, std::string_view text) {
        m_out.append(depth * 2, ' ');
        line(text);
    }

    /// Starts a section, putting exactly one blank line before it unless it is the
    /// first thing in the file.
    void section(std::string_view header) {
        if (!m_out.empty() && !m_pendingBlank) {
            m_out += m_lineEnding;
            m_pendingBlank = true;
        }
        line(header);
    }

    [[nodiscard]] const std::string& text() const noexcept {
        return m_out;
    }

private:
    std::string m_out;
    std::string_view m_lineEnding;
    bool m_pendingBlank{false};
};

[[nodiscard]] std::string keyValue(std::string_view key, std::string_view value) {
    return std::string(key) + "=" + std::string(value);
}

/// Appends ` key=value` only when `value` differs from the default, which is what
/// keeps adding a note to a one-line diff instead of a twelve-field one.
void appendIfChanged(std::string& out, std::string_view key, double value, double fallback) {
    if (value == fallback) {
        return;
    }
    out += ' ';
    out += key;
    out += '=';
    out += formatDouble(value);
}

/// The float overload matters more than it looks. A float 0.3 widened to double is
/// 0.30000001192092896, and formatDouble faithfully writes all of it - so a gain of
/// 0.3 would come back out as a number nobody typed, and `fmt` would not be
/// idempotent because the second pass would read that longer number as exact.
void appendIfChanged(std::string& out, std::string_view key, float value, float fallback) {
    if (value == fallback) {
        return;
    }
    out += ' ';
    out += key;
    out += '=';
    out += formatFloat(value);
}

void appendIfChanged(std::string& out, std::string_view key, bool value, bool fallback) {
    if (value == fallback) {
        return;
    }
    out += ' ';
    out += key;
    out += '=';
    out += formatBool(value);
}

/// Residue belonging to one section, re-emitted verbatim at the end of it.
void emitResidue(Emitter& emitter, const project::Project& project, std::string_view type,
                 std::string_view name) {
    const ResidueBlock* block = project.residue.forSection(type, name);
    if (block == nullptr) {
        return;
    }
    for (const ResidueLine& residue : block->lines) {
        emitter.line(residue.raw);
    }
}

void writeProjectSection(Emitter& emitter, const Project& project) {
    emitter.section("[PROJECT]");
    // The writer emits v2 syntax, so the file it produces is a v2 file whatever the
    // project was read from. `meta.version` keeps the *source* version so the CLI can
    // say "this was a v1 file"; writing it here would produce v2 text labelled v1,
    // which the next load would then hand to the v1 shim.
    emitter.line(keyValue("ADX_VERSION",
                          std::to_string(std::max(project.meta.version, project::kAdxVersion))));
    if (!project.meta.title.empty()) {
        emitter.line(keyValue("TITLE", quoteAlways(project.meta.title)));
    }
    if (!project.meta.author.empty()) {
        emitter.line(keyValue("AUTHOR", quoteAlways(project.meta.author)));
    }
    if (!project.meta.created.empty()) {
        emitter.line(keyValue("CREATED", quoteAlways(project.meta.created)));
    }
    emitter.line(keyValue("TUNING", formatDouble(project.meta.tuning)));
    if (project.meta.loopEnabled) {
        emitter.line(keyValue("LOOP", formatPosition(project.meta.loopStart, project.tempo) + "-" +
                                          formatPosition(project.meta.loopEnd, project.tempo)));
    }
    emitResidue(emitter, project, "PROJECT", "");
}

void writeTempoSections(Emitter& emitter, const Project& project) {
    emitter.section("[TEMPO]");
    for (const core::TempoEvent& event : project.tempo.tempoEvents()) {
        std::string text = formatPosition(event.at, project.tempo);
        text += ' ';
        text += formatDouble(event.bpm);
        if (event.ramp) {
            text += " ramp";
        }
        emitter.line(text);
    }
    emitResidue(emitter, project, "TEMPO", "");

    emitter.section("[METER]");
    for (const core::MeterEvent& event : project.tempo.meterEvents()) {
        std::string text = formatPosition(event.at, project.tempo);
        text += ' ';
        text += std::to_string(event.numerator);
        text += '/';
        text += std::to_string(event.denominator);
        emitter.line(text);
    }
    emitResidue(emitter, project, "METER", "");
}

/// One ZONE line (docs/adx-format-v2.md, ZONE).
std::string formatZone(const Project& project, const project::SampleZone& zone) {
    const project::SampleRef* sample = project.resources.find(zone.sample);
    return "ZONE " + quoteAlways(sample != nullptr ? sample->path : std::string{}) +
           formatZoneFields(zone);
}

void writeChannel(Emitter& emitter, const Project& project, const project::Channel& channel) {
    emitter.section("[CHANNEL " + channel.name + "]");
    emitter.line(keyValue("INSTRUMENT", channel.instrument.type));
    if (!channel.instrument.voicebank.empty()) {
        emitter.line(keyValue("VOICEBANK", quoteAlways(channel.instrument.voicebank)));
    }
    emitter.line(keyValue("OUTPUT", "insert." + std::to_string(channel.output.value)));
    emitter.line(keyValue("POLYPHONY", std::to_string(channel.maxPolyphony)));
    if (channel.stealMode != project::VoiceStealMode::OldestReleased) {
        emitter.line(keyValue("STEAL", project::toString(channel.stealMode)));
    }
    emitter.line(keyValue("VOLUME", formatFloat(channel.volume)));
    emitter.line(keyValue("PAN", formatFloat(channel.pan)));
    if (channel.pitchOffsetCents != 0.0F) {
        emitter.line(keyValue("PITCH", formatFloat(channel.pitchOffsetCents)));
    }
    if (channel.muted) {
        emitter.line(keyValue("MUTE", formatBool(true)));
    }
    if (channel.soloed) {
        emitter.line(keyValue("SOLO", formatBool(true)));
    }
    if (!(channel.color == project::kDefaultColor)) {
        emitter.line(keyValue("COLOR", formatColor(channel.color)));
    }

    for (const project::ParamValue& param : channel.instrument.params) {
        std::string text = "PARAM " + param.name + "=" + formatDouble(param.value);
        if (param.hasCurve) {
            text += " curve=";
            text += formatCurve(param.curve);
        }
        emitter.line(text);
    }

    for (const project::SampleZone& zone : channel.instrument.zones) {
        emitter.line(formatZone(project, zone));
    }

    if (channel.arp.mode != project::ArpMode::Off) {
        std::string text = "ARP mode=";
        text += project::toString(channel.arp.mode);
        text += " rate=";
        text += formatFraction(channel.arp.rate);
        text += " octaves=";
        text += std::to_string(channel.arp.octaves);
        text += " gate=";
        text += formatFloat(channel.arp.gate);
        emitter.line(text);
    }

    emitResidue(emitter, project, "CHANNEL", channel.name);
}

/// Appends a note's slide, bend and lyric. Absent ones write nothing, so a file that
/// uses none of them is byte-identical to what Phase 2 wrote (phase_4.md §4.0).
void appendExtras(std::string& text, const Project& project, const project::NoteExtras* extras) {
    if (extras == nullptr) {
        return;
    }
    if (extras->slide.has_value()) {
        text += " slide=";
        text += formatSlide(*extras->slide, project.tempo);
    }
    if (!extras->pitchCurve.empty()) {
        text += " bend=";
        text += formatBend(extras->pitchCurve, project.tempo);
    }
    if (!extras->lyric.empty()) {
        text += " lyric=";
        text += quoteAlways(extras->lyric);
    }
}

void writeBreakpoints(Emitter& emitter, const Project& project,
                      const std::vector<project::Breakpoint>& points, std::size_t depth) {
    for (const project::Breakpoint& point : points) {
        std::string body = formatPosition(point.at, project.tempo);
        body += ' ';
        body += formatFloat(point.value);
        if (point.curve.kind != core::CurveKind::Linear || point.curve.tension != 0.0F) {
            body += ' ';
            body += formatCurve(point.curve);
        }
        emitter.indented(depth, body);
    }
}

void writeNoteClip(Emitter& emitter, const Project& project, const project::NoteClip& clip) {
    const project::Channel* channel = project.find(clip.channel);
    if (channel == nullptr) {
        return;
    }
    emitter.line("NOTES " + quoteIfNeeded(channel->name));

    // Sorted by (start, pitch), so two projects that differ only in edit history
    // write the same bytes.
    std::vector<const project::Note*> ordered;
    ordered.reserve(clip.notes.size());
    for (const project::Note& note : clip.notes) {
        ordered.push_back(&note);
    }
    std::ranges::sort(ordered, [](const project::Note* lhs, const project::Note* rhs) {
        return project::noteOrderBefore(*lhs, *rhs);
    });

    for (const project::Note* note : ordered) {
        std::string text = midiToNoteName(note->pitch);
        text += ' ';
        text += formatPosition(note->start, project.tempo);
        text += ' ';
        text += formatDuration(note->length, project.tempo);
        text += ' ';
        text += std::to_string(note->velocity);
        appendIfChanged(text, "pan", note->pan, 0.0F);
        appendIfChanged(text, "cutoff", note->cutoff, 0.0F);
        appendIfChanged(text, "res", note->resonance, 0.0F);
        appendIfChanged(text, "fine", static_cast<double>(note->fineTuneCents), 0.0);
        appendIfChanged(text, "rel", static_cast<double>(note->releaseVelocity),
                        static_cast<double>(project::kDefaultReleaseVelocity));
        appendIfChanged(text, "mute", note->muted, false);
        appendExtras(text, project, clip.extrasFor(note->id));
        emitter.indented(1, text);
    }
}

void writeAutomationClip(Emitter& emitter, const Project& project,
                         const project::AutomationClip& clip, std::size_t depth) {
    std::string path = project::ParamRegistry::pathOf(clip.target, project);
    if (path.empty()) {
        // The target was deleted. The original text is still the best thing to write:
        // it says what the user meant, and a reload reports it rather than losing it.
        path = clip.targetPath;
    }
    if (path.empty()) {
        return;
    }
    emitter.indented(depth, "AUTOMATION " + path);
    for (const project::Breakpoint& point : clip.points) {
        std::string text = formatPosition(point.at, project.tempo);
        text += ' ';
        text += formatFloat(point.value);
        if (point.curve.kind != core::CurveKind::Linear || point.curve.tension != 0.0F) {
            text += ' ';
            text += formatCurve(point.curve);
        }
        emitter.indented(depth + 1, text);
    }
}

void writePattern(Emitter& emitter, const Project& project, const project::Pattern& pattern) {
    emitter.section("[PATTERN " + pattern.name + "]");
    emitter.line(keyValue("LENGTH", formatDuration(pattern.length, project.tempo)));
    if (!(pattern.color == project::kDefaultColor)) {
        emitter.line(keyValue("COLOR", formatColor(pattern.color)));
    }
    for (const project::NoteClip& clip : pattern.noteClips) {
        writeNoteClip(emitter, project, clip);
    }
    for (const project::MiniNotationSource& source : pattern.mini) {
        const project::Channel* channel = project.find(source.channel);
        if (channel == nullptr) {
            continue;
        }
        emitter.line("MINI " + quoteIfNeeded(channel->name));
        for (const std::string& text : source.lines) {
            emitter.indented(1, text);
        }
    }
    for (const project::AutomationClip& clip : pattern.autoClips) {
        writeAutomationClip(emitter, project, clip, 0);
    }
    emitResidue(emitter, project, "PATTERN", pattern.name);
}

void writePlaylistItem(Emitter& emitter, const Project& project,
                       const project::PlaylistItem& item) {
    std::string text;
    const project::AutomationClip* lane = nullptr;

    if (const auto* ref = std::get_if<project::PatternRef>(&item.content)) {
        const project::Pattern* pattern = project.find(ref->pattern);
        if (pattern == nullptr) {
            return;
        }
        text = "PATTERN " + quoteIfNeeded(pattern->name);
    } else if (const auto* audio = std::get_if<project::AudioClipRef>(&item.content)) {
        const project::SampleRef* sample = project.resources.find(audio->sample);
        if (sample == nullptr) {
            return;
        }
        text = "AUDIO " + quoteAlways(sample->path);
    } else if (const auto* automation = std::get_if<project::AutomationRef>(&item.content)) {
        lane = project.findAutomationClip(automation->clip);
        if (lane == nullptr) {
            return;
        }
        std::string path = project::ParamRegistry::pathOf(lane->target, project);
        if (path.empty()) {
            path = lane->targetPath;
        }
        text = "AUTOMATION " + path;
    } else {
        return;
    }

    text += ' ';
    text += formatPosition(item.start, project.tempo);
    if (item.length.value > 0) {
        text += " length=";
        text += formatDuration(item.length, project.tempo);
    }
    if (item.sourceOffset.value > 0) {
        text += " offset=";
        text += formatDuration(item.sourceOffset, project.tempo);
    }
    if (const auto* audio = std::get_if<project::AudioClipRef>(&item.content)) {
        appendIfChanged(text, "stretch", audio->stretch, 1.0F);
        appendIfChanged(text, "pitch", audio->pitchSemitones, 0.0F);
        appendIfChanged(text, "reverse", audio->reverse, false);
    }
    appendIfChanged(text, "mute", item.muted, false);
    emitter.indented(1, text);

    if (lane != nullptr) {
        writeBreakpoints(emitter, project, lane->points, 2);
        return;
    }
    for (const project::ClipEnvelope& envelope : item.envelopes) {
        std::string target;
        if (envelope.local != project::ClipTarget::Param) {
            target = project::toString(envelope.local);
        } else {
            target = project::ParamRegistry::pathOf(envelope.target, project);
            if (target.empty()) {
                target = envelope.targetPath;
            }
        }
        if (target.empty()) {
            continue;
        }
        emitter.indented(2, "ENVELOPE " + target);
        writeBreakpoints(emitter, project, envelope.points, 3);
    }
}

void writePlaylist(Emitter& emitter, const Project& project) {
    emitter.section("[PLAYLIST]");
    for (const project::PlaylistTrack& track : project.playlist.tracks) {
        std::string text = "TRACK " + std::to_string(track.id.value);
        if (!track.name.empty()) {
            text += " name=";
            text += quoteAlways(track.name);
        }
        if (track.height != project::kDefaultTrackHeight) {
            text += " height=";
            text += std::to_string(track.height);
        }
        appendIfChanged(text, "mute", track.muted, false);
        if (!(track.color == project::kDefaultColor)) {
            text += " color=";
            text += formatColor(track.color);
        }
        emitter.line(text);
        for (const project::PlaylistItem& item : track.items) {
            writePlaylistItem(emitter, project, item);
        }
    }
    emitResidue(emitter, project, "PLAYLIST", "");
}

void writeMixer(Emitter& emitter, const Project& project) {
    emitter.section("[MIXER]");
    for (const project::Insert& insert : project.mixer.inserts) {
        std::string text = "INSERT " + std::to_string(insert.id.value);
        if (!insert.name.empty()) {
            text += " name=";
            text += quoteAlways(insert.name);
        }
        appendIfChanged(text, "gain", insert.gain, 1.0F);
        appendIfChanged(text, "pan", insert.pan, 0.0F);
        appendIfChanged(text, "width", insert.stereoSeparation, 1.0F);
        appendIfChanged(text, "mute", insert.muted, false);
        appendIfChanged(text, "solo", insert.soloed, false);
        appendIfChanged(text, "invert", insert.polarityInvert, false);
        if (!(insert.color == project::kDefaultColor)) {
            text += " color=";
            text += formatColor(insert.color);
        }
        emitter.line(text);

        for (const project::Slot& slot : insert.slots) {
            std::string body = "SLOT " + std::to_string(slot.id.value) + " " + slot.type;
            appendIfChanged(body, "mix", slot.mix, 1.0F);
            appendIfChanged(body, "bypass", slot.bypass, false);
            if (slot.sidechain.valid()) {
                body += " sidechain=insert." + std::to_string(slot.sidechain.value);
            }
            if (const project::SampleRef* impulse = project.resources.find(slot.impulse)) {
                body += " ir=" + quoteAlways(impulse->path);
            }
            for (const project::SlotParam& param : slot.params) {
                body += ' ';
                body += param.name;
                body += '=';
                body += formatDouble(param.value);
            }
            emitter.indented(1, body);
        }
        for (const project::Send& send : insert.sends) {
            std::string body = "SEND " + std::to_string(send.id.value) + " insert." +
                               std::to_string(send.target.value);
            appendIfChanged(body, "level", send.level, 0.0F);
            appendIfChanged(body, "pre", send.preFader, false);
            emitter.indented(1, body);
        }
    }
    for (const project::Route& route : project.mixer.routes) {
        emitter.line("ROUTE insert." + std::to_string(route.from.value) + " -> insert." +
                     std::to_string(route.to.value));
    }
    emitResidue(emitter, project, "MIXER", "");
}

void writeMarkers(Emitter& emitter, const Project& project) {
    if (project.markers.empty()) {
        return;
    }
    emitter.section("[MARKERS]");
    std::vector<const project::Marker*> ordered;
    ordered.reserve(project.markers.size());
    for (const project::Marker& marker : project.markers) {
        ordered.push_back(&marker);
    }
    std::ranges::stable_sort(ordered, [](const project::Marker* lhs, const project::Marker* rhs) {
        return lhs->at < rhs->at;
    });
    for (const project::Marker* marker : ordered) {
        emitter.line(formatPosition(marker->at, project.tempo) + " " + quoteAlways(marker->name));
    }
    emitResidue(emitter, project, "MARKERS", "");
}

/// Whole sections the model never understood, re-emitted after everything it did.
///
/// After, not in place: their original position is between two sections whose own
/// order is canonical, and there is no canonical answer to "between which two".
/// Putting them at the end makes the rule simple, stable and idempotent, which is
/// what Rule 2 actually needs (docs/adx-format-v2.md §9).
void writeUnknownSections(Emitter& emitter, const Project& project) {
    for (const ResidueBlock& block : project.residue.blocks) {
        if (!block.wholeSection) {
            continue;
        }
        emitter.section(block.headerRaw);
        for (const ResidueLine& line : block.lines) {
            emitter.line(line.raw);
        }
    }
}

} // namespace

std::string write(const Project& project, const WriteOptions& options) {
    Emitter emitter(options.lineEnding);

    writeProjectSection(emitter, project);
    writeTempoSections(emitter, project);
    for (const project::Channel& channel : project.channels) {
        writeChannel(emitter, project, channel);
    }
    for (const project::Pattern& pattern : project.patterns) {
        writePattern(emitter, project, pattern);
    }
    writePlaylist(emitter, project);
    writeMixer(emitter, project);
    writeMarkers(emitter, project);
    writeUnknownSections(emitter, project);

    std::string out;
    if (options.emitBom) {
        out += "\xEF\xBB\xBF";
    }
    out += emitter.text();
    return out;
}

} // namespace adx::format
