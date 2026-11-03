// adx-thread: realtime
//
// A node whose parameters it declares itself rather than the TypeCatalog: a hosted
// plugin's (phase_9.md §4.7). The graph lays out such a node's slice from these, in
// this order. A parameter the project has given no value gets NaN, which the node
// reads as "leave the plugin's own value alone" - so a plugin's saved state is not
// overwritten by defaults the project never chose.
#pragma once

#include <cstdint>
#include <string_view>

namespace adx::graph {

class DeclaresParams {
public:
    DeclaresParams() = default;
    virtual ~DeclaresParams() = default;
    DeclaresParams(const DeclaresParams&) = delete;
    DeclaresParams& operator=(const DeclaresParams&) = delete;
    DeclaresParams(DeclaresParams&&) = delete;
    DeclaresParams& operator=(DeclaresParams&&) = delete;

    [[nodiscard]] virtual std::uint32_t declaredParamCount() const noexcept = 0;
    /// The .adx key of parameter `index`.
    [[nodiscard]] virtual std::string_view
    declaredParamName(std::uint32_t index) const noexcept = 0;
};

} // namespace adx::graph
