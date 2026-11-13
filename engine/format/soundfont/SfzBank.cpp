// adx-thread: main
//
// A folder of SFZ files as a General MIDI bank (phase_12.md §4.7.5): one file per
// program, `NNN/PPP_Name.sfz`, where NNN is the bank (128 the drum kits) and PPP the
// program. It is the layout of the user's converted Windows bank (ARIAConverted/) and
// of any SF2 run through Plogue's SF22SFZ.
//
// Each region becomes a zone the gm voice plays with its own articulation: the amp
// envelope from ampeg_*, the flex EG that carries a pitch depth (the converter's SF2
// modulation envelope, egNN_pitch) as the modulation envelope to pitch, the LFO with a
// plain pitch depth as the vibrato and one with an amplitude depth as the modulation
// LFO. An LFO whose depth only comes from a controller (lfoNN_pitch_onccN: the
// converter's spelling of SF2's mod-wheel modulator) is left to the gm voice's own
// default CC1 modulator.
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <system_error>
#include <utility>

#include "engine/format/audio/AudioFileLoader.h"
#include "engine/format/audio/SampleBuffer.h"
#include "engine/format/sfz/SfzParser.h"
#include "engine/format/soundfont/SoundFont.h"

namespace adx::format::soundfont {
namespace {

namespace fs = std::filesystem;

/// The leading digits of a name: `020_Reed_Organ` -> 20; -1 for none.
int leadingNumber(std::string_view name) {
    int value = 0;
    std::size_t digits = 0;
    for (const char c : name) {
        if (c < '0' || c > '9' || digits == 6) {
            break;
        }
        value = (value * 10) + (c - '0');
        ++digits;
    }
    return digits > 0 ? value : -1;
}

/// One flex EG or LFO's opcodes, by the number in its name (eg06_time2 -> 6, "time2").
// NOLINTNEXTLINE(bugprone-exception-escape) - std::map's implicit members; main thread.
struct Modulators {
    std::map<int, std::map<std::string, double>> eg;
    std::map<int, std::map<std::string, double>> lfo;
};

Modulators modulatorsOf(const sfz::Region& region) {
    Modulators out;
    for (const auto& [name, value] : region.modulation) {
        const bool eg = name.starts_with("eg");
        const std::size_t digits = eg ? 2 : 3;
        const std::size_t underscore = name.find('_');
        if (underscore == std::string::npos || underscore <= digits) {
            continue;
        }
        const int index = leadingNumber(std::string_view(name).substr(digits));
        if (index < 0) {
            continue;
        }
        (eg ? out.eg : out.lfo)[index][name.substr(underscore + 1)] = value;
    }
    return out;
}

double get(const std::map<std::string, double>& values, const std::string& key, double fallback) {
    const auto found = values.find(key);
    return found == values.end() ? fallback : found->second;
}

/// The flex EG's points as an SF2 envelope: delay (a first point at level 0), attack (to
/// the first point at 1), hold (the points at 1 after it), decay (to the sustain point),
/// sustain (its level) and release (the point after it).
BankEnvelope envelopeOf(const std::map<std::string, double>& eg) {
    BankEnvelope env;
    const auto sustainPoint = static_cast<int>(get(eg, "sustain", -1.0));
    const auto time = [&](int k) {
        return static_cast<float>(get(eg, "time" + std::to_string(k), 0.0));
    };
    const auto level = [&](int k) {
        return static_cast<float>(get(eg, "level" + std::to_string(k), 0.0));
    };
    int k = 0;
    if (level(0) == 0.0F) {
        env.delay = time(0);
        k = 1;
    }
    env.attack = time(k);
    ++k;
    while (k < sustainPoint && level(k) >= 1.0F) {
        env.hold += time(k);
        ++k;
    }
    if (sustainPoint >= 0) {
        for (; k <= sustainPoint; ++k) {
            env.decay += time(k);
        }
        env.sustain = std::clamp(level(sustainPoint), 0.0F, 1.0F);
        env.release = time(sustainPoint + 1);
    } else {
        env.sustain = 0.0F;
    }
    return env;
}

void applyModulation(const sfz::Region& region, BankZone& zone) {
    const Modulators mods = modulatorsOf(region);
    for (const auto& [index, eg] : mods.eg) {
        const double pitch = get(eg, "pitch", 0.0);
        if (pitch != 0.0) {
            zone.modulation = envelopeOf(eg);
            zone.modToPitch = static_cast<float>(pitch);
            break;
        }
    }
    for (const auto& [index, lfo] : mods.lfo) {
        const double pitch = get(lfo, "pitch", 0.0);
        const double amplitude = get(lfo, "amplitude", 100.0);
        if (pitch != 0.0) {
            zone.vibrato.hz = static_cast<float>(get(lfo, "freq", 8.176));
            zone.vibrato.delay = static_cast<float>(get(lfo, "delay", 0.0));
            zone.vibrato.toPitch = static_cast<float>(pitch);
        }
        if (amplitude != 100.0 && amplitude > 0.0) {
            zone.modLfo.hz = static_cast<float>(get(lfo, "freq", 8.176));
            zone.modLfo.delay = static_cast<float>(get(lfo, "delay", 0.0));
            zone.modLfo.toVolume = static_cast<float>(20.0 * std::log10(amplitude / 100.0));
        }
    }
}

class Loader {
public:
    explicit Loader(Bank& bank) : m_bank(bank) {}

    void program(const fs::path& file, std::uint16_t bankNumber, std::uint8_t number) {
        sfz::Import import;
        if (!sfz::loadSfz(file, import)) {
            m_bank.notes.push_back(
                BankNote{.what = file.filename().string(), .detail = "unreadable"});
            return;
        }
        for (const sfz::Note& note : import.notes) {
            if (note.kind == sfz::NoteKind::Unknown) {
                m_bank.notes.push_back(BankNote{.what = file.filename().string() + ":" +
                                                        std::to_string(note.line) + " " +
                                                        note.opcode,
                                                .detail = note.detail});
            }
        }
        BankPreset preset;
        preset.bank = bankNumber;
        preset.program = number;
        preset.name = file.stem().string();
        if (const std::size_t cut = preset.name.find('_'); cut != std::string::npos) {
            preset.name = preset.name.substr(cut + 1);
        }
        for (const sfz::Region& region : import.regions) {
            const int sample = sampleIndex(file.parent_path() / region.sample);
            if (sample < 0) {
                continue;
            }
            preset.zones.push_back(zoneOf(region, static_cast<std::uint32_t>(sample)));
        }
        m_bank.presets.push_back(std::move(preset));
    }

private:
    int sampleIndex(const fs::path& path) {
        const std::string key = path.lexically_normal().generic_string();
        if (const auto found = m_samples.find(key); found != m_samples.end()) {
            return found->second;
        }
        SampleBuffer buffer;
        std::string error;
        int index = -1;
        if (decodeFile(path, buffer, error)) {
            BankSample sample;
            sample.name = path.filename().string();
            sample.rate = buffer.sampleRate;
            const auto frames = static_cast<std::size_t>(buffer.frames);
            sample.data.resize(frames);
            for (std::size_t i = 0; i < frames; ++i) {
                const float l = buffer.left[kSampleGuardFrames + i];
                sample.data[i] =
                    buffer.right.empty() ? l : 0.5F * (l + buffer.right[kSampleGuardFrames + i]);
            }
            index = static_cast<int>(m_bank.samples.size());
            m_bank.samples.push_back(std::move(sample));
        } else {
            m_bank.notes.push_back(BankNote{.what = path.filename().string(), .detail = error});
        }
        m_samples.emplace(key, index);
        return index;
    }

    [[nodiscard]] BankZone zoneOf(const sfz::Region& region, std::uint32_t sample) const {
        const project::SampleZone& in = region.zone;
        const auto frames = static_cast<std::uint32_t>(m_bank.samples[sample].data.size());
        BankZone zone;
        zone.sample = sample;
        zone.keyLow = in.keyLow;
        zone.keyHigh = in.keyHigh;
        zone.velocityLow = in.velocityLow <= 1 ? 0 : in.velocityLow;
        zone.velocityHigh = in.velocityHigh;
        zone.rootKey = in.rootKey;
        zone.tune = in.tuneCents;
        zone.attenuation = -in.gainDb;
        zone.pan = in.pan;
        zone.start = std::min(in.start, frames);
        zone.end = in.end == 0 ? frames : std::min(in.end, frames);
        zone.loopStart = std::min(in.loopStart, frames);
        zone.loopEnd = std::min(in.loopEnd, frames);
        if (in.loop == project::LoopMode::Forward || in.loop == project::LoopMode::PingPong) {
            zone.loop = BankLoop::Continuous;
        } else if (in.loop == project::LoopMode::Sustain) {
            zone.loop = BankLoop::Sustain;
        }
        if (in.offBy != 0 && in.offBy == in.group) {
            zone.exclusiveClass = in.group;
        }
        zone.volume.attack = std::max(0.0F, region.attack);
        zone.volume.decay = std::max(0.0F, region.decay);
        zone.volume.sustain = region.sustain < 0.0F ? 1.0F : region.sustain;
        zone.volume.release = std::max(0.0F, region.release);
        applyModulation(region, zone);
        return zone;
    }

    Bank& m_bank;
    std::map<std::string, int> m_samples;
};

} // namespace

bool loadSfzFolder(const std::filesystem::path& folder, Bank& out, std::string& error) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        error = folder.string() + " is not a folder";
        return false;
    }
    out = Bank{};
    out.name = folder.filename().string();
    Loader loader(out);
    std::vector<fs::path> banks;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, ec)) {
        if (entry.is_directory() && leadingNumber(entry.path().filename().string()) >= 0) {
            banks.push_back(entry.path());
        }
    }
    std::ranges::sort(banks);
    for (const fs::path& bankFolder : banks) {
        const int bankNumber = leadingNumber(bankFolder.filename().string());
        std::vector<fs::path> files;
        for (const fs::directory_entry& entry : fs::directory_iterator(bankFolder, ec)) {
            if (entry.path().extension() == ".sfz" &&
                leadingNumber(entry.path().filename().string()) >= 0) {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);
        for (const fs::path& file : files) {
            const int program = leadingNumber(file.filename().string());
            if (program > 127 || bankNumber > 128) {
                continue;
            }
            loader.program(file, static_cast<std::uint16_t>(bankNumber),
                           static_cast<std::uint8_t>(program));
        }
    }
    if (out.presets.empty()) {
        error = folder.string() + " has no NNN/PPP_Name.sfz programs";
        return false;
    }
    return true;
}

} // namespace adx::format::soundfont
