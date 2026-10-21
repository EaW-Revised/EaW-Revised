#pragma once

#include "planet_hud.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace eawr::viewer::galactic {
enum class ReferenceTab { log, history, tree, summary };
enum class HistoryMetric { economy, military, planets };
enum class SummaryKind { production, economy, fleets };
enum class SummarySort { name, fronts, upgrades, capacity, total, basic, trade, mining };
struct ReferenceIcon {
    std::string icon, name, description;
    std::uint64_t type{}, count{1};
};
struct HistorySample {
    std::uint64_t frame{}, planets{}, technology{};
    double economy{}, military{};
};
struct HistoryFaction {
    std::uint32_t player{};
    std::string name;
    Rgb colour;
    std::vector<HistorySample> samples;
};
struct HistoryFacts {
    std::vector<HistoryFaction> factions;
    std::uint64_t fiscal_frames{}, planetary_objects{};
    std::uint32_t local_player{};
    bool multiplayer{};
    std::optional<sim::galactic::HistorySnapshot> published;
};
struct ChartLine { std::string name; Rgb colour; std::vector<std::array<double, 2>> points; };
struct HistoryView {
    bool available{};
    double maximum{1};
    std::uint64_t end_cycle{}, middle_cycle{};
    std::vector<ChartLine> lines;
};
[[nodiscard]] HistoryView history_view(const HistoryFacts&, HistoryMetric);
[[nodiscard]] std::optional<HistoryFacts> published_history(const sim::galactic::GalacticSnapshot&,
    const GalaxySnapshotStub&);
struct TreeSlot {
    std::string control;
    ReferenceIcon type;
    std::int64_t required_tech{};
    bool affiliated{}, initially_locked{}, unlocked{};
};
struct TreeFacts {
    std::string land_dialog, space_dialog;
    bool combined{};
    std::optional<std::int64_t> technology;
    std::vector<TreeSlot> land, space;
};
struct TreeViewSlot { std::string control; ReferenceIcon type; bool dimmed{}; };
[[nodiscard]] std::vector<TreeViewSlot> tree_view(const TreeFacts&, bool space);
// GREF-04: the atlas image an authored arrow gadget takes; empty when the id is not an arrow slot.
[[nodiscard]] std::string_view tree_arrow_texture(std::string_view gadget_id);
struct MiningModifier { double income{}; bool mining_facility{}; ReferenceIcon source; };
struct SummaryPlanet {
    std::uint64_t planet{};
    std::string name, icon;
    std::uint32_t owner{};
    bool authorized{};
    std::optional<double> basic, trade, capacity;
    std::optional<std::vector<MiningModifier>> modifiers;
    std::array<std::optional<ProductionItem>, 2> fronts; // space, ground
    std::optional<std::vector<ReferenceIcon>> upgrades;
    // GREF-15: provider has already admitted immediate allied contents.
    // Heroes precede type-key groups; these vectors retain provider order.
    std::optional<std::vector<ReferenceIcon>> ground;
    std::array<std::optional<std::vector<ReferenceIcon>>, 3> orbit;
};
struct EconomyRow {
    std::optional<std::int64_t> basic, trade, mining, total;
    std::size_t mines{};
    ReferenceIcon mine;
};
[[nodiscard]] EconomyRow economy_row(const SummaryPlanet&);
struct FleetRow { std::size_t planet_index{}; bool first{}; std::vector<ReferenceIcon> icons; };
struct SummaryView {
    std::vector<SummaryPlanet> planets;
    std::vector<FleetRow> fleet_rows;
    EconomyRow totals;
    std::optional<double> capacity;
};
struct LegendEntry { ReferenceIcon icon; std::string text; Rgb colour; };
struct ReferenceFacts {
    std::uint32_t local_player{};
    std::optional<HistoryFacts> history;
    TreeFacts tree;
    std::vector<SummaryPlanet> planets;
    std::vector<LegendEntry> legend, weather;
    std::string droid_icon;
    std::vector<LegendEntry> log;
    std::optional<std::array<bool, 3>> hints;
};
struct ReferenceType {
    ReferenceIcon icon;
    bool hero{}, hidden{}, container{}, mining_facility{};
    std::int64_t capacity{};
    bool gui_visibility_known{};
};
[[nodiscard]] std::vector<SummaryPlanet> published_summary_planets(
    const sim::galactic::GalacticSnapshot&, std::uint32_t local_player,
    const std::map<ObjectId, PlanetHudFacts>& admitted,
    const std::map<std::uint64_t, ReferenceType>& types,
    const std::map<ObjectId, ReferenceType>& planet_types);
// All inputs are copied presentation facts; absence never becomes a known zero.
class ReferencePagesModel final {
public:
    ReferenceTab tab{ReferenceTab::log};
    HistoryMetric metric{HistoryMetric::economy};
    SummaryKind kind{SummaryKind::production};
    bool visible{}, space{};
    std::size_t offset{};
    SummarySort sort{SummarySort::name};
    bool ascending{true};
    ReferenceFacts facts;
    SummaryView summary;
    void publish(ReferenceFacts);
    void open();
    void choose(ReferenceTab);
    void category(SummaryKind);
    void order(SummarySort);
    void scroll(std::size_t);
    void step(bool forward);
    [[nodiscard]] std::size_t maximum_offset() const;
    [[nodiscard]] std::optional<std::uint64_t> target(std::size_t visible_row, bool locked) const;
private:
    void rebuild();
};
} // namespace eawr::viewer::galactic
