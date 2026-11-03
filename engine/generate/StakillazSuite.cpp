// adx-thread: main
#include "engine/generate/StakillazSuite.h"

#include <array>
#include <random>

#include "engine/generate/ProjectBuilder.h"
#include "engine/generate/RiffGenerator.h"

namespace adx::generate {

namespace {

using analysis::Key;
using analysis::Mode;

constexpr std::array<Key, 4> kKeys{{{.tonic = 5, .mode = Mode::Minor},
                                    {.tonic = 7, .mode = Mode::Minor},
                                    {.tonic = 9, .mode = Mode::Minor},
                                    {.tonic = 1, .mode = Mode::Minor}}};

const std::array<std::vector<int>, 3> kIdioms{{{1, 1, 6, 7}, {1, 6, 4, 5}, {1, 7, 6, 7}}};

struct Section {
    const char* name;
    std::int64_t bars;
};

} // namespace

GeneratedTrack stakillazTrack(const preset::PresetLibrary& presets, const StyleOptions& options,
                              bool withNotes) {
    std::mt19937 random(options.seed);
    GeneratedTrack out;
    out.bpm = options.bpm.value_or(chance(random, 0.7) ? 150.0 : 130.0);
    out.key = options.key.value_or(
        kKeys.at(static_cast<std::size_t>(roll(random, 0, static_cast<int>(kKeys.size()) - 1))));
    const Key key = out.key;

    ProjectBuilder b(presets, "stakillaz", out.bpm,
                     "STAKILLAZ style - seed " + std::to_string(options.seed));
    b.effect(b.master(), "Blown Master");
    b.effect(b.master(), "Memphis Tape");
    const core::InsertId kickBus = b.insert("Kick");
    const core::InsertId lowBus = b.insert("Low", {}, 0.85F);
    b.effect(lowBus, "Kick Pump", kickBus);
    const core::InsertId leadBus = b.insert("Lead", {}, 0.7F);
    b.effect(leadBus, "Kick Pump", kickBus);
    const core::ChannelId kick = b.channel("Kick", "Hardstyle Kick", kickBus, 0.9F, 0.0F, 4);
    const core::ChannelId sub = b.channel("Sub", "808 Sub", lowBus, 0.8F, 0.0F, 4);
    const core::ChannelId bass = b.channel("Bass", "Phonk Bass", lowBus, 0.55F, 0.0F, 8);
    const core::ChannelId lead = b.channel("Lead", "Screech Lead", leadBus, 0.45F, 0.0F, 8);

    const std::int64_t phrases =
        std::max<std::int64_t>(6, (static_cast<std::int64_t>(options.bars) + 3) / 4);
    const std::int64_t drops = std::max<std::int64_t>(1, (phrases - 4) / 2);
    const std::vector<Section> form{{"Intro", 4},
                                    {"Build", 4},
                                    {"Drop", 4 * drops},
                                    {"Break", 4},
                                    {"Drop 2", 4 * std::max<std::int64_t>(1, phrases - 4 - drops)}};

    const std::vector<int>& degrees =
        kIdioms.at(static_cast<std::size_t>(roll(random, 0, static_cast<int>(kIdioms.size()) - 1)));
    const std::vector<Chord> chords = progression(key, degrees, 60);
    // The kick's tail is pitched: it follows each bar's root, low.
    const int low = 36 + key.tonic - (key.tonic > 4 ? 12 : 0);

    MelodySpec hook;
    hook.key = key;
    hook.tonicPitch = 72 + key.tonic - (key.tonic > 6 ? 12 : 0);
    hook.lowest = hook.tonicPitch - 5;
    hook.highest = hook.tonicPitch + 14;
    hook.length = 2 * kBar;
    hook.step = core::kPpq / 4;
    hook.density = 0.55;
    hook.leap = 0.3;
    hook.velocity = 100;
    hook.chords = &chords;
    hook.barTicks = kBar;
    // One two-bar hook, repeated: a phonk lead is a riff, not a solo.
    const std::vector<GenNote> riff = melody(hook, random);

    std::int64_t bar = 0;
    for (const Section& section : form) {
        b.marker(bar, section.name);
        if (!withNotes) {
            bar += section.bars;
            continue;
        }
        const std::string name = section.name;
        const bool drop = name.starts_with("Drop");
        std::vector<GenNote> kicks;
        std::vector<GenNote> subs;
        std::vector<GenNote> rolls;
        std::vector<GenNote> leads;
        for (std::int64_t i = 0; i < section.bars; ++i) {
            const Chord& chord = chords[static_cast<std::size_t>(i) % chords.size()];
            const int root = scalePitch(key, low, chord.degree - 1);
            const std::int64_t at = i * kBar;
            if (drop || name == "Build") {
                const char* pattern = drop ? "X...x...x...x..." : "X.......x.......";
                for (GenNote n :
                     pulse(pattern, 1, kBar, static_cast<std::uint8_t>(root + 12), 110, 127, 0.9)) {
                    n.start += at;
                    kicks.push_back(n);
                }
            }
            if (name != "Break") {
                for (GenNote n : pulse("..x...x...x...x.", 1, kBar, static_cast<std::uint8_t>(root),
                                       100, 100, 1.6)) {
                    n.start += at;
                    subs.push_back(n);
                }
            }
            if (drop) {
                for (GenNote n : pulse(".xx..xx..xx..xx.", 1, kBar,
                                       static_cast<std::uint8_t>(root + 12), 90, 90, 0.7)) {
                    n.start += at;
                    rolls.push_back(n);
                }
            }
            if (name != "Build") {
                for (const GenNote& n : riff) {
                    if (n.start >= (i % 2) * kBar && n.start < ((i % 2) + 1) * kBar) {
                        GenNote placed = n;
                        placed.start = at + (n.start - ((i % 2) * kBar));
                        leads.push_back(placed);
                    }
                }
            }
        }
        std::vector<std::pair<core::ChannelId, std::vector<GenNote>>> parts;
        for (auto& [channel, notes] : {std::pair{kick, kicks}, std::pair{sub, subs},
                                       std::pair{bass, rolls}, std::pair{lead, leads}}) {
            if (!notes.empty()) {
                parts.emplace_back(channel, notes);
            }
        }
        const core::PatternId pattern = b.pattern(name, section.bars, parts);
        b.place("Music", pattern, bar);
        bar += section.bars;
    }
    b.effectType(b.master(), "Limiter");
    out.notes = std::move(b.notes());
    out.notes.insert(out.notes.begin(), "form: intro 4, build 4, drop, break 4, drop 2 (" +
                                            std::to_string(bar) + " bars)");
    out.project = std::move(b).take();
    return out;
}

} // namespace adx::generate
