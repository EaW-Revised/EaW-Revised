#include "reference_pages.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace eawr::viewer::galactic {
std::optional<HistoryFacts> published_history(const sim::galactic::GalacticSnapshot& snapshot,
    const GalaxySnapshotStub& metadata) {
    const auto* published = snapshot.history();
    if (!published) return {};
    HistoryFacts out; out.local_player = snapshot.view_player();
    out.published = *published; out.fiscal_frames = published->fiscal_frames;
    out.planetary_objects = published->planetary_objects;
    for (const auto& line : published->lines) {
        const auto at = std::find_if(metadata.players.begin(), metadata.players.end(),
            [&](const auto& player) { return player.id == line.player; });
        if (at == metadata.players.end()) continue;
        HistoryFaction faction{line.player, at->faction, {at->colour[0] / 255.F, at->colour[1] / 255.F, at->colour[2] / 255.F}, {}};
        for (const auto& row : line.samples) faction.samples.push_back({row.frame, row.planets,
            static_cast<std::uint64_t>(row.technology), std::bit_cast<float>(row.economy.bits), std::bit_cast<float>(row.military.bits)});
        out.factions.push_back(std::move(faction));
    }
    return out;
}
HistoryView history_view(const HistoryFacts& facts, HistoryMetric metric) {
    HistoryView view;
    if (facts.fiscal_frames == 0 || facts.factions.empty()) return view;
    std::uint64_t first = std::numeric_limits<std::uint64_t>::max(), last{};
    double maximum = metric == HistoryMetric::planets ? static_cast<double>(facts.planetary_objects) : 0;
    auto value = [metric](const HistorySample& row) {
        return metric == HistoryMetric::economy ? row.economy : metric == HistoryMetric::military
            ? row.military : static_cast<double>(row.planets);
    };
    if (facts.published) {
        if (!facts.published->available) return view;
        first = facts.published->first_frame; last = facts.published->last_frame;
        maximum = metric == HistoryMetric::economy ? std::bit_cast<float>(facts.published->maximum_economy.bits)
            : metric == HistoryMetric::military ? std::bit_cast<float>(facts.published->maximum_military.bits)
            : static_cast<double>(std::max(facts.published->maximum_planets, facts.published->planetary_objects));
    }
    for (const auto& faction : facts.factions) {
        if (faction.samples.size() < 2) return view; // GREF-01
        if (!facts.published) for (const auto& sample : faction.samples) {
            first = std::min(first, sample.frame); last = std::max(last, sample.frame);
            maximum = std::max(maximum, value(sample));
        }
    }
    view.maximum = maximum > 0 ? maximum : 1;
    view.end_cycle = last / facts.fiscal_frames + 1;
    view.middle_cycle = view.end_cycle / 2;
    for (const auto& faction : facts.factions) {
        if (facts.multiplayer && faction.player != facts.local_player) continue;
        ChartLine line{faction.name, faction.colour, {}};
        for (const auto& row : faction.samples) line.points.push_back({last == first ? 0 :
            static_cast<double>(row.frame - first) / static_cast<double>(last - first), value(row) / view.maximum});
        view.lines.push_back(std::move(line));
    }
    view.available = true;
    return view;
}
std::vector<TreeViewSlot> tree_view(const TreeFacts& facts, bool space) {
    std::vector<TreeViewSlot> result;
    for (const auto& slot : space && !facts.combined ? facts.space : facts.land)
        if (slot.affiliated) result.push_back({slot.control, slot.type,
            !facts.technology || slot.required_tech > *facts.technology || (slot.initially_locked && !slot.unlocked)});
    return result;
}
std::string_view tree_arrow_texture(std::string_view gadget_id) {
    // Arrow slots are an id family plus a decimal index below one hundred.
    static constexpr std::array<std::array<std::string_view, 2>, 4> families{{
        {"IDC_RIGHT_ARROW", "i_tree_arrow_right.tga"}, {"IDC_DRIGHT_ARROW", "i_tree_arrow_2right.tga"},
        {"IDC_RRIGHT_ARROW", "i_tree_arrow_3right.tga"}, {"IDC_DOWN_ARROW", "i_tree_arrow_down.tga"}}};
    for (const auto& [prefix, texture] : families) {
        if (!gadget_id.starts_with(prefix)) continue;
        const auto index = gadget_id.substr(prefix.size());
        const bool decimal = !index.empty() && index.size() <= 2 && (index.size() == 1 || index[0] != '0')
            && std::all_of(index.begin(), index.end(), [](char c) { return c >= '0' && c <= '9'; });
        return decimal ? texture : std::string_view{};
    }
    return {};
}
EconomyRow economy_row(const SummaryPlanet& planet) {
    EconomyRow row;
    if (planet.basic) row.basic = static_cast<std::int64_t>(std::round(*planet.basic));
    if (planet.trade) row.trade = static_cast<std::int64_t>(std::round(*planet.trade));
    if (planet.modifiers) {
        row.mining = 0;
        for (const auto& modifier : *planet.modifiers) if (modifier.mining_facility && modifier.income != 0) {
            if (row.mines++ == 0) row.mine = modifier.source;
            *row.mining += static_cast<std::int64_t>(std::round(modifier.income));
        }
    }
    if (row.basic && row.trade && row.mining) row.total = *row.basic + *row.trade + *row.mining;
    return row;
}
void ReferencePagesModel::publish(ReferenceFacts value) {
    facts = std::move(value); rebuild();
    // GREF-22: construction clamps against planets, unlike the rendered scrollbar.
    if (kind == SummaryKind::fleets) offset = std::min(offset,
        summary.planets.size() > 10 ? summary.planets.size()-10 : 0);
}
void ReferencePagesModel::open() { visible = true; if (tab == ReferenceTab::summary) category(SummaryKind::production); }
void ReferencePagesModel::choose(ReferenceTab value) {
    tab = value; visible = true;
    if (tab == ReferenceTab::summary) category(SummaryKind::production);
}
void ReferencePagesModel::category(SummaryKind value) {
    kind = value; offset = 0; sort = SummarySort::name; ascending = true; rebuild();
}
void ReferencePagesModel::order(SummarySort value) {
    ascending = sort == value ? !ascending : value == SummarySort::name;
    sort = value; rebuild();
}
void ReferencePagesModel::rebuild() {
    summary = {};
    for (const auto& planet : facts.planets)
        if (planet.authorized && planet.owner == facts.local_player) summary.planets.push_back(planet);
    const auto key = [this](const SummaryPlanet& planet) -> std::optional<double> {
        const auto row = economy_row(planet);
        switch (sort) {
        case SummarySort::fronts: return static_cast<double>(planet.fronts[0].has_value() + planet.fronts[1].has_value());
        case SummarySort::upgrades: return planet.upgrades ? std::optional(static_cast<double>(planet.upgrades->size())) : std::nullopt;
        case SummarySort::capacity: return planet.capacity;
        case SummarySort::basic: return row.basic ? std::optional(static_cast<double>(*row.basic)) : std::nullopt;
        case SummarySort::trade: return row.trade ? std::optional(static_cast<double>(*row.trade)) : std::nullopt;
        case SummarySort::mining: return row.mining ? std::optional(static_cast<double>(*row.mining)) : std::nullopt;
        case SummarySort::total: return row.total ? std::optional(static_cast<double>(*row.total)) : std::nullopt;
        default: return {};
        }
    };
    std::stable_sort(summary.planets.begin(), summary.planets.end(), [&](const auto& a, const auto& b) {
        if (sort == SummarySort::name) return ascending ? a.name < b.name : b.name < a.name;
        const auto left = key(a), right = key(b);
        if (left.has_value() != right.has_value()) return left.has_value();
        if (!left) return false;
        return ascending ? *left < *right : *right < *left;
    });
    summary.totals.basic = summary.totals.trade = summary.totals.mining = summary.totals.total = 0;
    summary.capacity = 0;
    auto add = [](auto& total, const auto& value) { if (total && value) *total += *value; else total.reset(); };
    for (std::size_t index = 0; index < summary.planets.size(); ++index) {
        const auto& planet = summary.planets[index];
        const auto row = economy_row(planet);
        add(summary.totals.basic, row.basic); add(summary.totals.trade, row.trade);
        add(summary.totals.mining, row.mining); add(summary.totals.total, row.total);
        add(summary.capacity, planet.capacity);
        bool first = true;
        auto append = [&](const auto& icons) {
            if (!icons || icons->empty()) return;
            FleetRow fleet{index, first, *icons}; first = false;
            if (fleet.icons.size() > 15) fleet.icons.resize(15); // GREF-10, no overflow rows
            summary.fleet_rows.push_back(std::move(fleet));
        };
        append(planet.ground);
        for (const auto& orbit : planet.orbit) append(orbit);
        if (first) summary.fleet_rows.push_back({index, true, {}});
    }
    offset = std::min(offset, maximum_offset());
}
std::size_t ReferencePagesModel::maximum_offset() const {
    const auto count = kind == SummaryKind::fleets ? summary.fleet_rows.size() : summary.planets.size();
    const std::size_t window = kind == SummaryKind::fleets ? 10 : 6;
    return count > window ? count - window : 0;
}
void ReferencePagesModel::scroll(std::size_t value) { offset = std::min(value, maximum_offset()); }
void ReferencePagesModel::step(bool forward) {
    const auto count = summary.planets.size();
    const std::size_t window = kind == SummaryKind::fleets ? 10 : 6;
    const auto bound = count > window ? count - window : 0; // GREF-22 callback seam
    offset = forward ? std::min(offset + 1, bound) : (offset > 0 ? offset - 1 : 0);
}
std::optional<std::uint64_t> ReferencePagesModel::target(std::size_t row, bool locked) const {
    // GREF-23 deliberately indexes planets, including in the expanded fleet view.
    if (locked || row >= (kind == SummaryKind::fleets ? 10U : 6U)
        || offset + row >= summary.planets.size()) return {};
    return summary.planets[offset + row].planet;
}
} // namespace eawr::viewer::galactic
