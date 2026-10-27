#pragma once

#include "eawr/campaign/load_notes.hpp"
#include "eawr/sim/galactic/types.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace eawr::campaign {
// These limits describe our representation, not restrictions on retail XML.
// Only the owned authoring copy passes here; saves and replay input stay strict.
inline void setup_load_note(core::LoadNoteBuffer& notes, std::string_view code,
    std::string_view object, std::string_view tag, std::string authored, std::string effective,
    core::LoadAction action = core::LoadAction::clamped) {
    core::LoadNote note;
    note.code = code; note.object = object; note.tag = tag;
    note.classification = core::LoadCategory::unknown_degrade;
    note.authored_value = std::move(authored); note.effective_value = std::move(effective);
    note.action = action; notes.append(std::move(note));
}

inline void normalize_campaign_fiscal(sim::galactic::FiscalSetup& fiscal, std::size_t planets,
    core::LoadNoteBuffer& notes, std::string_view campaign) {
    using Fixed = sim::math::Fixed;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    std::int64_t sum{};
    for (auto& row : fiscal.planets) {
        const auto authored = row.credits.raw();
        auto effective = authored;
        if (sum > 0 && authored > maximum - sum) effective = maximum - sum;
        if (sum < 0 && authored < minimum - sum) effective = minimum - sum;
        if (effective != authored) setup_load_note(notes, "EAWR-LOAD-1020", campaign,
            "Planet_Credit_Value", std::to_string(authored), std::to_string(effective));
        row.credits = Fixed::from_raw(effective); sum += effective;
    }
    const auto count = static_cast<std::int64_t>(std::max(std::size_t{1}, planets));
    const auto cap = std::clamp(fiscal.credit_cap_per_planet.raw(), std::int64_t{0}, maximum / count);
    if (cap != fiscal.credit_cap_per_planet.raw()) setup_load_note(notes, "EAWR-LOAD-1021", campaign,
        "Credit_Cap_Per_Planet", std::to_string(fiscal.credit_cap_per_planet.raw()), std::to_string(cap));
    fiscal.credit_cap_per_planet = Fixed::from_raw(cap);
    for (auto& player : fiscal.players) {
        const auto authored = player.positive_credit_multiplier.raw();
        auto effective = std::max(std::int64_t{0}, authored);
        Fixed product;
        if (sum > 0 && !sim::math::try_multiply(Fixed::from_raw(sum), Fixed::from_raw(effective), product)) {
            // Monotonic search avoids cross-width arithmetic on MSVC and retains
            // the greatest representable difficulty factor.
            std::int64_t low{}, high = effective;
            while (low < high) {
                const auto middle = low + (high - low) / 2 + 1;
                if (sim::math::try_multiply(Fixed::from_raw(sum), Fixed::from_raw(middle), product)) low = middle;
                else high = middle - 1;
            }
            effective = low;
        }
        if (effective != authored) setup_load_note(notes, "EAWR-LOAD-1022", campaign,
            "Credit_Multiplier", std::to_string(authored), std::to_string(effective));
        player.positive_credit_multiplier = Fixed::from_raw(effective);
    }
}

inline void normalize_campaign_offers(sim::galactic::ProductionSetup& production,
    core::LoadNoteBuffer& notes, std::string_view campaign,
    std::size_t limit = sim::galactic::galactic_max_objects) {
    limit = std::min(limit, sim::galactic::galactic_max_objects);
    std::size_t remaining = limit;
    for (auto& planet : production.planets) {
        const auto keep = std::min(remaining, planet.offers.size());
        if (keep != planet.offers.size()) setup_load_note(notes, "EAWR-LOAD-1030", campaign,
            "Production_Offers", std::to_string(planet.offers.size()), std::to_string(keep), core::LoadAction::skipped_object);
        planet.offers.resize(keep); remaining -= keep;
    }
}

// Census membership is a counting graph. Remove unresolved links and the
// cyclic remainder (including parents that depend on it), preserving all other
// types. Return affected identities so placement/purchase can omit their templates.
inline std::set<sim::galactic::TypeId> normalize_campaign_census(sim::galactic::PopulationCensus& census,
    core::LoadNoteBuffer& notes, std::string_view campaign,
    std::size_t limit = sim::galactic::galactic_max_objects) {
    using namespace sim::galactic;
    using Fixed = sim::math::Fixed;
    limit = std::min(limit, galactic_max_objects);
    std::map<TypeId, std::size_t> pending;
    std::map<TypeId, std::vector<TypeId>> parents;
    for (const auto& row : census.types) pending.emplace(row.type, 0);
    std::set<TypeId> omitted, ready;
    std::size_t remaining = limit;
    for (auto& row : census.types) {
        const auto authored = row.members;
        std::erase_if(row.members, [&](auto id) { return !pending.contains(id); });
        std::sort(row.members.begin(), row.members.end());
        row.members.erase(std::unique(row.members.begin(), row.members.end()), row.members.end());
        const auto keep = std::min(remaining, row.members.size()); row.members.resize(keep); remaining -= keep;
        if (row.members != authored) omitted.insert(row.type);
        pending[row.type] = row.members.size();
        if (row.members.empty()) ready.insert(row.type);
        for (auto id : row.members) parents[id].push_back(row.type);
        const auto raw = std::clamp(row.population.raw(), std::int64_t{0}, std::int64_t{1} << 40);
        const auto capacity = std::clamp(row.additional_capacity,
            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()));
        if (raw != row.population.raw() || capacity != row.additional_capacity) {
            setup_load_note(notes, "EAWR-LOAD-1010", campaign, "Population_Value/Additional_Population_Capacity",
                std::to_string(row.population.raw()) + "/" + std::to_string(row.additional_capacity),
                std::to_string(raw) + "/" + std::to_string(capacity));
            row.population = Fixed::from_raw(raw); row.additional_capacity = capacity;
        }
    }
    while (!ready.empty()) {
        const auto id = *ready.begin(); ready.erase(ready.begin());
        for (auto parent : parents[id]) if (--pending[parent] == 0) ready.insert(parent);
    }
    for (auto& row : census.types) {
        if (pending[row.type]) { omitted.insert(row.type); row.members.clear(); }
        if (!row.company && row.transport) { omitted.insert(row.type); row.transport = 0; }
        if (row.company && (!pending.contains(row.transport) || row.transport == row.type
            || !std::binary_search(row.members.begin(), row.members.end(), row.transport))) {
            omitted.insert(row.type); row.company = false; row.transport = 0;
        }
        if (omitted.contains(row.type)) setup_load_note(notes, "EAWR-LOAD-1031", campaign,
            "Composite_Members", "unbound, cyclic or over-budget template " + std::to_string(row.type),
            "template omitted from creation; safe census links retained", core::LoadAction::skipped_object);
    }
    for (auto& player : census.players) {
        const auto effective = std::clamp(player.base_capacity, std::int64_t{0}, std::int64_t{1} << 24);
        if (effective != player.base_capacity) setup_load_note(notes, "EAWR-LOAD-1011", campaign,
            "Base_Population_Capacity", std::to_string(player.base_capacity), std::to_string(effective));
        player.base_capacity = effective;
    }
    return omitted;
}
} // namespace eawr::campaign
