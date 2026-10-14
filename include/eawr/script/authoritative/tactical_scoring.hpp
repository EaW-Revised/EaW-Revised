#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/snapshot.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace eawr::script::authoritative {

// FD-71/72: an owned scoring instance, visited on the simulation thread before
// mode work. Absence means no gate write; a retained hold survives that visit.
// A campaign owner may supply its retained instance and synchronization clock.
class TacticalScoring {
public:
    virtual ~TacticalScoring() = default;
    [[nodiscard]] virtual core::Result<std::optional<bool>> visit(
        const sim::tactical::TacticalSnapshot&) = 0;
    [[nodiscard]] virtual core::Result<std::string> save() = 0;
    [[nodiscard]] virtual core::Result<void> load(std::string_view) = 0;
};

} // namespace eawr::script::authoritative
