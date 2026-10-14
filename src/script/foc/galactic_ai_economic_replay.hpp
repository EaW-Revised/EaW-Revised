#pragma once

#include "galactic_ai_actions.hpp"

namespace eawr::script::foc::galactic_ai::economic_replay {
using Bytes = std::vector<std::uint8_t>;
[[nodiscard]] Bytes production(const gc::GalacticSession::NativeProductionRequest&, const gc::GalacticSession::NativeProductionReceipt&);
[[nodiscard]] Bytes payment(const gc::GalacticSession::NativeProductionNotice&, const gc::GalacticSession::NativeProductionPayment&);
[[nodiscard]] Bytes magic(const ActionRequest&, std::span<const sim::EntityId>);
[[nodiscard]] Bytes cash(gc::PlayerId, sim::math::Fixed amount, sim::math::Fixed multiplier, sim::math::Fixed actual);
[[nodiscard]] Bytes finish(sim::EntityId, bool salvage, sim::math::Fixed multiplier);
}
