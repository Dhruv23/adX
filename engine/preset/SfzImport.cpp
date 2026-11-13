// adx-thread: main
#include "engine/preset/SfzImport.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "engine/project/TypeCatalog.h"

namespace adx::preset {
namespace {

using format::sfz::Region;

/// The value most regions set through `field`, and how many set another.
struct Common {
    float value{-1.0F};
    std::size_t others{0};
};

Common mostCommon(const std::vector<Region>& regions, float Region::* field) {
    std::map<float, std::size_t> counts;
    for (const Region& region : regions) {
        ++counts[region.*field];
    }
    Common out;
    std::size_t best = 0;
    for (const auto& [value, count] : counts) {
        if (count > best) {
            best = count;
            out.value = value;
        }
    }
    out.others = regions.size() - best;
    return out;
}

} // namespace

Preset presetFromSfz(format::sfz::Import& import, const SfzPresetOptions& options) {
    Preset preset;
    preset.name = options.name;
    preset.type = "sampler";
    preset.pack = options.pack;
    const auto envelope = [&](const char* opcode, const char* param, float Region::* field) {
        const Common common = mostCommon(import.regions, field);
        if (common.value >= 0.0F) {
            // The sampler's range: VCSL's xylophone asks for a 15 s release, which loads as
            // an out-of-range warning. Clamped here, and said so.
            auto value = static_cast<double>(common.value);
            const project::TypeInfo* sampler = project::findInstrumentType("sampler");
            const project::ParamDescriptor* descriptor =
                sampler != nullptr ? project::findParam(*sampler, param) : nullptr;
            if (descriptor != nullptr &&
                (value < descriptor->minimum || value > descriptor->maximum)) {
                const double clamped = std::clamp(value, static_cast<double>(descriptor->minimum),
                                                  static_cast<double>(descriptor->maximum));
                import.notes.push_back(
                    format::sfz::Note{.kind = format::sfz::NoteKind::Approximated,
                                      .opcode = opcode,
                                      .detail = std::to_string(value) + " is outside " + param +
                                                "'s range; " + std::to_string(clamped) + " instead",
                                      .line = 0,
                                      .column = 0});
                value = clamped;
            }
            preset.params.push_back(project::ParamValue{.name = param,
                                                        .value = std::round(value * 1e6) / 1e6,
                                                        .hasCurve = false,
                                                        .curve = {}});
        }
        if (common.others > 0) {
            import.notes.push_back(
                format::sfz::Note{.kind = format::sfz::NoteKind::Approximated,
                                  .opcode = opcode,
                                  .detail = std::to_string(common.others) +
                                            " region(s) differ from the instrument's own " + param,
                                  .line = 0,
                                  .column = 0});
        }
    };
    if (!import.regions.empty()) {
        envelope("ampeg_attack", "env.attack", &Region::attack);
        envelope("ampeg_decay", "env.decay", &Region::decay);
        envelope("ampeg_sustain", "env.sustain", &Region::sustain);
        envelope("ampeg_release", "env.release", &Region::release);
        envelope("amp_veltrack", "velocity", &Region::velocityTrack);
        if (std::ranges::all_of(import.regions, &Region::oneShot)) {
            preset.params.push_back(project::ParamValue{
                .name = "oneShot", .value = 1.0, .hasCurve = false, .curve = {}});
        }
    }
    preset.zones.reserve(import.regions.size());
    for (const Region& region : import.regions) {
        preset.zones.push_back(
            PresetZone{.zone = region.zone, .path = options.pathPrefix + region.sample});
    }
    return preset;
}

} // namespace adx::preset
