#include "eawr/skirmish/roster_gate.hpp"
#include <algorithm>
#include <array>
#include "eawr/assets/map.hpp"

namespace eawr::skirmish {
namespace {
#include "roster_gate_data.hpp"
// Fold while comparing: no allocation or locale-dependent identity.
[[nodiscard]] int compare(std::string_view a, std::string_view b) noexcept {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        const char left = lower(a[i]);
        const char right = lower(b[i]);
        if (left != right) return left < right ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}
} // namespace
std::span<const DisabledUnit> roster_disabled_units() noexcept { return disabled_units; }
const sim::tactical::TypeFlags& roster_disabled_types() {
    static const sim::tactical::TypeFlags flags = [] {
        std::vector<sim::tactical::TypeId> ids;
        for (const auto& row : disabled_units) ids.push_back(assets::object_type_crc(row.unit));
        std::sort(ids.begin(), ids.end());
        return sim::tactical::TypeFlags(std::move(ids));
    }();
    return flags;
}
std::string_view roster_disabled_reason(const std::string_view unit) noexcept {
    const auto found = std::lower_bound(disabled_units.begin(), disabled_units.end(), unit,
        [](const DisabledUnit& row, std::string_view name) { return compare(row.unit, name) < 0; });
    return found != disabled_units.end() && compare(found->unit, unit) == 0 ? found->tooltip : std::string_view{};
}
std::string_view roster_ability_reason(const std::string_view unit, const std::string_view ability) noexcept {
    const auto found = std::lower_bound(disabled_abilities.begin(), disabled_abilities.end(), unit,
        [](const DisabledAbility& row, std::string_view name) { return compare(row.unit, name) < 0; });
    for (auto row = found; row != disabled_abilities.end() && compare(row->unit, unit) == 0; ++row) {
        if (compare(row->ability, ability) == 0) return row->reason;
    }
    return {};
}
} // namespace eawr::skirmish
