// adx-thread: main
#include "engine/instruments/voice/Voicebank.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

#include "engine/format/text/ShiftJis.h"

namespace adx::instruments {
namespace {

namespace fs = std::filesystem;

/// A path from UTF-8 text. std::filesystem::path(std::string) reads the narrow string
/// in the OS's ANSI code page on Windows, which mangles every Japanese file name.
fs::path fromUtf8(std::string_view text) {
    std::u8string u8;
    u8.reserve(text.size());
    for (const char c : text) {
        u8.push_back(static_cast<char8_t>(c));
    }
    return fs::path{u8};
}

std::string toUtf8String(const fs::path& path) {
    const std::u8string u8 = path.u8string();
    return {u8.begin(), u8.end()};
}

bool readFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::uint64_t fnv(std::uint64_t h, std::string_view bytes) {
    for (const char c : bytes) {
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return h;
}

bool parseNumber(std::string_view text, double& out) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        out = 0.0;
        return true; // UTAU writes an empty field for zero
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

/// True for `<v> <kana>`: a VCV alias.
bool isVcv(std::string_view alias) {
    return alias.size() > 2 && alias[1] == ' ' &&
           std::string_view("aiueon").find(alias[0]) != std::string_view::npos;
}

/// One UTF-8 code point from the end of `text`, removed; 0 when empty.
char32_t popBack(std::string_view& text) {
    if (text.empty()) {
        return 0;
    }
    std::size_t start = text.size() - 1;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
        --start;
    }
    const std::string_view unit = text.substr(start);
    text = text.substr(0, start);
    const auto lead = static_cast<unsigned char>(unit[0]);
    char32_t code = 0;
    if (lead < 0x80) {
        return lead;
    }
    const std::size_t extra = unit.size() - 1;
    code = lead & (0x3F >> extra);
    for (std::size_t k = 1; k < unit.size(); ++k) {
        code = (code << 6) | (static_cast<unsigned char>(unit[k]) & 0x3F);
    }
    return code;
}

/// Hiragana and katakana to their vowel. Small ya/yu/yo carry theirs.
char vowelOfKana(char32_t code) {
    if (code >= 0x30A1 && code <= 0x30F6) {
        code -= 0x60; // katakana to hiragana
    }
    static constexpr std::array<std::pair<std::u32string_view, char>, 6> kRows{{
        {U"あかさたなはまやらわがざだばぱぁゃゎ", 'a'},
        {U"いきしちにひみりぎじぢびぴぃゐ", 'i'},
        {U"うくすつぬふむゆるぐずづぶぷぅゅゔっ", 'u'},
        {U"えけせてねへめれげぜでべぺぇゑ", 'e'},
        {U"おこそとのほもよろをごぞどぼぽぉょ", 'o'},
        {U"ん", 'n'},
    }};
    for (const auto& [row, vowel] : kRows) {
        if (row.find(code) != std::u32string_view::npos) {
            return vowel;
        }
    }
    return 0;
}

struct Romaji {
    std::string_view latin;
    std::string_view kana;
};

// The gojuon, voiced rows and the common contracted sounds. Longest first per onset is
// not needed: lookup is by the whole lyric.
// NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table.
constexpr std::array<Romaji, 101> kRomaji{{
    {"a", "あ"},     {"i", "い"},     {"u", "う"},     {"e", "え"},     {"o", "お"},
    {"ka", "か"},    {"ki", "き"},    {"ku", "く"},    {"ke", "け"},    {"ko", "こ"},
    {"sa", "さ"},    {"shi", "し"},   {"si", "し"},    {"su", "す"},    {"se", "せ"},
    {"so", "そ"},    {"ta", "た"},    {"chi", "ち"},   {"ti", "ち"},    {"tsu", "つ"},
    {"tu", "つ"},    {"te", "て"},    {"to", "と"},    {"na", "な"},    {"ni", "に"},
    {"nu", "ぬ"},    {"ne", "ね"},    {"no", "の"},    {"ha", "は"},    {"hi", "ひ"},
    {"fu", "ふ"},    {"hu", "ふ"},    {"he", "へ"},    {"ho", "ほ"},    {"ma", "ま"},
    {"mi", "み"},    {"mu", "む"},    {"me", "め"},    {"mo", "も"},    {"ya", "や"},
    {"yu", "ゆ"},    {"yo", "よ"},    {"ra", "ら"},    {"ri", "り"},    {"ru", "る"},
    {"re", "れ"},    {"ro", "ろ"},    {"wa", "わ"},    {"wo", "を"},    {"n", "ん"},
    {"ga", "が"},    {"gi", "ぎ"},    {"gu", "ぐ"},    {"ge", "げ"},    {"go", "ご"},
    {"za", "ざ"},    {"ji", "じ"},    {"zi", "じ"},    {"zu", "ず"},    {"ze", "ぜ"},
    {"zo", "ぞ"},    {"da", "だ"},    {"de", "で"},    {"do", "ど"},    {"ba", "ば"},
    {"bi", "び"},    {"bu", "ぶ"},    {"be", "べ"},    {"bo", "ぼ"},    {"pa", "ぱ"},
    {"pi", "ぴ"},    {"pu", "ぷ"},    {"pe", "ぺ"},    {"po", "ぽ"},    {"kya", "きゃ"},
    {"kyu", "きゅ"}, {"kyo", "きょ"}, {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"},
    {"cha", "ちゃ"}, {"chu", "ちゅ"}, {"cho", "ちょ"}, {"nya", "にゃ"}, {"nyu", "にゅ"},
    {"nyo", "にょ"}, {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"}, {"mya", "みゃ"},
    {"myu", "みゅ"}, {"myo", "みょ"}, {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
    {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"}, {"ja", "じゃ"},  {"ju", "じゅ"},
    {"jo", "じょ"},
}};
// NOLINTEND(modernize-use-designated-initializers)

} // namespace

bool Voicebank::load(const fs::path& root, std::vector<std::string>& warnings) {
    m_name.clear();
    m_readme.clear();
    m_subBanks.clear();
    m_entries.clear();
    m_index.clear();
    m_hash = 1469598103934665603ULL;

    std::error_code error;
    if (!fs::is_directory(root, error)) {
        warnings.push_back("ADX4300: voicebank '" + toUtf8String(root) + "' is not a folder");
        return false;
    }
    // character.txt and readme.txt at the root, or one level down (Teto's are).
    std::vector<fs::path> directories{root};
    for (const fs::directory_entry& entry : fs::directory_iterator(root, error)) {
        if (entry.is_directory(error)) {
            directories.push_back(entry.path());
        }
    }
    std::ranges::sort(directories.begin() + 1, directories.end());
    for (const fs::path& directory : directories) {
        std::string bytes;
        if (m_name.empty() && readFile(directory / "character.txt", bytes)) {
            m_hash = fnv(m_hash, bytes);
            const std::string text = format::toUtf8(bytes);
            std::istringstream lines(text);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.starts_with("name=")) {
                    m_name = line.substr(5);
                    if (!m_name.empty() && m_name.back() == '\r') {
                        m_name.pop_back();
                    }
                }
            }
        }
        if (m_readme.empty() && readFile(directory / "readme.txt", bytes)) {
            m_readme = format::toUtf8(bytes);
        }
    }
    // Sub-banks: the root itself and every folder below it (two levels) with an oto.ini.
    std::vector<fs::path> candidates{root};
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, error)) {
        if (!entry.is_directory(error)) {
            continue;
        }
        const fs::path relative = entry.path().lexically_relative(root);
        const auto depth =
            static_cast<std::size_t>(std::distance(relative.begin(), relative.end()));
        if (depth >= 1 && depth <= 2) {
            candidates.push_back(entry.path());
        }
    }
    std::ranges::sort(candidates.begin() + 1, candidates.end());
    for (const fs::path& directory : candidates) {
        if (fs::exists(directory / "oto.ini", error)) {
            loadSubBank(directory, warnings);
        }
    }
    if (m_subBanks.empty()) {
        warnings.push_back("ADX4300: no oto.ini under '" + toUtf8String(root) + "'");
        return false;
    }
    // Search order: most VCV aliases first (the continuous bank), then the rest as found.
    std::vector<std::size_t> order(m_subBanks.size());
    for (std::size_t s = 0; s < order.size(); ++s) {
        order[s] = s;
    }
    std::ranges::stable_sort(order, [this](std::size_t a, std::size_t b) {
        return m_subBanks[a].vcvAliases > m_subBanks[b].vcvAliases;
    });
    std::vector<std::size_t> rank(order.size());
    for (std::size_t r = 0; r < order.size(); ++r) {
        rank[order[r]] = r;
    }
    for (std::size_t e = 0; e < m_entries.size(); ++e) {
        m_index[m_entries[e].alias].push_back(e);
    }
    for (auto& [alias, list] : m_index) {
        std::ranges::stable_sort(list, [&](std::size_t a, std::size_t b) {
            return rank[m_entries[a].subBank] < rank[m_entries[b].subBank];
        });
    }
    if (m_name.empty()) {
        m_name = toUtf8String(root.filename());
    }
    return true;
}

void Voicebank::loadSubBank(const fs::path& directory, std::vector<std::string>& warnings) {
    std::string bytes;
    if (!readFile(directory / "oto.ini", bytes)) {
        return;
    }
    m_hash = fnv(m_hash, bytes);
    const std::string text = format::toUtf8(bytes);
    SubBank bank;
    bank.name = toUtf8String(directory.filename());
    bank.directory = directory;
    const std::size_t index = m_subBanks.size();

    std::istringstream lines(text);
    std::string line;
    std::size_t number = 0;
    std::unordered_map<std::string, std::uintmax_t> sizes;
    while (std::getline(lines, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            warnings.push_back("ADX4300: " + bank.name + "/oto.ini:" + std::to_string(number) +
                               ": no '='");
            continue;
        }
        OtoEntry entry;
        const std::string wav = line.substr(0, equals);
        std::vector<std::string> fields;
        std::string field;
        std::istringstream rest(line.substr(equals + 1));
        while (std::getline(rest, field, ',')) {
            fields.push_back(field);
        }
        while (fields.size() < 6) {
            fields.emplace_back();
        }
        entry.wav = directory / fromUtf8(wav);
        // An empty alias is the WAV's own name without its extension.
        entry.alias = fields[0].empty() ? wav.substr(0, wav.rfind('.')) : fields[0];
        bool ok = true;
        const std::array<double*, 5> targets{&entry.offset, &entry.consonant, &entry.cutoff,
                                             &entry.preutterance, &entry.overlap};
        for (std::size_t f = 0; f < 5; ++f) {
            ok = parseNumber(fields[f + 1], *targets[f]) && ok;
        }
        std::error_code error;
        auto known = sizes.find(wav);
        if (known == sizes.end()) {
            const std::uintmax_t size = fs::file_size(entry.wav, error);
            known = sizes.emplace(wav, error ? 0 : size).first;
            m_hash = fnv(m_hash, std::to_string(known->second));
        }
        if (!ok || known->second == 0 || entry.offset < 0.0) {
            warnings.push_back(
                "ADX4300: " + bank.name + "/oto.ini:" + std::to_string(number) + ": " +
                (known->second == 0 ? "missing WAV '" + wav + "'" : "timing out of range"));
            continue;
        }
        entry.subBank = index;
        bank.vcvAliases += isVcv(entry.alias) ? 1 : 0;
        ++bank.lines;
        m_entries.push_back(std::move(entry));
    }
    m_subBanks.push_back(std::move(bank));
}

const OtoEntry* Voicebank::find(std::string_view alias) const {
    const auto found = m_index.find(std::string(alias));
    return found == m_index.end() || found->second.empty() ? nullptr
                                                           : &m_entries[found->second.front()];
}

std::string romajiToKana(std::string_view lyric) {
    for (const Romaji& r : kRomaji) {
        if (r.latin == lyric) {
            return std::string(r.kana);
        }
    }
    return {};
}

std::string vowelOf(std::string_view lyric) {
    if (lyric.empty()) {
        return {};
    }
    // Romaji: its last letter, if a vowel or n.
    const auto last = static_cast<unsigned char>(lyric.back());
    if (last < 0x80) {
        const char c = static_cast<char>(last);
        return std::string_view("aiueon").find(c) != std::string_view::npos ? std::string(1, c)
                                                                            : std::string{};
    }
    std::string_view rest = lyric;
    while (!rest.empty()) {
        const char32_t code = popBack(rest);
        if (code == U'ー') {
            continue; // a long mark extends the vowel before it
        }
        const char vowel = vowelOfKana(code);
        return vowel != 0 ? std::string(1, vowel) : std::string{};
    }
    return {};
}

const OtoEntry* resolveAlias(const Voicebank& bank, std::string_view lyric,
                             std::string_view previousLyric) {
    std::vector<std::string> spellings{std::string(lyric)};
    if (const std::string kana = romajiToKana(lyric); !kana.empty()) {
        spellings.push_back(kana);
    }
    const std::string vowel = vowelOf(previousLyric);
    for (const std::string& spelling : spellings) {
        std::vector<std::string> tries;
        if (!vowel.empty()) {
            std::string vcv = vowel;
            vcv += ' ';
            vcv += spelling;
            tries.push_back(std::move(vcv));
            tries.push_back(spelling);
            tries.push_back("- " + spelling);
        } else {
            tries.push_back("- " + spelling);
            tries.push_back(spelling);
        }
        tries.push_back("* " + spelling);
        for (const std::string& alias : tries) {
            if (const OtoEntry* entry = bank.find(alias)) {
                return entry;
            }
        }
    }
    return nullptr;
}

} // namespace adx::instruments
