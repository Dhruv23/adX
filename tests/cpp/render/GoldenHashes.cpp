#include "tests/cpp/render/GoldenHashes.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

#include "tests/cpp/Corpus.h"
#include "tests/cpp/Env.h"

namespace adx::tests {

std::filesystem::path goldenDirectory() {
    return repoRoot() / "tests" / "golden";
}

std::map<std::string, std::string> readHashes(const std::filesystem::path& file) {
    std::map<std::string, std::string> hashes;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::istringstream fields(line);
        std::string name;
        std::string hash;
        fields >> name >> hash;
        if (!name.empty()) {
            hashes[name] = hash;
        }
    }
    return hashes;
}

void checkGolden(const std::filesystem::path& file, std::string_view header,
                 const std::map<std::string, std::string>& actual) {
    if (environment("ADX_UPDATE_GOLDEN")) {
        std::ofstream out(file, std::ios::binary);
        out << header;
        for (const auto& [name, hash] : actual) {
            out << name << ' ' << hash << '\n';
        }
        WARN("golden hashes rewritten: " << file.string());
        return;
    }

    const std::map<std::string, std::string> committed = readHashes(file);
    for (const auto& [name, hash] : actual) {
        INFO(name);
        const auto found = committed.find(name);
        REQUIRE(found != committed.end());
        CHECK(found->second == hash);
    }
    for (const auto& [name, hash] : committed) {
        INFO(name << " is committed but no longer rendered");
        CHECK(actual.contains(name));
    }
}

} // namespace adx::tests
