#pragma once

#include "eawr/campaign/setup_plan.hpp"
#include "../script/foc/galactic_ai_actions.hpp"
#include "../script/foc/galactic_ai_world.hpp"
#include "eawr/sim/galactic/graph.hpp"

namespace eawr::campaign {
// Immutable campaign identities; each fresh/load runtime owns a distinct cache.
struct GalacticAiUsabilityContent {
    std::vector<script::foc::ai::GalacticPlayerFacts> players;
    std::vector<sim::galactic::PlanetId> planet_order;
    bool uses_fog{};
    std::uint32_t logical_fps{};
};
[[nodiscard]] core::Result<std::shared_ptr<const GalacticAiUsabilityContent>>
prepare_stock_galactic_ai_usability(const data::galactic::GalacticData&, const CampaignSetupPlan&,
    const script::foc::ai::GalacticPerceptionSetup&);

// Exact inputs and results of one player's last connecting-path pass (derived, never archived or hashed).
// The pass is a pure function of these inputs; an equal key replays its three connector sets.
struct GalacticAiIslandMemo {
    std::vector<sim::galactic::PlanetId> planets, representatives;
    std::vector<std::uint8_t> access;                          // relation | usable << 2, per planet
    std::vector<std::array<std::uint64_t, 3>> force;           // planet, space bits, ground bits
    std::vector<script::foc::ai::SharedPlanetList> goals;      // policy-0 site list per representative
    std::vector<std::size_t> sizes;                            // policy-1 site count per representative
    std::vector<sim::galactic::GraphNode> nodes;               // graph and route inputs, stored with a result only
    std::optional<sim::math::Fixed> largest_factor;
    std::vector<sim::galactic::Route> routes;
    std::set<sim::galactic::PlanetId> shortest, least, largest;
};

class GalacticAiUsabilityProvider final : public script::foc::galactic_ai::TrackedStateContributor {
public:
    using World = script::foc::ai::GalacticWorld;
    using Engine = script::foc::ai::Engine;
    using MovementInput = script::foc::galactic_ai::NativeActions::MovementInput;
    explicit GalacticAiUsabilityProvider(std::shared_ptr<const GalacticAiUsabilityContent>);
    [[nodiscard]] core::Result<bool> usability(sim::galactic::PlayerId,
        const sim::galactic::GalacticSnapshot&, sim::galactic::PlanetId) const;
    [[nodiscard]] core::Result<MovementInput> movement(sim::galactic::PlayerId,
        const sim::galactic::GalacticSnapshot&, const World&, const Engine&, const sim::PartitionExecutor&);
    [[nodiscard]] core::Result<script::foc::ai::GalacticConnectivity> connectivity(
        const sim::galactic::GalacticSession&, World&, const Engine&, const sim::PartitionExecutor&);
    using ConnectivityPublication = std::shared_ptr<const script::foc::ai::GalacticConnectivity>;
    [[nodiscard]] core::Result<ConnectivityPublication> connectivity_publication(
        const sim::galactic::GalacticSession&, World&, const Engine&, const sim::PartitionExecutor&);
    // #2676/WGA-185: an ability producer must resolve the live fleet predicate
    // before enabling stealth; this bounded provider cannot fabricate that owner.
    void refuse_live_stealth() noexcept { live_stealth_unavailable_ = true; }
    void refuse_story_path_inputs() noexcept { story_path_unavailable_ = true; }
    [[nodiscard]] core::Result<script::foc::galactic_ai::NativeState> capture() const override;
    [[nodiscard]] core::Result<void> restore(const script::foc::galactic_ai::NativeState&) override;
    [[nodiscard]] core::Result<std::string> state_hash() const override;
    [[nodiscard]] core::Result<std::string> record_sync_hash(bool cold = false) const override;
    [[nodiscard]] std::size_t record_byte_upper_bound() const noexcept override { return records_.byte_size() + prepared_through_.size() * 13 + 1024; }
    // Pending invalidation also depends on upstream writers after our last read.
    [[nodiscard]] std::optional<std::uint64_t> state_revision() const noexcept override { return std::nullopt; }
    struct Work { std::uint64_t generations{}, planets{}, searches{}, edges{}, stealth_preparations{}, site_searches{},
        island_reuses{}, access_reuses{}; };
    [[nodiscard]] Work work() const noexcept { return work_; }
private:
    [[nodiscard]] core::Result<void> prepare_stealth(const sim::galactic::GalacticSnapshot&);
    // Bound by connectivity to the same live owners that use this contributor.
    const sim::galactic::GalacticSession* observed_session_{};
    const World* observed_world_{};
    std::optional<std::uint64_t> stealth_generation_;
    std::set<sim::EntityId> stealth_fleets_; // derived from the saved world owner, never archived twice
    struct Cache {
        std::uint64_t observer_generation{}, access_generation{}, perception_revision{};
        script::numeric::LuaNumber valid_through;
        MovementInput movement;
        // Player-owned pure lists, indexed by (target, policy). Their reuse does
        // not service the retained list/stamp cadence or alter archive bytes.
        const sim::galactic::PlanetGraph* site_graph{};
        std::vector<std::array<std::shared_ptr<const std::vector<sim::galactic::PlanetId>>, 6>> site_memos{};
        std::map<sim::galactic::PlanetId, std::pair<script::numeric::LuaNumber, script::numeric::LuaNumber>> force;
        std::vector<script::foc::ai::GalacticGoalReach> goal_reach;
        std::vector<script::foc::ai::GalacticConnectivity::Island> islands;
        std::shared_ptr<const GalacticAiIslandMemo> island_memo; // carried across refreshes of this player
        std::array<std::int32_t, 2> island_sizes{-1, -1}; // WGA-476, derived from the retained site lists
        bool connectivity_ready{};
        bool source_changed{};
        std::uint64_t connectivity_through{};
    };
    [[nodiscard]] core::Result<const Cache*> refresh(sim::galactic::PlayerId,
        const sim::galactic::GalacticSnapshot&, const World&, const Engine&, const sim::PartitionExecutor&,
        const script::foc::ai::DerivedExecutionRestore* = nullptr, bool force = false);
    std::shared_ptr<const GalacticAiUsabilityContent> content_;
    std::map<sim::galactic::PlayerId, Cache> caches_;
    // Retain the consumed expiry, not a later reader's refreshed World clock.
    struct PreparedInput {
        script::numeric::LuaNumber through{};
        std::uint8_t pending_changes{}; // bit 0: input refresh; bit 1: site publication
        bool operator==(const PreparedInput&) const = default;
    };
    std::map<sim::galactic::PlayerId, PreparedInput> prepared_through_;
    ConnectivityPublication published_{std::make_shared<const script::foc::ai::GalacticConnectivity>()};
    struct Reach {
        std::array<script::foc::ai::SharedPlanetList, 6> sites;
        std::array<std::optional<std::uint64_t>, 6> frames;
        bool shortest{}, least_force{}, largest{};
        bool operator==(const Reach&) const = default;
    };
    std::map<std::pair<sim::galactic::PlayerId, sim::galactic::PlanetId>, Reach> retained_;
    [[nodiscard]] std::vector<std::uint8_t> reach_record(const std::pair<sim::galactic::PlayerId, sim::galactic::PlanetId>&, const Reach&) const;
    void update_reach_records(const decltype(retained_)&);
    [[nodiscard]] core::RecordDigest rebuild_records() const;
    core::RecordDigest records_;
    bool reconstructed_{};
    std::uint64_t publication_revision_{1};
    bool live_stealth_unavailable_{};
    bool story_path_unavailable_{};
    Work work_;
};
} // namespace eawr::campaign
