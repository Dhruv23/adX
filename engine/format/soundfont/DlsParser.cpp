// adx-thread: main
#include <algorithm>
#include <optional>
#include <vector>

#include "engine/dsp/Math.h"
#include "engine/format/soundfont/Generators.h"
#include "engine/format/soundfont/Riff.h"
#include "engine/format/soundfont/SoundFont.h"

namespace adx::format::soundfont {
namespace {

// DLS connection sources and destinations (MMA DLS Level 2, §2.10).
constexpr std::uint16_t kSrcNone = 0x0000;
constexpr std::uint16_t kSrcLfo = 0x0001;
constexpr std::uint16_t kSrcKeyNumber = 0x0003;
constexpr std::uint16_t kSrcEg2 = 0x0005;
constexpr std::uint16_t kSrcVibrato = 0x0009;

constexpr std::uint16_t kDstAttenuation = 0x0001;
constexpr std::uint16_t kDstPitch = 0x0003;
constexpr std::uint16_t kDstPan = 0x0004;
constexpr std::uint16_t kDstLfoFrequency = 0x0104;
constexpr std::uint16_t kDstLfoDelay = 0x0105;
constexpr std::uint16_t kDstVibFrequency = 0x0114;
constexpr std::uint16_t kDstVibDelay = 0x0115;
constexpr std::uint16_t kDstEg1Attack = 0x0206;
constexpr std::uint16_t kDstEg1Decay = 0x0207;
constexpr std::uint16_t kDstEg1Release = 0x0209;
constexpr std::uint16_t kDstEg1Sustain = 0x020A;
constexpr std::uint16_t kDstEg1Delay = 0x020B;
constexpr std::uint16_t kDstEg1Hold = 0x020C;
constexpr std::uint16_t kDstEg2Attack = 0x030A;
constexpr std::uint16_t kDstEg2Decay = 0x030B;
constexpr std::uint16_t kDstEg2Release = 0x030D;
constexpr std::uint16_t kDstEg2Sustain = 0x030E;
constexpr std::uint16_t kDstEg2Delay = 0x030F;
constexpr std::uint16_t kDstEg2Hold = 0x0310;
constexpr std::uint16_t kDstFilterCutoff = 0x0500;
constexpr std::uint16_t kDstFilterQ = 0x0501;

constexpr std::uint32_t kDrumFlag = 0x80000000U;

struct Wsmp {
    std::uint8_t unity{60};
    float fineTune{0.0F}; // cents
    float gainDb{0.0F};
    bool looped{false};
    bool releaseLoop{false};
    std::uint32_t loopStart{0};
    std::uint32_t loopLength{0};
};

std::optional<Wsmp> readWsmp(const Chunk* chunk) {
    if (chunk == nullptr) {
        return std::nullopt;
    }
    const auto b = chunk->body;
    std::uint32_t size = 0;
    std::uint16_t unity = 60;
    std::int16_t fine = 0;
    std::int32_t gain = 0;
    std::uint32_t loops = 0;
    if (!readU32(b, 0, size) || !readU16(b, 4, unity) || !readI16(b, 6, fine) ||
        !readI32(b, 8, gain) || !readU32(b, 16, loops)) {
        return std::nullopt;
    }
    Wsmp w;
    w.unity = static_cast<std::uint8_t>(std::min<std::uint16_t>(unity, 127));
    w.fineTune = static_cast<float>(fine) * 100.0F / 32768.0F; // relative pitch, 1/32768 st
    w.gainDb = static_cast<float>(gain) / 655360.0F;
    if (loops > 0) {
        std::uint32_t type = 0;
        const std::size_t at = size;
        if (readU32(b, at + 4, type) && readU32(b, at + 8, w.loopStart) &&
            readU32(b, at + 12, w.loopLength)) {
            w.looped = w.loopLength > 1;
            w.releaseLoop = type == 1;
        }
    }
    return w;
}

/// One articulation: the defaults a region starts from, set by connection blocks.
struct Articulation {
    BankZone shape;
    std::vector<std::string> unmapped;
};

double timecents(std::int32_t scale) {
    return timecentsToSeconds(static_cast<double>(scale) / 65536.0);
}

void applyConnections(const Chunk& art, Articulation& out) {
    const auto b = art.body;
    std::uint32_t header = 0;
    std::uint32_t count = 0;
    if (!readU32(b, 0, header) || !readU32(b, 4, count)) {
        return;
    }
    BankZone& z = out.shape;
    for (std::uint32_t c = 0; c < count; ++c) {
        const std::size_t at = header + (static_cast<std::size_t>(c) * 12);
        std::uint16_t source = 0;
        std::uint16_t control = 0;
        std::uint16_t dest = 0;
        std::int32_t scale = 0;
        if (!readU16(b, at, source) || !readU16(b, at + 2, control) || !readU16(b, at + 4, dest) ||
            !readI32(b, at + 8, scale)) {
            return;
        }
        const double value = static_cast<double>(scale) / 65536.0;
        bool mapped = control == 0;
        if (mapped && source == kSrcNone) {
            switch (dest) {
            case kDstEg1Attack:
                z.volume.attack = static_cast<float>(timecents(scale));
                break;
            case kDstEg1Decay:
                z.volume.decay = static_cast<float>(timecents(scale));
                break;
            case kDstEg1Release:
                z.volume.release = static_cast<float>(timecents(scale));
                break;
            case kDstEg1Delay:
                z.volume.delay = static_cast<float>(timecents(scale));
                break;
            case kDstEg1Hold:
                z.volume.hold = static_cast<float>(timecents(scale));
                break;
            case kDstEg1Sustain:
                z.volume.sustain = static_cast<float>(std::clamp(value / 1000.0, 0.0, 1.0));
                break;
            case kDstEg2Attack:
                z.modulation.attack = static_cast<float>(timecents(scale));
                break;
            case kDstEg2Decay:
                z.modulation.decay = static_cast<float>(timecents(scale));
                break;
            case kDstEg2Release:
                z.modulation.release = static_cast<float>(timecents(scale));
                break;
            case kDstEg2Delay:
                z.modulation.delay = static_cast<float>(timecents(scale));
                break;
            case kDstEg2Hold:
                z.modulation.hold = static_cast<float>(timecents(scale));
                break;
            case kDstEg2Sustain:
                z.modulation.sustain = static_cast<float>(std::clamp(value / 1000.0, 0.0, 1.0));
                break;
            case kDstLfoFrequency:
                // DLS absolute pitch: 6900 cents is 440 Hz.
                z.modLfo.hz = static_cast<float>(440.0 * dsp::exp2((value - 6900.0) / 1200.0));
                break;
            case kDstLfoDelay:
                z.modLfo.delay = static_cast<float>(timecents(scale));
                break;
            case kDstVibFrequency:
                z.vibrato.hz = static_cast<float>(440.0 * dsp::exp2((value - 6900.0) / 1200.0));
                break;
            case kDstVibDelay:
                z.vibrato.delay = static_cast<float>(timecents(scale));
                break;
            case kDstFilterCutoff:
                z.filterHz = static_cast<float>(
                    std::clamp(440.0 * dsp::exp2((value - 6900.0) / 1200.0), 20.0, 20000.0));
                break;
            case kDstFilterQ:
                z.filterQ = static_cast<float>(std::max(0.0, value / 10.0));
                break;
            case kDstPan:
                z.pan = static_cast<float>(std::clamp(value / 500.0, -1.0, 1.0));
                break;
            case kDstAttenuation:
                z.attenuation = static_cast<float>(std::max(0.0, -value / 10.0));
                break;
            case kDstPitch:
                z.tune += static_cast<float>(value);
                break;
            default:
                mapped = false;
                break;
            }
        } else if (mapped && source == kSrcLfo && dest == kDstPitch) {
            z.modLfo.toPitch = static_cast<float>(value);
        } else if (mapped && source == kSrcLfo && dest == kDstAttenuation) {
            z.modLfo.toVolume = static_cast<float>(value / 10.0);
        } else if (mapped && source == kSrcLfo && dest == kDstFilterCutoff) {
            z.modLfo.toFilter = static_cast<float>(value);
        } else if (mapped && source == kSrcVibrato && dest == kDstPitch) {
            z.vibrato.toPitch = static_cast<float>(value);
        } else if (mapped && source == kSrcEg2 && dest == kDstPitch) {
            z.modToPitch = static_cast<float>(value);
        } else if (mapped && source == kSrcEg2 && dest == kDstFilterCutoff) {
            z.modToFilter = static_cast<float>(value);
        } else if (mapped && source == kSrcKeyNumber &&
                   (dest == kDstEg1Decay || dest == kDstEg1Hold)) {
            // The scale is over the whole keyboard (128 keys); SF2's is per key.
            (dest == kDstEg1Decay ? z.volume.keyToDecay : z.volume.keyToHold) =
                static_cast<float>(-value / 128.0);
        } else {
            mapped = false;
        }
        if (!mapped) {
            out.unmapped.push_back(std::to_string(source) + "->" + std::to_string(dest));
        }
    }
}

Articulation articulationOf(RiffReader& riff, const std::vector<Chunk>& chunks,
                            const Articulation& base) {
    Articulation out = base;
    for (const Chunk& chunk : chunks) {
        if (chunk.id == "LIST" && (chunk.listType == "lart" || chunk.listType == "lar2")) {
            for (const Chunk& art : riff.children(chunk)) {
                if (art.id == "art1" || art.id == "art2") {
                    applyConnections(art, out);
                }
            }
        }
    }
    return out;
}

struct Wave {
    std::optional<std::uint32_t> sample;
    std::optional<Wsmp> wsmp;
};

} // namespace

// NOLINTNEXTLINE(readability-function-size) - lins, then wvpl, then the regions.
bool parseDls(std::span<const std::byte> file, Bank& out, std::string& error) {
    RiffReader riff(file);
    const std::optional<Chunk> root = riff.root();
    if (!root || root->listType != "DLS ") {
        error = root ? "not a DLS file (form '" + root->listType + "')" : riff.error();
        return false;
    }
    const std::vector<Chunk> top = riff.children(*root);
    const Chunk* lins = findChunk(top, "LIST", "lins");
    const Chunk* wvpl = findChunk(top, "LIST", "wvpl");
    const Chunk* ptbl = findChunk(top, "ptbl");
    if (lins == nullptr || wvpl == nullptr) {
        error = !riff.error().empty() ? riff.error() : "a DLS file needs lins and wvpl";
        return false;
    }
    if (const Chunk* info = findChunk(top, "LIST", "INFO")) {
        const std::vector<Chunk> infos = riff.children(*info);
        if (const Chunk* name = findChunk(infos, "INAM")) {
            out.name = readName(name->body, 0, name->body.size());
        }
    }

    // The wave pool, in file order; ptbl maps a cue to its offset in wvpl.
    std::vector<Wave> waves;
    std::vector<std::size_t> waveOffsets;
    for (const Chunk& wave : riff.children(*wvpl)) {
        if (wave.id != "LIST" || wave.listType != "wave") {
            continue;
        }
        waveOffsets.push_back(wave.offset - (wvpl->offset + 12));
        const std::vector<Chunk> parts = riff.children(wave);
        const Chunk* fmt = findChunk(parts, "fmt ");
        const Chunk* data = findChunk(parts, "data");
        Wave entry;
        entry.wsmp = readWsmp(findChunk(parts, "wsmp"));
        std::uint16_t format = 0;
        std::uint16_t channels = 1;
        std::uint32_t rate = 22050;
        std::uint16_t bits = 16;
        if (fmt == nullptr || data == nullptr || !readU16(fmt->body, 0, format) ||
            !readU16(fmt->body, 2, channels) || !readU32(fmt->body, 4, rate) ||
            !readU16(fmt->body, 14, bits) || format != 1 || channels == 0 ||
            (bits != 8 && bits != 16)) {
            out.notes.push_back({.what = "wave " + std::to_string(waves.size()),
                                 .detail = "not 8- or 16-bit PCM: not read"});
            waves.push_back(entry);
            continue;
        }
        BankSample sample;
        sample.name = "wave " + std::to_string(waves.size());
        sample.rate = std::clamp<std::uint32_t>(rate, 400, 400000);
        const std::size_t frameBytes = static_cast<std::size_t>(bits / 8) * channels;
        const std::size_t frames = data->body.size() / frameBytes;
        sample.data.resize(frames);
        for (std::size_t f = 0; f < frames; ++f) {
            const std::size_t at = f * frameBytes;
            if (bits == 8) {
                sample.data[f] =
                    (static_cast<float>(static_cast<std::uint8_t>(data->body[at])) - 128.0F) /
                    128.0F;
            } else {
                std::int16_t v = 0;
                static_cast<void>(readI16(data->body, at, v));
                sample.data[f] = static_cast<float>(v) / 32768.0F;
            }
        }
        entry.sample = static_cast<std::uint32_t>(out.samples.size());
        out.samples.push_back(std::move(sample));
        waves.push_back(entry);
    }
    // ptbl: cue index -> wave. Without one, the cue is the wave's index.
    std::vector<std::size_t> cueToWave;
    if (ptbl != nullptr) {
        std::uint32_t size = 0;
        std::uint32_t cues = 0;
        if (readU32(ptbl->body, 0, size) && readU32(ptbl->body, 4, cues)) {
            for (std::uint32_t c = 0; c < cues; ++c) {
                std::uint32_t offset = 0;
                if (!readU32(ptbl->body, size + (static_cast<std::size_t>(c) * 4), offset)) {
                    break;
                }
                const auto match = std::ranges::find(waveOffsets, offset);
                cueToWave.push_back(match == waveOffsets.end()
                                        ? waves.size()
                                        : static_cast<std::size_t>(match - waveOffsets.begin()));
            }
        }
    }

    std::size_t unmapped = 0;
    for (const Chunk& ins : riff.children(*lins)) {
        if (ins.id != "LIST" || ins.listType != "ins ") {
            continue;
        }
        const std::vector<Chunk> parts = riff.children(ins);
        const Chunk* insh = findChunk(parts, "insh");
        std::uint32_t bankWord = 0;
        std::uint32_t program = 0;
        if (insh == nullptr || !readU32(insh->body, 4, bankWord) ||
            !readU32(insh->body, 8, program)) {
            continue;
        }
        BankPreset preset;
        const bool drums = (bankWord & kDrumFlag) != 0;
        preset.bank = drums ? Bank::kDrumBank
                            : static_cast<std::uint16_t>(((bankWord >> 8U) & 0x7FU) * 128U +
                                                         (bankWord & 0x7FU));
        preset.program = static_cast<std::uint8_t>(program & 0x7FU);
        if (const Chunk* info = findChunk(parts, "LIST", "INFO")) {
            const std::vector<Chunk> infos = riff.children(*info);
            if (const Chunk* name = findChunk(infos, "INAM")) {
                preset.name = readName(name->body, 0, name->body.size());
            }
        }
        Articulation global;
        global = articulationOf(riff, parts, global);
        const Chunk* lrgn = findChunk(parts, "LIST", "lrgn");
        if (lrgn == nullptr) {
            continue;
        }
        for (const Chunk& rgn : riff.children(*lrgn)) {
            if (rgn.id != "LIST" || (rgn.listType != "rgn " && rgn.listType != "rgn2")) {
                continue;
            }
            const std::vector<Chunk> rparts = riff.children(rgn);
            const Chunk* rgnh = findChunk(rparts, "rgnh");
            const Chunk* wlnk = findChunk(rparts, "wlnk");
            std::uint16_t keyLow = 0;
            std::uint16_t keyHigh = 127;
            std::uint16_t velLow = 0;
            std::uint16_t velHigh = 127;
            std::uint16_t keyGroup = 0;
            std::uint32_t cue = 0;
            if (rgnh == nullptr || wlnk == nullptr || !readU16(rgnh->body, 0, keyLow) ||
                !readU16(rgnh->body, 2, keyHigh) || !readU16(rgnh->body, 4, velLow) ||
                !readU16(rgnh->body, 6, velHigh) || !readU32(wlnk->body, 8, cue)) {
                continue;
            }
            static_cast<void>(readU16(rgnh->body, 10, keyGroup));
            const std::size_t waveIndex = cue < cueToWave.size() ? cueToWave[cue] : cue;
            if (waveIndex >= waves.size() || !waves[waveIndex].sample) {
                continue;
            }
            const Articulation art = articulationOf(riff, rparts, global);
            unmapped += art.unmapped.size();
            BankZone zone = art.shape;
            zone.sample = *waves[waveIndex].sample;
            zone.keyLow = static_cast<std::uint8_t>(std::min<std::uint16_t>(keyLow, 127));
            zone.keyHigh = static_cast<std::uint8_t>(std::min<std::uint16_t>(keyHigh, 127));
            zone.velocityLow = static_cast<std::uint8_t>(std::min<std::uint16_t>(velLow, 127));
            zone.velocityHigh = static_cast<std::uint8_t>(std::min<std::uint16_t>(velHigh, 127));
            zone.exclusiveClass = keyGroup;
            std::optional<Wsmp> wsmp = readWsmp(findChunk(rparts, "wsmp"));
            if (!wsmp) {
                wsmp = waves[waveIndex].wsmp;
            }
            const std::size_t frames = out.samples[zone.sample].data.size();
            zone.end = static_cast<std::uint32_t>(frames);
            if (wsmp) {
                zone.rootKey = wsmp->unity;
                zone.tune += wsmp->fineTune;
                zone.attenuation = std::max(0.0F, zone.attenuation - wsmp->gainDb);
                if (wsmp->looped && wsmp->loopStart + wsmp->loopLength <= frames) {
                    zone.loopStart = wsmp->loopStart;
                    zone.loopEnd = wsmp->loopStart + wsmp->loopLength;
                    zone.loop = wsmp->releaseLoop ? BankLoop::Sustain : BankLoop::Continuous;
                }
            }
            preset.zones.push_back(zone);
        }
        out.presets.push_back(std::move(preset));
    }
    if (unmapped > 0) {
        out.notes.push_back({.what = "art1/art2",
                             .detail = std::to_string(unmapped) +
                                       " connection block(s) adX does not map are not applied"});
    }
    std::ranges::sort(out.presets, [](const BankPreset& a, const BankPreset& b) {
        return a.bank != b.bank ? a.bank < b.bank : a.program < b.program;
    });
    return true;
}

} // namespace adx::format::soundfont
