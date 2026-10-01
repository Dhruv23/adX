// A send: a tap on an insert, scaled by its level, feeding another insert.
//
// Pre- or post-fader is not a property of this node but of which of the insert's two
// outputs its input edge leaves from - so a send is an ordinary edge pair in the DAG,
// sorted, delay-compensated and cycle-checked like every other connection.
#pragma once

#include <cstdint>

#include "engine/graph/Node.h"

namespace adx::graph {

enum class SendParam : std::uint32_t { Level, Count };

inline constexpr std::uint32_t kSendParamCount = static_cast<std::uint32_t>(SendParam::Count);

class SendNode final : public Node {
public:
    void prepare(const PrepareInfo& info) override;
    void process(ProcessContext& context) noexcept override;
    void reset() noexcept override;
    [[nodiscard]] PortSpec ports() const noexcept override;
};

} // namespace adx::graph
