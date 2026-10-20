#pragma once

#include "eawr/sim/tactical/snapshot.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace eawr::data { struct EffectiveObject; }

namespace eawr::presentation::ui {

// Battle-neutral snapshot seam. The caller owns the snapshot for the duration of refresh().
struct TeamResearchView {
    std::span<const sim::tactical::EconomyView> ledgers;
    std::span<const sim::tactical::SnapshotPlayer> players;
    sim::tactical::PlayerId local{};
    std::uint64_t completed_tick{};
};

// WPR-70..77: capture-derived choices kept outside simulation state.
struct TeamDockPolicy {
    bool require_teammate{false};
    bool include_local{true};
    bool include_waiting{true};
    bool include_level_ups{true};
    bool completed_first{true};
    bool takes_pointer{true};
    float right_margin{0.0F};
    float row_step{38.0F};
    float top{60.0F}; // reference height 768; component Offset consumer is unknown
};

struct TeamDockRow {
    sim::tactical::PlayerId owner{};
    sim::tactical::TypeId type{};
    sim::EntityId station{};
    std::uint64_t entry_id{};
    bool in_progress{};
    bool starbase{};
    std::uint32_t percent{100};
    std::uint64_t display_order{}; // retained first-observed order, outside simulation state
    std::uint64_t completion_order{}; // first observed completion, for the completed group
    friend bool operator==(const TeamDockRow&, const TeamDockRow&) = default;
};

struct TeamResearchType {
    sim::tactical::TypeId type{};
    bool while_building{};
    bool when_complete{};
    bool level_up{};
};

// HCB-30: static, inherited presentation flags; never added to simulation content.
[[nodiscard]] TeamResearchType team_research_type(sim::tactical::TypeId type, bool level_up,
    const data::EffectiveObject& object);

// Reads only research ledgers and relationship metadata. Storage survives refreshes;
// an unchanged visible result performs no allocations or writes to the retained rows.
class TeamDockModel final {
public:
    [[nodiscard]] bool refresh(const TeamResearchView& view,
        std::span<const TeamResearchType> types, const TeamDockPolicy& policy = {});
    [[nodiscard]] std::span<const TeamDockRow> rows() const noexcept { return rows_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] bool team_game() const noexcept { return team_game_; }
private:
    std::vector<TeamDockRow> rows_;
    std::vector<std::uint8_t> seen_;
    std::uint64_t next_display_order_{1};
    std::uint64_t revision_{};
    bool team_game_{};
    bool completed_first_{true};
};

} // namespace eawr::presentation::ui
