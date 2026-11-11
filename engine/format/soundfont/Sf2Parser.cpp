// adx-thread: main
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <vector>

#include "engine/dsp/Math.h"
#include "engine/format/soundfont/Generators.h"
#include "engine/format/soundfont/Riff.h"
#include "engine/format/soundfont/SoundFont.h"

namespace adx::format::soundfont {
namespace {

constexpr std::size_t kPhdr = 38;
constexpr std::size_t kBag = 4;
constexpr std::size_t kMod = 10;
constexpr std::size_t kGen = 4;
constexpr std::size_t kInst = 22;
constexpr std::size_t kShdr = 46;

struct Generator {
    std::uint16_t oper{0};
    std::int16_t amount{0};
};

struct Bag {
    std::uint16_t gen{0};
    std::uint16_t mod{0};
};

struct SampleHeader {
    std::string name;
    std::uint32_t start{0};
    std::uint32_t end{0};
    std::uint32_t loopStart{0};
    std::uint32_t loopEnd{0};
    std::uint32_t rate{44100};
    std::uint8_t pitch{60};
    std::int8_t correction{0};
    std::uint16_t type{1};
};

/// One zone's generators: set or not, and the key and velocity ranges kept apart since
/// they intersect rather than replace.
struct GenSet {
    std::array<std::optional<std::int16_t>, kGenCount> value{};
    std::uint8_t keyLow{0};
    std::uint8_t keyHigh{127};
    std::uint8_t velLow{0};
    std::uint8_t velHigh{127};

    void set(const Generator& g) {
        if (g.oper == static_cast<std::uint16_t>(Gen::KeyRange)) {
            keyLow = static_cast<std::uint8_t>(static_cast<std::uint16_t>(g.amount) & 0xFFU);
            keyHigh = static_cast<std::uint8_t>(static_cast<std::uint16_t>(g.amount) >> 8U);
        } else if (g.oper == static_cast<std::uint16_t>(Gen::VelRange)) {
            velLow = static_cast<std::uint8_t>(static_cast<std::uint16_t>(g.amount) & 0xFFU);
            velHigh = static_cast<std::uint8_t>(static_cast<std::uint16_t>(g.amount) >> 8U);
        } else if (g.oper < kGenCount) {
            value.at(g.oper) = g.amount;
        }
    }
    [[nodiscard]] bool has(Gen g) const {
        return value.at(static_cast<std::size_t>(g)).has_value();
    }
    [[nodiscard]] int get(Gen g) const {
        const auto& v = value.at(static_cast<std::size_t>(g));
        return v ? *v : generatorDefault(static_cast<std::uint16_t>(g));
    }
};

template<class T, class Read>
std::vector<T> records(const Chunk* chunk, std::size_t size, Read read) {
    std::vector<T> out;
    if (chunk == nullptr) {
        return out;
    }
    const std::size_t count = chunk->body.size() / size;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(read(chunk->body, i * size));
    }
    return out;
}

Bag readBag(std::span<const std::byte> b, std::size_t at) {
    Bag bag;
    static_cast<void>(readU16(b, at, bag.gen));
    static_cast<void>(readU16(b, at + 2, bag.mod));
    return bag;
}

Generator readGen(std::span<const std::byte> b, std::size_t at) {
    Generator g;
    static_cast<void>(readU16(b, at, g.oper));
    static_cast<void>(readI16(b, at + 2, g.amount));
    return g;
}

/// A modulator the default set already covers is not news; anything else is reported.
bool isDefaultModulator(std::span<const std::byte> b, std::size_t at) {
    std::uint16_t source = 0;
    std::uint16_t dest = 0;
    static_cast<void>(readU16(b, at, source));
    static_cast<void>(readU16(b, at + 2, dest));
    const std::uint16_t index = source & 0x7FU;
    const bool cc = (source & 0x80U) != 0;
    // velocity (2) -> attenuation / filter; CC1 -> vibrato; CC7, 10, 11; pitch wheel (14).
    if (!cc && index == 2 &&
        (dest == static_cast<std::uint16_t>(Gen::InitialAttenuation) ||
         dest == static_cast<std::uint16_t>(Gen::InitialFilterFc))) {
        return true;
    }
    if (cc &&
        (index == 1 || index == 7 || index == 10 || index == 11 || index == 91 || index == 93)) {
        return true;
    }
    return !cc && (index == 14 || index == 13 || index == 0);
}

float envSeconds(const GenSet& g, Gen gen) {
    return static_cast<float>(timecentsToSeconds(g.get(gen)));
}

BankZone zoneOf(const GenSet& g, const SampleHeader& s, std::uint32_t sampleIndex,
                std::size_t frames) {
    BankZone z;
    z.sample = sampleIndex;
    z.keyLow = g.keyLow;
    z.keyHigh = g.keyHigh;
    z.velocityLow = g.velLow;
    z.velocityHigh = g.velHigh;
    const int root = g.get(Gen::OverridingRootKey);
    z.rootKey = static_cast<std::uint8_t>(std::clamp(root >= 0 ? root : s.pitch, 0, 127));
    z.tune =
        static_cast<float>((g.get(Gen::CoarseTune) * 100) + g.get(Gen::FineTune) + s.correction);
    z.scaleTuning = static_cast<float>(g.get(Gen::ScaleTuning));
    z.attenuation =
        static_cast<float>(std::max(0.0, centibelsToDb(g.get(Gen::InitialAttenuation))));
    z.pan = std::clamp(static_cast<float>(g.get(Gen::Pan)) / 500.0F, -1.0F, 1.0F);
    const auto offset = [&](Gen fine, Gen coarse) {
        return static_cast<std::int64_t>(g.get(fine)) +
               (32768 * static_cast<std::int64_t>(g.get(coarse)));
    };
    const auto clampFrame = [frames](std::int64_t v) {
        return static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(v, 0, static_cast<std::int64_t>(frames)));
    };
    z.start = clampFrame(offset(Gen::StartAddrsOffset, Gen::StartAddrsCoarseOffset));
    z.end = clampFrame(static_cast<std::int64_t>(frames) +
                       offset(Gen::EndAddrsOffset, Gen::EndAddrsCoarseOffset));
    z.loopStart = clampFrame(static_cast<std::int64_t>(s.loopStart) - s.start +
                             offset(Gen::StartloopAddrsOffset, Gen::StartloopAddrsCoarseOffset));
    z.loopEnd = clampFrame(static_cast<std::int64_t>(s.loopEnd) - s.start +
                           offset(Gen::EndloopAddrsOffset, Gen::EndloopAddrsCoarseOffset));
    const int modes = g.get(Gen::SampleModes) & 3;
    z.loop = BankLoop::None;
    if (modes == 1) {
        z.loop = BankLoop::Continuous;
    } else if (modes == 3) {
        z.loop = BankLoop::Sustain;
    }
    if (z.loopEnd <= z.loopStart + 1) {
        z.loop = BankLoop::None;
    }
    z.exclusiveClass = static_cast<std::uint16_t>(std::max(0, g.get(Gen::ExclusiveClass)));
    z.filterHz = static_cast<float>(
        std::clamp(absoluteCentsToHz(g.get(Gen::InitialFilterFc)), 20.0, 20000.0));
    z.filterQ = static_cast<float>(std::max(0.0, centibelsToDb(g.get(Gen::InitialFilterQ))));

    z.volume = BankEnvelope{.delay = envSeconds(g, Gen::DelayVolEnv),
                            .attack = envSeconds(g, Gen::AttackVolEnv),
                            .hold = envSeconds(g, Gen::HoldVolEnv),
                            .decay = envSeconds(g, Gen::DecayVolEnv),
                            .sustain = static_cast<float>(dsp::dbToGain(
                                -std::clamp(centibelsToDb(g.get(Gen::SustainVolEnv)), 0.0, 144.0))),
                            .release = envSeconds(g, Gen::ReleaseVolEnv),
                            .keyToHold = static_cast<float>(g.get(Gen::KeynumToVolEnvHold)),
                            .keyToDecay = static_cast<float>(g.get(Gen::KeynumToVolEnvDecay))};
    z.modulation = BankEnvelope{
        .delay = envSeconds(g, Gen::DelayModEnv),
        .attack = envSeconds(g, Gen::AttackModEnv),
        .hold = envSeconds(g, Gen::HoldModEnv),
        .decay = envSeconds(g, Gen::DecayModEnv),
        .sustain = std::clamp(1.0F - (static_cast<float>(g.get(Gen::SustainModEnv)) / 1000.0F),
                              0.0F, 1.0F),
        .release = envSeconds(g, Gen::ReleaseModEnv),
        .keyToHold = static_cast<float>(g.get(Gen::KeynumToModEnvHold)),
        .keyToDecay = static_cast<float>(g.get(Gen::KeynumToModEnvDecay))};
    z.modToPitch = static_cast<float>(g.get(Gen::ModEnvToPitch));
    z.modToFilter = static_cast<float>(g.get(Gen::ModEnvToFilterFc));
    z.vibrato = BankLfo{.delay = envSeconds(g, Gen::DelayVibLfo),
                        .hz = static_cast<float>(absoluteCentsToHz(g.get(Gen::FreqVibLfo))),
                        .toPitch = static_cast<float>(g.get(Gen::VibLfoToPitch)),
                        .toFilter = 0.0F,
                        .toVolume = 0.0F};
    z.modLfo = BankLfo{.delay = envSeconds(g, Gen::DelayModLfo),
                       .hz = static_cast<float>(absoluteCentsToHz(g.get(Gen::FreqModLfo))),
                       .toPitch = static_cast<float>(g.get(Gen::ModLfoToPitch)),
                       .toFilter = static_cast<float>(g.get(Gen::ModLfoToFilterFc)),
                       .toVolume = static_cast<float>(centibelsToDb(g.get(Gen::ModLfoToVolume)))};
    return z;
}

} // namespace

const BankPreset* Bank::find(std::uint16_t bank, std::uint8_t program) const noexcept {
    for (const BankPreset& p : presets) {
        if (p.bank == bank && p.program == program) {
            return &p;
        }
    }
    if (bank != kDrumBank) {
        for (const BankPreset& p : presets) {
            if (p.bank == 0 && p.program == program) {
                return &p;
            }
        }
    } else {
        // A missing kit: GM's standard kit, program 0 on the drum bank.
        for (const BankPreset& p : presets) {
            if (p.bank == kDrumBank) {
                return &p;
            }
        }
        return nullptr;
    }
    return presets.empty() ? nullptr : &presets.front();
}

// NOLINTNEXTLINE(readability-function-size) - the pdta hierarchy, top to bottom.
bool parseSf2(std::span<const std::byte> file, Bank& out, std::string& error) {
    RiffReader riff(file);
    const std::optional<Chunk> root = riff.root();
    if (!root || root->listType != "sfbk") {
        error = root ? "not a SoundFont (form '" + root->listType + "')" : riff.error();
        return false;
    }
    const std::vector<Chunk> top = riff.children(*root);
    const Chunk* info = findChunk(top, "LIST", "INFO");
    const Chunk* sdta = findChunk(top, "LIST", "sdta");
    const Chunk* pdta = findChunk(top, "LIST", "pdta");
    if (sdta == nullptr || pdta == nullptr) {
        error = !riff.error().empty() ? riff.error() : "a SoundFont needs sdta and pdta lists";
        return false;
    }
    if (info != nullptr) {
        const std::vector<Chunk> infos = riff.children(*info);
        if (const Chunk* name = findChunk(infos, "INAM")) {
            out.name = readName(name->body, 0, name->body.size());
        }
    }
    const std::vector<Chunk> samples = riff.children(*sdta);
    const Chunk* smpl = findChunk(samples, "smpl");
    const Chunk* sm24 = findChunk(samples, "sm24");
    const std::vector<Chunk> hydra = riff.children(*pdta);
    if (!riff.error().empty()) {
        error = riff.error();
        return false;
    }
    const auto chunk = [&hydra](std::string_view id) { return findChunk(hydra, id); };

    const auto phdr = records<std::tuple<std::string, std::uint16_t, std::uint16_t, std::uint16_t>>(
        chunk("phdr"), kPhdr, [](std::span<const std::byte> b, std::size_t at) {
            std::uint16_t program = 0;
            std::uint16_t bank = 0;
            std::uint16_t bag = 0;
            static_cast<void>(readU16(b, at + 20, program));
            static_cast<void>(readU16(b, at + 22, bank));
            static_cast<void>(readU16(b, at + 24, bag));
            return std::tuple{readName(b, at, 20), program, bank, bag};
        });
    const auto pbag = records<Bag>(chunk("pbag"), kBag, readBag);
    const auto pgen = records<Generator>(chunk("pgen"), kGen, readGen);
    const auto inst = records<std::pair<std::string, std::uint16_t>>(
        chunk("inst"), kInst, [](std::span<const std::byte> b, std::size_t at) {
            std::uint16_t bag = 0;
            static_cast<void>(readU16(b, at + 20, bag));
            return std::pair{readName(b, at, 20), bag};
        });
    const auto ibag = records<Bag>(chunk("ibag"), kBag, readBag);
    const auto igen = records<Generator>(chunk("igen"), kGen, readGen);
    const auto shdr = records<SampleHeader>(chunk("shdr"), kShdr,
                                            [](std::span<const std::byte> b, std::size_t at) {
                                                SampleHeader s;
                                                s.name = readName(b, at, 20);
                                                static_cast<void>(readU32(b, at + 20, s.start));
                                                static_cast<void>(readU32(b, at + 24, s.end));
                                                static_cast<void>(readU32(b, at + 28, s.loopStart));
                                                static_cast<void>(readU32(b, at + 32, s.loopEnd));
                                                static_cast<void>(readU32(b, at + 36, s.rate));
                                                s.pitch = static_cast<std::uint8_t>(b[at + 40]);
                                                s.correction = static_cast<std::int8_t>(b[at + 41]);
                                                static_cast<void>(readU16(b, at + 44, s.type));
                                                return s;
                                            });
    if (phdr.size() < 2 || inst.size() < 2 || shdr.size() < 2 || pbag.empty() || ibag.empty()) {
        error = "the SoundFont's preset, instrument or sample headers are missing";
        return false;
    }
    for (const char* modChunk : {"pmod", "imod"}) {
        if (const Chunk* mods = chunk(modChunk)) {
            std::size_t reported = 0;
            for (std::size_t at = 0; at + kMod <= mods->body.size(); at += kMod) {
                reported += isDefaultModulator(mods->body, at) ? 0 : 1;
            }
            // The last record is the terminal one.
            if (reported > 1) {
                out.notes.push_back(
                    BankNote{.what = modChunk,
                             .detail = std::to_string(reported - 1) +
                                       " modulator(s) outside SF2's default set are not applied"});
            }
        }
    }

    // The samples, one buffer each, as float.
    const std::size_t smplFrames = smpl != nullptr ? smpl->body.size() / 2 : 0;
    const bool has24 = sm24 != nullptr && sm24->body.size() >= smplFrames;
    std::vector<std::optional<std::uint32_t>> sampleIndex(shdr.size());
    for (std::size_t s = 0; s + 1 < shdr.size(); ++s) {
        const SampleHeader& h = shdr[s];
        if ((h.type & 0x8000U) != 0) {
            out.notes.push_back({.what = h.name, .detail = "a ROM sample: not in the file"});
            continue;
        }
        if ((h.type & 0x10U) != 0) {
            out.notes.push_back({.what = h.name, .detail = "an SF3 compressed sample: not read"});
            continue;
        }
        if (h.end <= h.start || h.end > smplFrames) {
            out.notes.push_back({.what = h.name, .detail = "its frames lie outside smpl"});
            continue;
        }
        BankSample sample;
        sample.name = h.name;
        sample.rate = std::clamp<std::uint32_t>(h.rate, 400, 400000);
        sample.data.resize(h.end - h.start);
        for (std::uint32_t i = h.start; i < h.end; ++i) {
            std::int16_t v = 0;
            static_cast<void>(readI16(smpl->body, static_cast<std::size_t>(i) * 2, v));
            float value = static_cast<float>(v) / 32768.0F;
            if (has24) {
                value += static_cast<float>(static_cast<std::uint8_t>(sm24->body[i])) /
                         (32768.0F * 256.0F);
            }
            sample.data[i - h.start] = value;
        }
        sampleIndex[s] = static_cast<std::uint32_t>(out.samples.size());
        out.samples.push_back(std::move(sample));
    }

    const auto genRange = [](const std::vector<Bag>& bags, const std::vector<Generator>& gens,
                             std::size_t bag) {
        const std::size_t from = bags[bag].gen;
        const std::size_t to = bag + 1 < bags.size() ? bags[bag + 1].gen : gens.size();
        return std::pair{std::min(from, gens.size()), std::min(std::max(from, to), gens.size())};
    };

    for (std::size_t p = 0; p + 1 < phdr.size(); ++p) {
        const auto& [name, program, bank, bagStart] = phdr[p];
        const std::uint16_t bagEnd = std::get<3>(phdr[p + 1]);
        BankPreset preset{.bank = bank,
                          .program = static_cast<std::uint8_t>(program & 0x7F),
                          .name = name,
                          .zones = {}};
        GenSet presetGlobal;
        for (std::size_t bag = bagStart; bag < bagEnd && bag < pbag.size(); ++bag) {
            GenSet local = presetGlobal;
            const auto [g0, g1] = genRange(pbag, pgen, bag);
            std::optional<std::uint16_t> instrument;
            GenSet own;
            for (std::size_t g = g0; g < g1; ++g) {
                if (pgen[g].oper == static_cast<std::uint16_t>(Gen::Instrument)) {
                    instrument = static_cast<std::uint16_t>(pgen[g].amount);
                } else {
                    own.set(pgen[g]);
                }
            }
            if (!instrument) {
                if (bag == bagStart) {
                    presetGlobal = own; // a global preset zone
                }
                continue;
            }
            for (std::size_t i = 0; i < kGenCount; ++i) {
                if (own.value.at(i)) {
                    local.value.at(i) = own.value.at(i);
                }
            }
            local.keyLow = std::max(presetGlobal.keyLow, own.keyLow);
            local.keyHigh = std::min(presetGlobal.keyHigh, own.keyHigh);
            local.velLow = std::max(presetGlobal.velLow, own.velLow);
            local.velHigh = std::min(presetGlobal.velHigh, own.velHigh);
            if (*instrument + 1 >= inst.size()) {
                continue;
            }
            const std::uint16_t iStart = inst[*instrument].second;
            const std::uint16_t iEnd = inst[*instrument + 1].second;
            GenSet instGlobal;
            for (std::size_t ib = iStart; ib < iEnd && ib < ibag.size(); ++ib) {
                const auto [h0, h1] = genRange(ibag, igen, ib);
                GenSet zone = instGlobal;
                std::optional<std::uint16_t> sampleId;
                for (std::size_t g = h0; g < h1; ++g) {
                    if (igen[g].oper == static_cast<std::uint16_t>(Gen::SampleId)) {
                        sampleId = static_cast<std::uint16_t>(igen[g].amount);
                    } else {
                        zone.set(igen[g]);
                    }
                }
                if (!sampleId) {
                    if (ib == iStart) {
                        instGlobal = zone;
                    }
                    continue;
                }
                if (*sampleId >= shdr.size() || !sampleIndex[*sampleId]) {
                    continue;
                }
                // Preset generators add to the instrument's; ranges intersect.
                for (std::size_t i = 0; i < kGenCount; ++i) {
                    if (local.value.at(i) && presetAdditive(static_cast<std::uint16_t>(i))) {
                        zone.value.at(i) = static_cast<std::int16_t>(std::clamp(
                            zone.get(static_cast<Gen>(i)) + *local.value.at(i), -32768, 32767));
                    }
                }
                zone.keyLow = std::max(zone.keyLow, local.keyLow);
                zone.keyHigh = std::min(zone.keyHigh, local.keyHigh);
                zone.velLow = std::max(zone.velLow, local.velLow);
                zone.velHigh = std::min(zone.velHigh, local.velHigh);
                if (zone.keyLow > zone.keyHigh || zone.velLow > zone.velHigh) {
                    continue;
                }
                const std::uint32_t index = *sampleIndex[*sampleId];
                preset.zones.push_back(
                    zoneOf(zone, shdr[*sampleId], index, out.samples[index].data.size()));
            }
        }
        out.presets.push_back(std::move(preset));
    }
    std::ranges::sort(out.presets, [](const BankPreset& a, const BankPreset& b) {
        return a.bank != b.bank ? a.bank < b.bank : a.program < b.program;
    });
    return true;
}

bool loadBank(const std::filesystem::path& path, Bank& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path.string();
        return false;
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(raw.data()),
                                           raw.size());
    if (raw.size() >= 12 && std::string_view(raw.data() + 8, 4) == "DLS ") {
        return parseDls(bytes, out, error);
    }
    return parseSf2(bytes, out, error);
}

std::filesystem::path systemGmBank() {
    char* root = nullptr;
    std::size_t size = 0;
#ifdef _WIN32
    if (_dupenv_s(&root, &size, "SystemRoot") != 0 || root == nullptr) {
        return {};
    }
    const std::filesystem::path path =
        std::filesystem::path(root) / "System32" / "drivers" / "gm.dls";
    std::free(root); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s allocates with malloc
#else
    static_cast<void>(root);
    static_cast<void>(size);
    std::filesystem::path path;
#endif
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) ? path : std::filesystem::path{};
}

std::string reportOf(const Bank& bank) {
    std::string out;
    for (const BankNote& note : bank.notes) {
        out += note.what + ": " + note.detail + "\n";
    }
    return out;
}

} // namespace adx::format::soundfont
