#pragma once

#include "battle_source.hpp"
#include "live_session_options.hpp"
#include "eawr/battle/arrival_plan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::platform { struct LiveScripts; class BattleScoring; }

namespace eawr::presentation::godot_backend {

struct BattleDrawBindings;

enum class LocalStartKind { m2, selected, attract };

// The source owns these setup-time values while preparing. Both success and
// failure return them to the view, preserving the original partial preparation.
struct BattleSourceState final {
    explicit BattleSourceState(const live_session::Options& options);
    ~BattleSourceState();
    const live_session::Options& options_;
    std::optional<battle::BattleStart> start_;
    // The plan the source selected (T11); empty when the battle has no start or tables.
    std::optional<battle::ArrivalPlan> arrivals_;
    std::shared_ptr<const platform::LiveScripts> ai_scripts_;
    std::vector<sim::tactical::PlayerId> ai_players_;
    std::optional<sim::tactical::TacticalSetup> setup_;
    std::optional<sim::tactical::TacticalReplay> replay_;
    std::optional<units::UnitTables> tables_;
    std::optional<skirmish::SessionContent> content_;
    sim::tactical::VictoryRules victory_;
    sim::tactical::EconomyRules economy_;
    std::unique_ptr<BattleDrawBindings> draw_bindings_;
    std::vector<std::uint32_t> session_records_;
    std::vector<sim::EntityId> unfogged_map_props_;
    std::map<sim::EntityId, std::size_t> ship_of_entity_;
    std::map<sim::EntityId, sim::tactical::PlayerId> owner_of_entity_;
    std::map<sim::tactical::PlayerId, sim::tactical::TeamId> team_of_player_;
    std::array<std::array<float, 3>, 2> preview_colours_{};
    std::shared_ptr<platform::BattleScoring> scoring_;
    sim::tactical::PlayerId player_{};
    bool colorization_enabled_{true};

    // Called last by every source: start_, tables_, economy_ and setup_ are final here.
    void select_arrivals();
    [[nodiscard]] bool prepare_local(const vfs::Vfs&, const data::Catalog&, std::string&, LocalStartKind);
    [[nodiscard]] bool prepare_borrowed(std::string&);
    [[nodiscard]] bool prepare_native(const live_session::PreparedBattle&, std::string&);
    [[nodiscard]] std::optional<sim::tactical::TacticalReplay> read_replay(std::string&);
    [[nodiscard]] std::optional<sim::tactical::TacticalReplay> build_melee(const vfs::Vfs&, const data::Catalog&,
        std::optional<units::UnitTables>&, std::string&);
    [[nodiscard]] bool bind_replay(const vfs::Vfs&, const data::Catalog&, std::string_view,
        std::optional<sim::tactical::TacticalReplay>, std::optional<units::UnitTables>, std::string&);
};

class LocalSkirmishSource final : public BattleSource {
public:
    explicit LocalSkirmishSource(LocalStartKind kind);
    [[nodiscard]] BattleAiCapability ai_capability() const noexcept override;
    [[nodiscard]] bool prepare(BattlePreparation&, const vfs::Vfs&, const data::Catalog&,
        std::string_view, std::string&) const override;
private:
    LocalStartKind kind_;
};

class AttractSource final : public BattleSource {
public:
    [[nodiscard]] BattleAiCapability ai_capability() const noexcept override;
    [[nodiscard]] bool prepare(BattlePreparation&, const vfs::Vfs&, const data::Catalog&,
        std::string_view, std::string&) const override;
};

class ReplaySource final : public BattleSource {
public:
    [[nodiscard]] BattleAiCapability ai_capability() const noexcept override;
    [[nodiscard]] bool prepare(BattlePreparation&, const vfs::Vfs&, const data::Catalog&,
        std::string_view, std::string&) const override;
};

class MeleeSource final : public BattleSource {
public:
    [[nodiscard]] BattleAiCapability ai_capability() const noexcept override;
    [[nodiscard]] bool prepare(BattlePreparation&, const vfs::Vfs&, const data::Catalog&,
        std::string_view, std::string&) const override;
};

class LanSource final : public BattleSource {
public:
    explicit LanSource(LocalStartKind kind);
    [[nodiscard]] BattleAiCapability ai_capability() const noexcept override;
    [[nodiscard]] bool prepare(BattlePreparation&, const vfs::Vfs&, const data::Catalog&,
        std::string_view, std::string&) const override;
private:
    LocalStartKind kind_;
};

class BorrowedBattleSource final : public BattleSource {
public:
    [[nodiscard]] BattleAiCapability ai_capability() const noexcept override;
    [[nodiscard]] bool prepare(BattlePreparation&, const vfs::Vfs&, const data::Catalog&,
        std::string_view, std::string&) const override;
};

} // namespace eawr::presentation::godot_backend
