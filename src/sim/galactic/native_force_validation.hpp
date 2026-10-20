#pragma once

#include "encoding.hpp"
#include "eawr/sim/galactic/session.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr::sim::galactic::session_detail {
template<class T> core::Result<T> invalid(std::string message) {
    return core::Result<T>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, std::move(message)));
}
core::Result<std::vector<GalacticSession::NativeActorChange>> ordered_births(std::span<const CensusObject>);
core::Result<std::vector<EntityId>> validate_native_force(const GalacticSession::NativeProductionRequest&,
    const GalacticSession::NativeForcePlacement&, EntityId frontier, std::size_t live_objects,
    const NestedCreationSetup&, const std::function<bool(EntityId)>& existing_source);
} // namespace eawr::sim::galactic::session_detail
