#pragma once

#include <cstddef>
#include <string_view>

namespace eawr::presentation {

enum class ProxyOutcome { reserved, refused_budget, allocation_failed, invalid_recipe };
struct ProxyResult {
    ProxyOutcome outcome{ProxyOutcome::reserved};
    std::size_t bytes{};
    std::string_view reason{};
    std::size_t first_slot{};
};

} // namespace eawr::presentation
