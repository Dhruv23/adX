// adx-thread: main
#include "engine/generate/C418Suite.h"

#include <array>
#include <random>

#include "engine/generate/ProjectBuilder.h"
#include "engine/generate/RiffGenerator.h"

namespace adx::generate {

namespace {

using analysis::Key;
using analysis::Mode;

constexpr std::array<Key, 5> kKeys{{{.tonic = 0, .mode = Mode::Major},
                                    {.tonic = 7, .mode = Mode::Major},
                                    {.tonic = 2, .mode = Mode::Major},
                                    {.tonic = 9, .mode = Mode::Minor},
                                    {.tonic = 4, .mode = Mode::Minor}}};

const std::array<std::vector<int>, 4> kMajorIdioms{
    {{1, 5, 6, 4}, {1, 4, 6, 5}, {4, 1, 5, 6}, {1, 6, 4, 5}}};
const std::array<std::vector<int>, 3> kMinorIdioms{{{1, 6, 3, 7}, {1, 4, 6, 5}, {6, 7, 1, 1}}};

struct Section {
    const char* name;
    std::int64_t bars;
};

} // namespace

GeneratedTrack c418Track(const preset::PresetLibrary& presets, const StyleOptions& options,
                         bool withNotes) {
    std::mt19937 random(options.seed);
    GeneratedTrack out;
    out.bpm = options.bpm.value_or(84.0 + roll(random, 0, 8));
    out.key = options.key.value_or(
        kKeys.at(static_cast<std::size_t>(roll(random, 0, static_cast<int>(kKeys.size()) - 1))));
    const Key key = out.key;

    ProjectBuilder b(presets, "c418", out.bpm, "C418 style - seed " + std::to_string(options.seed));
    const core::InsertId hall = b.insert("Hall");
    b.effect(hall, "Long Hall");
    const core::InsertId padBus = b.insert("Pad", {}, 0.8F);
    const core::InsertId malletBus = b.insert("Mallets", {}, 0.9F);
    b.effect(malletBus, "Ping-Pong Echo");
    const core::InsertId pianoBus = b.insert("Piano");
    const core::InsertId bassBus = b.insert("Bass", {}, 0.85F);
    b.send(padBus, hall, 0.35F);
    b.send(malletBus, hall, 0.45F);
    b.send(pianoBus, hall, 0.4F);
    const core::ChannelId pad = b.channel("Pad", "Warm Pad", padBus, 0.55F, 0.0F, 12);
    const core::ChannelId kalimba = b.channel("Kalimba", "Kalimba", malletBus, 0.6F, -0.15F, 16);
    const core::ChannelId piano = b.channel("Piano", "Giant Piano", pianoBus, 0.6F, 0.1F, 16);
    const core::ChannelId bass = b.channel("Bass", "Pizzicato", bassBus, 0.7F, 0.0F, 8);

    // The form, scaled to the bars asked for, in four-bar phrases.
    const std::int64_t phrases =
        std::max<std::int64_t>(4, (static_cast<std::int64_t>(options.bars) + 3) / 4);
    const std::int64_t develop = std::max<std::int64_t>(1, (phrases - 3) / 2);
    const std::vector<Section> form{{"Intro", 4},
                                    {"Theme", 4 * std::max<std::int64_t>(1, phrases - 3 - develop)},
                                    {"Development", 4 * develop},
                                    {"Outro", 4}};

    const auto& idioms =
        key.mode == Mode::Major
            ? std::vector<std::vector<int>>(kMajorIdioms.begin(), kMajorIdioms.end())
            : std::vector<std::vector<int>>(kMinorIdioms.begin(), kMinorIdioms.end());
    const std::vector<int>& degrees =
        idioms.at(static_cast<std::size_t>(roll(random, 0, static_cast<int>(idioms.size()) - 1)));
    const std::vector<Chord> chords = progression(key, degrees, 57);
    const std::vector<Chord> high = progression(key, degrees, 69);
    const int tonic = 48 + key.tonic;

    std::int64_t bar = 0;
    for (const Section& section : form) {
        b.marker(bar, section.name);
        if (!withNotes) {
            bar += section.bars;
            continue;
        }
        const std::string name = section.name;
        std::vector<GenNote> padNotes;
        std::vector<GenNote> arps;
        std::vector<GenNote> line;
        std::vector<GenNote> roots;
        for (std::int64_t i = 0; i < section.bars; ++i) {
            const Chord& chord = chords[static_cast<std::size_t>(i) % chords.size()];
            for (const int p : chord.pitches) {
                padNotes.push_back({.start = i * kBar,
                                    .length = kBar,
                                    .pitch = static_cast<std::uint8_t>(p),
                                    .velocity = 64});
            }
            if (name != "Intro") {
                const Chord& up = high[static_cast<std::size_t>(i) % high.size()];
                const bool sparse = name == "Outro";
                for (GenNote n :
                     arpeggio(up, i * kBar, kBar, sparse ? core::kPpq / 2 : core::kPpq / 4, 2,
                              ArpMode::Up, 78, 0.8, random)) {
                    arps.push_back(n);
                }
            }
            if (name == "Development" || name == "Outro") {
                const int root = scalePitch(key, tonic - 12, chord.degree - 1);
                roots.push_back({.start = i * kBar,
                                 .length = core::kPpq / 2,
                                 .pitch = static_cast<std::uint8_t>(root),
                                 .velocity = 80});
                roots.push_back(
                    {.start = (i * kBar) + (2 * core::kPpq),
                     .length = core::kPpq / 2,
                     .pitch = static_cast<std::uint8_t>(root + (chance(random, 0.3) ? 7 : 0)),
                     .velocity = 70});
            }
        }
        if (name == "Development" || name == "Outro") {
            MelodySpec spec;
            spec.key = key;
            spec.tonicPitch = tonic + 12;
            spec.lowest = 62;
            spec.highest = 84;
            spec.length = section.bars * kBar;
            spec.step = core::kPpq / 2;
            spec.density = name == "Outro" ? 0.2 : 0.38;
            spec.leap = 0.25;
            spec.velocity = 76;
            spec.chords = &high;
            spec.barTicks = kBar;
            line = melody(spec, random);
        }
        std::vector<std::pair<core::ChannelId, std::vector<GenNote>>> parts{{pad, padNotes}};
        if (!arps.empty()) {
            parts.emplace_back(kalimba, arps);
        }
        if (!line.empty()) {
            parts.emplace_back(piano, line);
        }
        if (!roots.empty()) {
            parts.emplace_back(bass, roots);
        }
        const core::PatternId pattern = b.pattern(name, section.bars, parts);
        b.place("Music", pattern, bar);
        bar += section.bars;
    }
    b.effectType(b.master(), "Limiter");
    out.notes = std::move(b.notes());
    out.notes.insert(out.notes.begin(), "form: intro 4, theme, development, outro 4 (" +
                                            std::to_string(bar) + " bars)");
    out.project = std::move(b).take();
    return out;
}

} // namespace adx::generate
