#include "engine/core/Version.h"

namespace adx {

std::string_view version() noexcept {
    return ADX_VERSION_STRING;
}

std::string_view gitSha() noexcept {
    return ADX_GIT_SHA;
}

} // namespace adx
