// adx-thread: main
//
// The gm instrument's main-thread half (phase_12.md §4.7.5): banks loaded once and
// shared, a channel's BANK= resolved to a file, and the node given flat views of the
// bank that its pins keep alive. The node itself (GmInstrument.h) is realtime code and
// sees no string, vector or shared_ptr.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "engine/format/soundfont/BankModel.h"
#include "engine/instruments/gm/GmInstrument.h"
#include "engine/project/Channel.h"

namespace adx::instruments {

/// Banks by file, parsed once and shared while any node holds one.
class GmBankCache {
public:
    /// The bank at `path`, loading it if no node holds it. Null (with `error`) when it
    /// cannot be read.
    [[nodiscard]] static std::shared_ptr<const format::soundfont::Bank>
    get(const std::filesystem::path& path, std::string& error);
};

/// The GM bank adX fetches into the library store (GeneralUser GS): what `system:gm.dls`
/// plays on a machine that has no Windows bank.
inline constexpr std::string_view kShippedGmBank = "lib://generaluser/GeneralUser-GS.sf2";

/// The file a channel's BANK= names: `system:gm.dls` and empty are the Windows bank, or
/// the shipped one without it; `lib://` is in the library store; a relative path is
/// relative to `base`. Empty when there is no such file.
[[nodiscard]] std::filesystem::path gmBankPath(const std::string& written,
                                               const std::filesystem::path& base);

/// Gives `node` the bank (null for none) and remembers the path it came from.
void setGmBank(GmInstrument& node, std::shared_ptr<const format::soundfont::Bank> bank,
               std::filesystem::path path);
/// The bank a node plays, and where it came from; null and empty for none.
[[nodiscard]] const format::soundfont::Bank* gmBankOf(const GmInstrument& node) noexcept;
[[nodiscard]] std::filesystem::path gmBankPathOf(const GmInstrument& node);

/// Builds the node's bank from the channel; a missing bank leaves the node silent.
void configureGm(GmInstrument& node, const project::Channel& channel,
                 const std::filesystem::path& base);
[[nodiscard]] bool gmMatches(const GmInstrument& node, const project::Channel& channel,
                             const std::filesystem::path& base);

} // namespace adx::instruments
