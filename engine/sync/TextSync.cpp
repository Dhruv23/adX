// adx-thread: main
#include "engine/sync/TextSync.h"

#include <utility>

#include "engine/core/Config.h"
#include "engine/format/adx/Parser.h"
#include "engine/project/Project.h"
#include "engine/project/SourceMap.h"
#include "engine/project/Validate.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/sync/ReloadPolicy.h"

namespace adx::sync {

namespace {

constexpr std::size_t kRecentWrites = 16;

/// Clears a flag on every way out of a scope.
class Applying {
public:
    explicit Applying(bool& flag) noexcept : m_flag(flag) {
        m_flag = true;
    }
    ~Applying() {
        m_flag = false;
    }
    Applying(const Applying&) = delete;
    Applying& operator=(const Applying&) = delete;
    Applying(Applying&&) = delete;
    Applying& operator=(Applying&&) = delete;

private:
    bool& m_flag;
};

} // namespace

const char* toString(SyncMode mode) noexcept {
    switch (mode) {
    case SyncMode::Off:
        return "off";
    case SyncMode::OnSave:
        return "on-save";
    case SyncMode::Live:
        return "live";
    }
    return "on-save";
}

const char* toString(ApplyStatus status) noexcept {
    switch (status) {
    case ApplyStatus::Unchanged:
        return "unchanged";
    case ApplyStatus::Applied:
        return "applied";
    case ApplyStatus::Invalid:
        return "invalid";
    case ApplyStatus::Conflict:
        return "conflict";
    case ApplyStatus::SelfWrite:
        return "self-write";
    }
    return "unchanged";
}

std::uint64_t contentHash(std::string_view text) noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

TextSync::TextSync(project::Project& project, project::CommandStack& stack,
                   format::WriteOptions spelling, std::string baseDirectory)
    : m_project(project), m_stack(stack), m_spelling(spelling), m_lineEnding(spelling.lineEnding),
      m_baseDirectory(std::move(baseDirectory)) {
    // The options hold a view; keep the bytes it views alive here.
    m_spelling.lineEnding = m_lineEnding;
    m_syncedRevision = m_stack.revision();
}

void TextSync::markSynced(std::string_view text) {
    m_syncedText = std::string(text);
    m_syncedRevision = m_stack.revision();
}

std::string TextSync::renderText() const {
    return format::write(m_project, m_spelling);
}

bool TextSync::modelDirty() const noexcept {
    return m_stack.revision() != m_syncedRevision;
}

bool TextSync::modelDiffers() const {
    if (!modelDirty()) {
        return false;
    }
    project::Project synced;
    project::CommandStack scratch;
    format::DiagnosticList ignored;
    format::ParseOptions options;
    options.baseDirectory = m_baseDirectory;
    format::load(m_syncedText, synced, scratch, ignored, options);
    return !format::sameContent(m_project, format::remapInto(m_project, synced));
}

void TextSync::noteWrite(std::string_view text) {
    m_recentWrites.push_back(contentHash(text));
    while (m_recentWrites.size() > kRecentWrites) {
        m_recentWrites.pop_front();
    }
}

ApplyResult TextSync::applyText(std::string_view text) {
    return apply(text, "Edit text", false, false);
}

ApplyResult TextSync::applyExternal(std::string_view text) {
    return apply(text, "Reload from file", true, false);
}

ApplyResult TextSync::applyForced(std::string_view text) {
    return apply(text, "Reload from file", true, true);
}

ApplyResult TextSync::apply(std::string_view text, std::string_view label, bool external,
                            bool force) {
    // Guard 3. A re-entrant apply means a caller reacted to the change an apply made by
    // applying again - the loop this class exists to prevent.
    ADX_ASSERT(!m_applying);
    ApplyResult result;
    if (m_applying) {
        result.status = ApplyStatus::Unchanged;
        return result;
    }
    const Applying applying(m_applying);

    // Guard 1.
    if (external && !force) {
        const std::uint64_t hash = contentHash(text);
        for (const std::uint64_t written : m_recentWrites) {
            if (written == hash) {
                result.status = ApplyStatus::SelfWrite;
                return result;
            }
        }
        // What the file held when the two last agreed carries no news - a late event for
        // a change already applied - and must never be mistaken for a rival edit.
        if (text == m_syncedText) {
            result.status = ApplyStatus::Unchanged;
            return result;
        }
    }

    project::Project target;
    project::CommandStack scratch;
    project::SourceMap sources;
    format::ParseOptions options;
    options.baseDirectory = m_baseDirectory;
    options.sources = &sources;
    format::load(text, target, scratch, result.diagnostics, options);
    // Validated too: a routing cycle typed into the text is refused here, at its line,
    // rather than discovered by the engine after the model took it.
    project::ValidationOptions validation;
    validation.sources = &sources;
    static_cast<void>(project::validate(target, result.diagnostics, validation));
    if (!reloadMayApply(result.diagnostics)) {
        result.status = ApplyStatus::Invalid;
        return result;
    }

    if (external && !force && modelDiffers()) {
        // The model has edits the file had not seen. If the new text already says what
        // the model says (someone saved the same thing), there is nothing to decide.
        const project::Project remapped = format::remapInto(m_project, target);
        if (format::sameContent(m_project, remapped)) {
            markSynced(text);
            result.status = ApplyStatus::Unchanged;
            return result;
        }
        result.status = ApplyStatus::Conflict;
        return result;
    }

    // Guard 2: a patch with no difference executes nothing.
    result.patch = format::applyPatch(m_project, m_stack, target, label);
    result.status = result.patch.commands > 0 ? ApplyStatus::Applied : ApplyStatus::Unchanged;
    if (result.patch.commands > 0) {
        ++m_applies;
    }
    if (external) {
        // The file is the source of this change: the two agree now, and the model has
        // nothing to write back.
        markSynced(text);
    }
    return result;
}

} // namespace adx::sync
