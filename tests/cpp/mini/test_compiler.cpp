// The mini-notation compiler (phase_7.md §6). The corpus is written against the
// documented grammar (docs/mininotation.md), not against the implementation, so it
// can find a bug the port carried over rather than enshrine it: mini_euclid_table
// did - the archive's bucket formula is a rotation of Bjorklund's, not Bjorklund's.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/format/adx/Diagnostics.h"
#include "engine/format/adx/Parser.h"
#include "engine/format/mininotation/PatternCompiler.h"
#include "engine/project/EventCompile.h"
#include "engine/project/SnapshotBuilder.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::core::kPpq;
using adx::core::Ticks;
namespace mini = adx::mini;

namespace {

constexpr std::int64_t kBar = kPpq * 4;

/// mini_deterministic's fingerprint of 16 realised cycles, recorded when the port
/// landed. It changes only if what a pattern means changes.
constexpr std::uint32_t kMiniFingerprint = 0x6c2d2eddU;

struct Expect {
    std::int64_t start;
    std::int64_t length;
    int pitch;
};

std::vector<Expect> notesOf(std::string_view text, std::int64_t cycle = 0) {
    const mini::CompileResult result = mini::compile(text, Ticks{0}, Ticks{kBar}, cycle);
    REQUIRE(result.ok);
    std::vector<Expect> out;
    out.reserve(result.notes.size());
    for (const mini::MiniNote& n : result.notes) {
        out.push_back(Expect{.start = n.start.value, .length = n.length.value, .pitch = n.pitch});
    }
    return out;
}

bool same(const std::vector<Expect>& a, const std::vector<Expect>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].start != b[i].start || a[i].length != b[i].length || a[i].pitch != b[i].pitch) {
            return false;
        }
    }
    return true;
}

std::string pattern(const std::vector<bool>& hits) {
    std::string out;
    for (const bool hit : hits) {
        out += hit ? 'x' : '.';
    }
    return out;
}

// Tidal's bjorklund, transcribed independently of the engine's: pairs of
// (count, sequence), distributing the smaller group over the larger until one group
// has at most one element. The engine's implementation must agree with it everywhere.
std::string tidalBjorklund(int k, int n) {
    using Group = std::pair<int, std::vector<std::string>>;
    Group a{k, std::vector<std::string>(static_cast<std::size_t>(k), "x")};
    Group b{n - k, std::vector<std::string>(static_cast<std::size_t>(n - k), ".")};
    while (std::min(a.first, b.first) > 1) {
        if (a.first > b.first) {
            // right: the first b.first of a absorb b.
            std::vector<std::string> joined;
            joined.reserve(b.first);
            for (int i = 0; i < b.first; ++i) {
                joined.push_back(a.second[static_cast<std::size_t>(i)] +
                                 b.second[static_cast<std::size_t>(i)]);
            }
            std::vector<std::string> const rest(a.second.begin() + b.first, a.second.end());
            const int restCount = a.first - b.first;
            a = Group{b.first, joined};
            b = Group{restCount, rest};
        } else {
            // left: every a absorbs one of b.
            std::vector<std::string> joined;
            joined.reserve(a.first);
            for (int i = 0; i < a.first; ++i) {
                joined.push_back(a.second[static_cast<std::size_t>(i)] +
                                 b.second[static_cast<std::size_t>(i)]);
            }
            std::vector<std::string> const rest(b.second.begin() + a.first, b.second.end());
            b = Group{b.first - a.first, rest};
            a = Group{a.first, joined};
        }
    }
    std::string out;
    for (const std::string& s : a.second) {
        out += s;
    }
    for (const std::string& s : b.second) {
        out += s;
    }
    return out;
}

} // namespace

TEST_CASE("mini_grammar_corpus", "[mini]") {
    // One bar of 4/4 is 15360 ticks; every expectation below is worked out by hand from
    // docs/mininotation.md.
    CHECK(same(notesOf("bd sn"), {{0, 7680, 36}, {7680, 7680, 38}}));
    CHECK(same(notesOf("~ sn"), {{7680, 7680, 38}}));
    CHECK(same(notesOf("[a b] c"), {{0, 3840, 69}, {3840, 3840, 71}, {7680, 7680, 60}}));
    CHECK(same(notesOf("c e g"), {{0, 5120, 60}, {5120, 5120, 64}, {10240, 5120, 67}}));
    CHECK(same(notesOf("c#5 eb"), {{0, 7680, 73}, {7680, 7680, 63}}));
    CHECK(same(notesOf("bd*4"),
               {{0, 3840, 36}, {3840, 3840, 36}, {7680, 3840, 36}, {11520, 3840, 36}}));
    CHECK(same(notesOf("bd!3 sn"),
               {{0, 3840, 36}, {3840, 3840, 36}, {7680, 3840, 36}, {11520, 3840, 38}}));
    CHECK(same(notesOf("bd ! sn"), {{0, 5120, 36}, {5120, 5120, 36}, {10240, 5120, 38}}));
    CHECK(same(notesOf("bd@3 sn"), {{0, 11520, 36}, {11520, 3840, 38}}));
    // @1.5 against @1: boundaries at 15360 * 1500 / 2500 = 9216.
    CHECK(same(notesOf("bd@1.5 sn"), {{0, 9216, 36}, {9216, 6144, 38}}));
    CHECK(same(notesOf("bd(3,8)"), {{0, 1920, 36}, {5760, 1920, 36}, {11520, 1920, 36}}));
    // Rotated left by 2: x..x..x. -> .x..x.x.
    CHECK(same(notesOf("bd(3,8,2)"), {{1920, 1920, 36}, {7680, 1920, 36}, {11520, 1920, 36}}));
    // Stacked layers sound together; simultaneous notes keep layer order.
    CHECK(same(notesOf("bd, hh*2"), {{0, 15360, 36}, {0, 7680, 42}, {7680, 7680, 42}}));
    // Alternation picks by cycle.
    CHECK(same(notesOf("<a b>", 0), {{0, 15360, 69}}));
    CHECK(same(notesOf("<a b>", 1), {{0, 15360, 71}}));
    CHECK(same(notesOf("<a b>", 2), {{0, 15360, 69}}));
    // /2: plays on even cycles only.
    CHECK(same(notesOf("bd/2", 0), {{0, 15360, 36}}));
    CHECK(notesOf("bd/2", 1).empty());
    // Scale degrees: trailing form and pipe form; negative degrees fall an octave.
    CHECK(
        same(notesOf("0 2 4 scale=c_major"), {{0, 5120, 60}, {5120, 5120, 64}, {10240, 5120, 67}}));
    CHECK(same(notesOf("0 1 | scale=a_minor"), {{0, 7680, 69}, {7680, 7680, 71}}));
    CHECK(same(notesOf("-1 7 scale=c_major"), {{0, 7680, 59}, {7680, 7680, 72}}));
    CHECK(same(notesOf("0 3 scale=d_dorian"), {{0, 7680, 62}, {7680, 7680, 67}}));
    // Transforms.
    CHECK(same(notesOf("bd sn | rev"), {{0, 7680, 38}, {7680, 7680, 36}}));
    CHECK(same(notesOf("bd sn | fast 2"),
               {{0, 3840, 36}, {3840, 3840, 38}, {7680, 3840, 36}, {11520, 3840, 38}}));
    CHECK(same(notesOf("bd sn | every 2 rev", 0), {{0, 7680, 38}, {7680, 7680, 36}}));
    CHECK(same(notesOf("bd sn | every 2 rev", 1), {{0, 7680, 36}, {7680, 7680, 38}}));
    // The plan's own example, phase_7.md §4.7: degrees 0 3 5 7 of A natural minor are
    // A4 D5 F5 A5.
    CHECK(same(notesOf("0 3 5 7 | scale=a_minor | fast 2"), {{0, 1920, 69},
                                                             {1920, 1920, 74},
                                                             {3840, 1920, 77},
                                                             {5760, 1920, 81},
                                                             {7680, 1920, 69},
                                                             {9600, 1920, 74},
                                                             {11520, 1920, 77},
                                                             {13440, 1920, 81}}));
    // An empty pattern is silence, not an error.
    CHECK(notesOf("").empty());
    CHECK(notesOf("~ ~").empty());
}

TEST_CASE("mini_errors_have_spans", "[mini]") {
    {
        const auto r = mini::compile("[bd sn", Ticks{0}, Ticks{kBar}, 0);
        CHECK_FALSE(r.ok);
        CHECK(r.notes.empty());
        REQUIRE(r.problems.size() == 1);
        CHECK(r.problems[0].code == adx::format::code::kMiniSyntax);
        CHECK(r.problems[0].start == 6); // at the end, where ']' was expected
    }
    {
        const std::string text = "bd sn | wobble 3";
        const auto r = mini::compile(text, Ticks{0}, Ticks{kBar}, 0);
        CHECK_FALSE(r.ok);
        REQUIRE(r.problems.size() == 1);
        CHECK(text.substr(r.problems[0].start, r.problems[0].length) == "wobble 3");
    }
    {
        const std::string text = "bd zz sn";
        const auto r = mini::compile(text, Ticks{0}, Ticks{kBar}, 0);
        CHECK(r.ok); // a warning, and the step still plays (middle C)
        REQUIRE(r.problems.size() == 1);
        CHECK(r.problems[0].code == adx::format::code::kMiniUnknownStep);
        CHECK(text.substr(r.problems[0].start, r.problems[0].length) == "zz");
        REQUIRE(r.notes.size() == 3);
        CHECK(r.notes[1].pitch == 60);
    }
    {
        // Mapped to the file: the second line of a MINI block, columns from 3.
        const std::vector<std::string> lines{"bd sn", "hh [cp"};
        const std::vector<mini::SourceLine> where{{.line = 10, .column = 3, .byteOffset = 100},
                                                  {.line = 11, .column = 3, .byteOffset = 110}};
        const auto r = mini::compile(mini::joinLines(lines), Ticks{0}, Ticks{kBar}, 0);
        adx::format::DiagnosticList diagnostics;
        mini::toDiagnostics(r.problems, lines, where, diagnostics);
        REQUIRE(diagnostics.size() == 1);
        const auto& d = diagnostics.all()[0];
        CHECK(d.span.line == 11);
        CHECK(d.span.column == 3 + 6); // "hh [cp" has length 6: the error is at its end
    }
}

TEST_CASE("mini_deterministic", "[mini]") {
    const std::string text =
        "<bd [bd bd]> sn?0.7 hh(5,8,1)?, <0 2 4>*2 | scale=e_minor | every 3 rev";
    const auto first = mini::compile(text, Ticks{0}, Ticks{kBar}, 5);
    REQUIRE(first.ok);
    REQUIRE_FALSE(first.notes.empty());
    for (int i = 0; i < 1000; ++i) {
        const auto again = mini::compile(text, Ticks{0}, Ticks{kBar}, 5);
        REQUIRE(again.notes == first.notes);
        REQUIRE(again.spans == first.spans);
    }
    // Across processes and builds: the same text compiles to these exact notes in
    // Debug, RelWithDebInfo and Release, on any machine. A fingerprint of the output of
    // 16 cycles, recorded when the port landed.
    std::uint32_t fingerprint = 2166136261U;
    const auto mix = [&](std::int64_t v) {
        for (int b = 0; b < 8; ++b) {
            fingerprint ^= static_cast<std::uint32_t>((v >> (8 * b)) & 0xFF);
            fingerprint *= 16777619U;
        }
    };
    for (const mini::RealizedNote& n :
         mini::realize(text, Ticks{0}, Ticks{kBar * 16}, Ticks{kBar})) {
        mix(n.note.start.value);
        mix(n.note.length.value);
        mix(n.note.pitch);
        mix(n.span.start);
    }
    CHECK(fingerprint == kMiniFingerprint);
}

TEST_CASE("mini_probability_seeded", "[mini]") {
    const std::string text = "hh*64?";
    const auto a = mini::compile(text, Ticks{0}, Ticks{kBar}, 0, 1234);
    const auto b = mini::compile(text, Ticks{0}, Ticks{kBar}, 0, 1234);
    const auto c = mini::compile(text, Ticks{0}, Ticks{kBar}, 0, 999);
    CHECK(a.notes == b.notes);
    CHECK(a.notes != c.notes);
    // About half survive at the default probability of 0.5.
    CHECK(a.notes.size() > 16);
    CHECK(a.notes.size() < 48);
    // The default seed is the text's: still deterministic, and it varies by cycle.
    const auto d0 = mini::compile(text, Ticks{0}, Ticks{kBar}, 0);
    const auto d0again = mini::compile(text, Ticks{0}, Ticks{kBar}, 0);
    const auto d1 = mini::compile(text, Ticks{0}, Ticks{kBar}, 1);
    CHECK(d0.notes == d0again.notes);
    CHECK(d0.spans.size() == d0.notes.size());
    std::vector<std::int64_t> starts0;
    std::vector<std::int64_t> starts1;
    starts0.reserve(d0.notes.size());
    for (const auto& n : d0.notes) {
        starts0.push_back(n.start.value);
    }
    starts1.reserve(d1.notes.size());
    for (const auto& n : d1.notes) {
        starts1.push_back(n.start.value);
    }
    CHECK(starts0 != starts1);
    // ?0 never plays; ?1 always does.
    CHECK(mini::compile("bd*8?0", Ticks{0}, Ticks{kBar}, 0).notes.empty());
    CHECK(mini::compile("bd*8?1", Ticks{0}, Ticks{kBar}, 0).notes.size() == 8);
}

TEST_CASE("mini_euclid_table", "[mini]") {
    // Toussaint's examples, as Tidal and Strudel spell them (first onset first). Where
    // Toussaint prints a different rotation of the same necklace, Tidal's is the one a
    // live coder hears: E(3,4) is xxx. in Tidal, x.xx in the paper.
    CHECK(pattern(mini::euclid(3, 8, 0)) == "x..x..x.");
    CHECK(pattern(mini::euclid(5, 8, 0)) == "x.xx.xx.");
    CHECK(pattern(mini::euclid(2, 5, 0)) == "x.x..");
    CHECK(pattern(mini::euclid(3, 4, 0)) == "xxx.");
    CHECK(pattern(mini::euclid(3, 5, 0)) == "x.x.x");
    CHECK(pattern(mini::euclid(3, 7, 0)) == "x.x.x..");
    CHECK(pattern(mini::euclid(4, 7, 0)) == "x.x.x.x");
    CHECK(pattern(mini::euclid(4, 9, 0)) == "x.x.x.x..");
    CHECK(pattern(mini::euclid(4, 11, 0)) == "x..x..x..x.");
    CHECK(pattern(mini::euclid(5, 7, 0)) == "x.xx.xx");
    CHECK(pattern(mini::euclid(5, 9, 0)) == "x.x.x.x.x");
    CHECK(pattern(mini::euclid(5, 12, 0)) == "x..x.x..x.x.");
    CHECK(pattern(mini::euclid(7, 12, 0)) == "x.xx.x.xx.x.");
    CHECK(pattern(mini::euclid(7, 16, 0)) == "x..x.x.x..x.x.x.");
    CHECK(pattern(mini::euclid(9, 16, 0)) == "x.xx.x.x.xx.x.x.");
    // Rotation is to the left.
    CHECK(pattern(mini::euclid(3, 8, 2)) == ".x..x.x.");
    CHECK(pattern(mini::euclid(3, 8, -1)) == ".x..x..x"); // negative: to the right

    for (int n = 1; n <= 16; ++n) {
        for (int k = 0; k <= n; ++k) {
            const std::vector<bool> hits = mini::euclid(k, n, 0);
            INFO("E(" << k << "," << n << ") = " << pattern(hits));
            REQUIRE(hits.size() == static_cast<std::size_t>(n));
            CHECK(pattern(hits) == tidalBjorklund(k, n));
            // Maximally even: exactly k onsets, and the gaps take at most two lengths
            // that differ by one.
            int onsets = 0;
            std::vector<int> gaps;
            int last = -1;
            for (int i = 0; i < n; ++i) {
                if (hits[static_cast<std::size_t>(i)]) {
                    ++onsets;
                    if (last >= 0) {
                        gaps.push_back(i - last);
                    }
                    last = i;
                }
            }
            CHECK(onsets == k);
            if (k > 0) {
                CHECK(hits[0]);
                int first = 0;
                while (!hits[static_cast<std::size_t>(first)]) {
                    ++first;
                }
                gaps.push_back(n - last + first);
                const auto [lo, hi] = std::ranges::minmax(gaps);
                CHECK(hi - lo <= 1);
            }
        }
    }
}

TEST_CASE("mini_step_spans", "[mini]") {
    const std::vector<std::string> corpus{
        "bd sn",
        "[bd [sn sn]] hh*2",
        "<c e g>*3",
        "bd(3,8,1) cp",
        "0 2 4 scale=c_major",
        "a b, c? d@2",
        "bd!2 ! sn | rev | fast 3",
    };
    for (const std::string& text : corpus) {
        for (std::int64_t cycle = 0; cycle < 4; ++cycle) {
            const auto r = mini::compile(text, Ticks{0}, Ticks{kBar}, cycle);
            REQUIRE(r.ok);
            REQUIRE(r.spans.size() == r.notes.size());
            for (std::size_t i = 0; i < r.notes.size(); ++i) {
                const mini::StepSpan s = r.spans[i];
                INFO(text << " cycle " << cycle << " note " << i);
                REQUIRE(s.start + s.length <= text.size());
                const std::string token = text.substr(s.start, s.length);
                // The span is exactly a step token: compiling it alone gives the pitch.
                const std::string alone =
                    text.find("scale=") != std::string::npos ? token + " scale=c_major" : token;
                const auto single = mini::compile(alone, Ticks{0}, Ticks{kBar}, 0);
                REQUIRE(single.notes.size() == 1);
                CHECK(single.notes[0].pitch == r.notes[i].pitch);
            }
        }
    }
}

TEST_CASE("mini_cycle_index", "[mini]") {
    // Realised over 16 bars: <a b> alternates and every 4 rev flips cycles 0, 4, 8, 12.
    const auto alt = mini::realize("<a b>", Ticks{0}, Ticks{kBar * 16}, Ticks{kBar});
    REQUIRE(alt.size() == 16);
    for (std::size_t i = 0; i < alt.size(); ++i) {
        CHECK(std::cmp_equal(alt[i].cycle, i));
        CHECK(alt[i].note.start.value == static_cast<std::int64_t>(i) * kBar);
        CHECK(alt[i].note.pitch == (i % 2 == 0 ? 69 : 71));
    }
    const auto every =
        mini::realize("bd sn | every 4 rev", Ticks{0}, Ticks{kBar * 16}, Ticks{kBar});
    REQUIRE(every.size() == 32);
    for (std::int64_t cycle = 0; cycle < 16; ++cycle) {
        const auto& firstOfBar = every[static_cast<std::size_t>(cycle * 2)];
        CHECK(firstOfBar.note.start.value == cycle * kBar);
        CHECK(firstOfBar.note.pitch == (cycle % 4 == 0 ? 38 : 36));
    }
    // A range that starts mid-song realises the song's cycles, not cycle 0.
    const auto late = mini::realize("<a b c>", Ticks{kBar * 7}, Ticks{kBar * 8}, Ticks{kBar});
    REQUIRE(late.size() == 1);
    CHECK(late[0].cycle == 7);
    CHECK(late[0].note.pitch == 71); // 7 % 3 == 1: b
}

TEST_CASE("mini_plays_through_the_snapshot", "[mini][render]") {
    // A MINI block is compiled into its channel's events, at the song's cycles, and an
    // edit to anything else leaves its voice ids alone.
    auto loaded = adx::tests::loadText(R"([PROJECT]
ADX_VERSION=2

[CHANNEL Drums]
OUTPUT=insert.1

[PATTERN Beat]
LENGTH=2:0:0
MINI Drums
  <bd sn> hh

[PLAYLIST]
TRACK 1
  PATTERN Beat 1:0:0

[MIXER]
INSERT 1 name="Master"
)");
    REQUIRE_FALSE(loaded->diagnostics.hasErrors());
    adx::project::SnapshotBuilder builder{48000, 512};
    auto built = builder.build(loaded->project, 1, adx::project::dirty::kAll);
    REQUIRE(built.snapshot != nullptr);
    REQUIRE(built.snapshot->eventTracks.size() == 1);
    std::vector<std::pair<std::int64_t, int>> ons;
    for (const auto& e : built.snapshot->eventTracks[0].events) {
        if (e.kind == adx::project::EventKind::NoteOn) {
            ons.emplace_back(e.tick, e.pitch);
            CHECK((e.noteId & adx::project::kMiniNoteIdBit) != 0);
        }
    }
    // Placed at bar 2 (song cycle 1, then 2): sn hh, then bd hh.
    const std::vector<std::pair<std::int64_t, int>> expected{
        {kBar, 38}, {kBar + (kBar / 2), 42}, {2 * kBar, 36}, {(2 * kBar) + (kBar / 2), 42}};
    CHECK(ons == expected);
    adx::project::destroySnapshot(built.snapshot);
}

TEST_CASE("mini_problem_reported_at_file_position", "[mini][format]") {
    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(R"([PROJECT]
ADX_VERSION=2

[CHANNEL Drums]
OUTPUT=insert.1

[PATTERN Beat]
MINI Drums
  bd sn
  hh [cp zz

[MIXER]
INSERT 1 name="Master"
)",
                      project, stack, diagnostics);
    REQUIRE(diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : diagnostics.all()) {
        if (d.code == adx::format::code::kMiniSyntax) {
            found = true;
            CHECK(d.span.line == 10);
            CHECK(d.span.column == 3 + 9); // the end of "hh [cp zz"
        }
    }
    CHECK(found);
}
