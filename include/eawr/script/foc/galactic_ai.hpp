#pragma once

#include <cstdlib>

// WGA-01..05, GFD-A01..08: the retained manager consumes copied campaign
// inputs; queries never advance it. Action admission and world progress have
// separate boundaries. Missing capabilities fail explicitly.
#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/sim/galactic/ai_funding.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/production.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace eawr::script::foc::galactic_ai {
namespace gc = sim::galactic;
// Native encoders preserve Number.repr as an unsigned 64-bit binary64
// interchange word, little endian, without host floating-point conversion.
using Number = numeric::LuaNumber;
enum class Mode : std::uint8_t { none, galactic, space, land };
enum class Service : std::uint8_t { perception, goals, planning, execution, learning, templates };
inline constexpr std::size_t service_count = 6;
// GFC-A01..06: deterministic session configuration, in synchronization frames.
using ServicePeriods = std::array<std::uint64_t, service_count>;
inline constexpr ServicePeriods retail_service_periods{300, 1, 3, 3, 300, 1};
struct Clocks {
    std::uint64_t synchronization{};
    std::uint64_t mode{};
    bool operator==(const Clocks&) const = default;
};
struct Admission {
    Mode active_mode{Mode::galactic};
    bool processing{true};
    bool auto_resolve{};
    bool operator==(const Admission&) const = default;
};
struct TargetInput {
    gc::PlanetId planet{};
    gc::PlayerId owner{};
    sim::math::Vec3 position{};
    std::map<std::string, Number, std::less<>> tokens;
};
struct ProducerInput {
    sim::EntityId producer{};
    gc::PlanetId planet{};
    gc::PlayerId player{};
    gc::TypeId type{};
    gc::ProductionDomain domain{};
    gc::ProductionAdmission admission{gc::ProductionAdmission::unavailable};
    sim::math::Fixed cost{}, build_seconds{}, queue_seconds{}, travel_seconds{};
    std::uint32_t base_level{}, required_level{}, logical_fps{30};
    bool reachable{};
    Number distance{}, hyperspace_speed{}, faction_speed_factor{}, technology_weight{};
};
// Completed views share the large producer domain. Any fixture/producer write
// detaches first, so a retained earlier view remains immutable.
class ProducerInputs {
public:
    using Rows = std::vector<ProducerInput>;
    using Query = std::function<core::Result<Rows>(gc::PlayerId, gc::PlanetId, std::span<const gc::TypeId>, bool)>;
    ProducerInputs() : rows_(std::make_shared<std::vector<ProducerInput>>()) {}
    ProducerInputs(std::initializer_list<ProducerInput> rows)
        : rows_(std::make_shared<std::vector<ProducerInput>>(rows)) {}
    ProducerInputs& operator=(std::initializer_list<ProducerInput> rows) {
        lazy_.reset(); rows_ = std::make_shared<std::vector<ProducerInput>>(rows); return *this;
    }
    [[nodiscard]] auto begin() const { return readable().cbegin(); }
    [[nodiscard]] auto end() const { return readable().cend(); }
    [[nodiscard]] auto begin() { return writable().begin(); }
    [[nodiscard]] auto end() { return writable().end(); }
    [[nodiscard]] const ProducerInput* data() const { return readable().data(); }
    [[nodiscard]] std::size_t size() const { return readable().size(); }
    [[nodiscard]] bool empty() const { return readable().empty(); }
    [[nodiscard]] const ProducerInput& front() const { return readable().front(); }
    [[nodiscard]] ProducerInput& front() { return writable().front(); }
    [[nodiscard]] const ProducerInput& operator[](std::size_t index) const { return readable()[index]; }
    [[nodiscard]] ProducerInput& operator[](std::size_t index) { return writable()[index]; }
    void reserve(std::size_t size) { writable().reserve(size); }
    void push_back(ProducerInput row) { writable().push_back(std::move(row)); }
    void clear() { lazy_.reset(); rows_ = std::make_shared<Rows>(); }
    operator std::span<const ProducerInput>() const { return readable(); }
    void set_query(Query query) { rows_.reset(); lazy_ = std::make_shared<Lazy>(std::move(query)); }
    [[nodiscard]] core::Result<Rows> read(gc::PlayerId player, gc::PlanetId planet = 0,
        std::span<const gc::TypeId> types = {}, bool first_allowed = false) const {
        if (lazy_) return lazy_->query(player, planet, types, first_allowed);
        Rows result;
        for (const auto& row : readable()) {
            if ((player && row.player != player) || (planet && row.planet != planet)
                || (!types.empty() && std::find(types.begin(), types.end(), row.type) == types.end())) continue;
            if (first_allowed && row.admission != gc::ProductionAdmission::allowed) continue;
            result.push_back(row);
            if (first_allowed) break;
        }
        return core::Result<Rows>::success(std::move(result));
    }
private:
    struct Lazy {
        explicit Lazy(Query value) : query(std::move(value)) {}
        Query query;
        mutable std::once_flag materialize;
        mutable Rows rows;
    };
    const Rows& readable() const {
        if (rows_) return *rows_;
        std::call_once(lazy_->materialize, [&] {
            auto result = lazy_->query(0, 0, {}, false);
            if (!result) throw std::runtime_error(result.error().message);
            lazy_->rows = std::move(result).value();
        });
        return lazy_->rows;
    }
    std::vector<ProducerInput>& writable() {
        if (lazy_) { rows_ = std::make_shared<Rows>(readable()); lazy_.reset(); }
        if (rows_.use_count() != 1) rows_ = std::make_shared<std::vector<ProducerInput>>(*rows_);
        return *rows_;
    }
    std::shared_ptr<std::vector<ProducerInput>> rows_;
    std::shared_ptr<Lazy> lazy_;
};
struct WorldInput {
    Clocks clocks;
    std::vector<TargetInput> targets; // canonical planet order
    ProducerInputs producers; // canonical player/planet/domain/type order
    std::map<gc::PlayerId, sim::math::Fixed> credits;
};
struct QueryContext {
    gc::PlayerId self{};
    std::optional<gc::PlayerId> enemy, human;
    std::optional<gc::PlanetId> target, source;
    Mode manager{Mode::galactic};
};
class Queries {
public:
    virtual ~Queries() = default;
    [[nodiscard]] virtual std::shared_ptr<const WorldInput> copy_world() const = 0;
    [[nodiscard]] virtual core::Result<Number> equation(std::string_view, const QueryContext&) const = 0;
    [[nodiscard]] virtual std::optional<gc::PlanetId> containing_planet(sim::EntityId) const = 0;
};
enum class ActionKind : std::uint8_t { produce, move, invade, raid, land, launch, regroup, magic, give_money };
enum class MovementReach : std::uint8_t {
    friendly_only, friendly_ignore_threat, enemy_destination, enemy_undefended, single_hop_disconnected, any
};
struct ActionRequest {
    ActionKind kind{};
    gc::PlayerId player{};
    std::uint64_t plan{}, taskforce{}, block{};
    sim::EntityId object{};
    gc::PlanetId target{};
    gc::TypeId type{};
    Number amount{};
    MovementReach reach{MovementReach::friendly_only};
    std::vector<sim::EntityId> members; // retained TaskForce joining order
};
struct ActionReceipt {
    bool admitted{};
    std::uint64_t request{};
    // Admission does not imply completion or a coroutine resume (GFD-A05/07).
};
class Actions {
public:
    virtual ~Actions() = default;
    [[nodiscard]] virtual bool supports(ActionKind) const noexcept = 0;
    [[nodiscard]] virtual core::Result<ActionReceipt> admit(const ActionRequest&) = 0;
};
enum class NoticeKind : std::uint8_t { fiscal, windfall, marked, billed, cancelled, completed, battle_enter, battle_release, battle_return };
struct FundingNotice {
    authoritative::EventKey key;
    NoticeKind kind{};
    gc::PlayerId player{};
    gc::funding::GoalId goal{};
    sim::math::Fixed amount{}, actual_credits{}, interval_seconds{};
};
struct FundingTransaction {
    gc::PlayerId player{};
    gc::funding::GoalRequest request;
    bool activation{};
};
template<class T> struct Setting {
    T value{};
    int priority{};
    bool operator==(const Setting&) const = default;
};
struct TemplatePolicy {
    std::map<std::string, Setting<std::string>, std::less<>> equations;
    std::map<std::string, Setting<Number>, std::less<>> allowances;
    std::map<std::string, Setting<bool>, std::less<>> goal_categories, goal_types;
    std::map<std::string, Setting<bool>, std::less<>> plan_categories, plan_types, plan_names;
    bool operator==(const TemplatePolicy&) const = default;
};
class Funding {
public:
    virtual ~Funding() = default;
    [[nodiscard]] virtual core::Result<void> notify(const FundingNotice&) = 0;
    [[nodiscard]] virtual core::Result<bool> feasible(const FundingTransaction&) const = 0;
    [[nodiscard]] virtual core::Result<void> reserve(const FundingTransaction&) = 0;
    [[nodiscard]] virtual std::shared_ptr<const gc::funding::Ledger> ledger(gc::PlayerId) const = 0;
};
// F owns the whole-game container and fixup order. Each lane contributes a
// versioned component at the same completed boundary; no legacy reader.
// Component encoders use little-endian integer/raw Fixed/Number words, explicit
// length framing and canonical map-key order. Include the component domain and
// version in its bytes. Hashes are lowercase, 64-hex-digit SHA-256 digests of
// those canonical bytes; the whole-game owner composes them with Lua/world state.
struct NativeState {
    std::uint32_t version{1};
    std::map<std::string, std::vector<std::uint8_t>, std::less<>> components;
};
class StateContributor {
public:
    virtual ~StateContributor() = default;
    [[nodiscard]] virtual core::Result<NativeState> capture() const = 0;
    [[nodiscard]] virtual core::Result<void> restore(const NativeState&) = 0;
    [[nodiscard]] virtual core::Result<std::string> state_hash() const = 0;
    // Cheap per-tick composition. Unqualified contributors retain the full path.
    [[nodiscard]] virtual core::Result<std::string> sync_hash() const { return state_hash(); }
    // Empty opts an external contributor into canonical payload fingerprinting.
    // Native owners maintain record sums at their write boundaries. The cold
    // option reconstructs those sums independently for the per-tick oracle.
    [[nodiscard]] virtual core::Result<std::string> record_sync_hash(bool cold = false) const {
        static_cast<void>(cold); return core::Result<std::string>::success({});
    }
    // A conservative canonical payload size. Crossing the estimate triggers
    // exact capture validation; it never narrows the archive's accepted limit.
    [[nodiscard]] virtual std::size_t record_byte_upper_bound() const noexcept { return 0; }
    [[nodiscard]] core::Result<void> verify_record_sync_hash() const {
        bool verify{};
#ifndef NDEBUG
        verify = true;
#elif defined(_MSC_VER)
        char* option{}; std::size_t length{};
        if (_dupenv_s(&option, &length, "EAWR_VERIFY_GALACTIC_HASH") == 0 && option) verify = std::string_view(option) == "1";
        std::free(option);
#else
        const auto* option = std::getenv("EAWR_VERIFY_GALACTIC_HASH"); verify = option && std::string_view(option) == "1";
#endif
        if (!verify) return core::Result<void>::success();
        auto warm = record_sync_hash(); if (!warm) return core::Result<void>::failure(warm.error());
        if (warm.value().empty()) return core::Result<void>::success();
        auto cold = record_sync_hash(true); if (!cold) return core::Result<void>::failure(cold.error());
        if (warm.value() == cold.value()) return core::Result<void>::success();
        core::Diagnostic error; error.code = "EAWR-GCAI-STATE-0001"; error.message = "native record sum differs from independent cold reconstruction";
        return core::Result<void>::failure(std::move(error));
    }
    // Absent means external/untracked writes: the compositor must recapture.
    // Epochs are process-local work metadata, excluded from saves and hashes.
    [[nodiscard]] virtual std::optional<std::uint64_t> state_revision() const noexcept { return {}; }
};
class TrackedStateContributor : public StateContributor {
public:
    [[nodiscard]] std::optional<std::uint64_t> state_revision() const noexcept override { return revision_; }
    void mark_state_changed() const noexcept { if (++revision_ == 0) std::terminate(); }
private:
    mutable std::uint64_t revision_{1};
};
struct ServiceInput {
    gc::PlayerId player{};
    Service service{};
    Clocks clocks;
    std::shared_ptr<const WorldInput> world;
    std::shared_ptr<const TemplatePolicy> policy;
};
enum class PolicyWrite : std::uint8_t { initial, reversal, ordinary };
class Services {
public:
    virtual ~Services() = default;
    // Initialization installs template policy before initializing the ledger.
    [[nodiscard]] virtual core::Result<void> initialize(gc::PlayerId) = 0;
    // WGA-103/104: every departure clears category equations once, even if
    // surviving replay reproduces identical policy. The retained funding
    // consumer resets equation priorities and marks equations changed.
    [[nodiscard]] virtual core::Result<void> clear_equations(gc::PlayerId) = 0;
    // WGA-68: deliver the authored funding setters on immediate activation,
    // including equal values/priorities and rejected lower-priority writes.
    // Only equations/allowances are populated; selected context evaluation
    // belongs to the retained funding adapter. Switch-only activation omits it.
    [[nodiscard]] virtual core::Result<void> apply_funding_writes(gc::PlayerId, const TemplatePolicy&) = 0;
    // Initial/reversal policy supplies all final funding rows; ordinary policy
    // updates supply native goal/plan gates after the explicit setter writes.
    [[nodiscard]] virtual core::Result<void> apply_policy(gc::PlayerId, const TemplatePolicy&, PolicyWrite) = 0;
    [[nodiscard]] virtual core::Result<void> run(const ServiceInput&, authoritative::ScriptScheduler&) = 0;
    [[nodiscard]] virtual core::Result<void> finish_barrier(authoritative::ScriptScheduler&) { return core::Result<void>::success(); }
    [[nodiscard]] virtual core::Result<void> after_service(authoritative::ServiceReport&) { return core::Result<void>::success(); }
};
struct Capabilities {
    std::shared_ptr<const Queries> queries;
    std::shared_ptr<Actions> actions;
    std::shared_ptr<Funding> funding;
    std::shared_ptr<Services> services;
    std::shared_ptr<StateContributor> state;
    // Ordered barrier preparation refreshes the provider before copying rows.
    std::function<core::Result<void>(const sim::PartitionExecutor&, const gc::GalacticSession&, const gc::GalacticTick&)> prepare;
    // Explicit plan/freestore budget for the owner-thread native action set.
    // Reconstruct this runtime provider before attaching/resuming Lua instances.
    std::shared_ptr<authoritative::ImmediateEffectHost> immediate_effects;
    std::size_t immediate_instance_limit{};
};

struct PlayerSelection {
    gc::PlayerId player{};
    std::string player_type, difficulty{"NORMAL"};
    std::optional<gc::PlayerId> enemy, human;
    bool operator==(const PlayerSelection&) const = default;
};
struct PlanInput {
    std::string name;
    std::vector<std::string> goals;
};
struct LifecycleSetup {
    std::vector<PlayerSelection> players;
    std::map<std::string, std::string, std::less<>> xml;
    std::map<std::string, gc::funding::CategoryId, std::less<>> categories;
    std::function<std::optional<Number>(std::string_view, std::string_view)> converters;
    // Empty selects required_xml(); explicit closure is useful for mod/fixture hosts.
    std::vector<std::string> xml_paths;
    std::vector<PlanInput> plans; // definitions loaded by the shared plan host
    ServicePeriods service_periods{retail_service_periods};
};
struct PlayerLifecycle {
    PlayerSelection selected;
    bool initialized{};
    std::array<std::uint64_t, service_count> deadlines{};
    std::vector<bool> active_templates;
    std::vector<std::size_t> activation_order;
    std::size_t template_cursor{};
    TemplatePolicy policy;
    bool operator==(const PlayerLifecycle&) const = default;
};
struct LifecycleState {
    std::uint32_t version{2};
    Clocks clocks;
    Admission admission;
    std::vector<PlayerLifecycle> players; // ascending player identity
    ServicePeriods service_periods{retail_service_periods};
    bool operator==(const LifecycleState&) const = default;
};
// Process-local work counters for the immutable per-player policy snapshots that
// service inputs share. Never saved or hashed.
struct PolicySnapshotStats {
    std::uint64_t builds{}, reuses{};
    bool operator==(const PolicySnapshotStats&) const = default;
};
struct ServiceTrace {
    gc::PlayerId player{};
    Service service{};
    Clocks clocks;
    bool operator==(const ServiceTrace&) const = default;
};

[[nodiscard]] std::vector<std::string> required_xml();
// WGA-01..05/09, GFSA-02/03. Bounded player/template coordination remains ordered;
// providers partition their immutable per-planet/producer preparation separately.
inline constexpr std::string_view lifecycle_component = "galactic-ai-lifecycle";
class Manager final : public authoritative::GalacticScriptEngine, public TrackedStateContributor {
public:
    [[nodiscard]] static core::Result<std::shared_ptr<Manager>> create(LifecycleSetup, Capabilities);
    ~Manager() override;
    [[nodiscard]] core::Result<void> service(Clocks, Admission, authoritative::ScriptScheduler&);
    [[nodiscard]] core::Result<authoritative::ServiceOptions> before_service(
        const sim::PartitionExecutor&, const gc::GalacticSession&, const gc::GalacticTick&, authoritative::ScriptScheduler&) override;
    [[nodiscard]] bool admits_service(const gc::GalacticTick&) const noexcept override;
    [[nodiscard]] bool perception_due(std::uint64_t synchronization) const noexcept;
    [[nodiscard]] ServicePeriods service_periods() const noexcept;
    [[nodiscard]] core::Result<void> after_service(authoritative::ServiceReport&) override;
    void set_admission(Admission) noexcept;
    [[nodiscard]] LifecycleState lifecycle() const;
    [[nodiscard]] core::Result<void> restore_lifecycle(const LifecycleState&);
    [[nodiscard]] core::Result<std::string> lifecycle_hash() const;
    [[nodiscard]] core::Result<NativeState> capture() const override;
    [[nodiscard]] core::Result<std::string> record_sync_hash(bool cold = false) const override;
    [[nodiscard]] std::size_t record_byte_upper_bound() const noexcept override;
    [[nodiscard]] core::Result<void> restore(const NativeState&) override;
    [[nodiscard]] core::Result<std::string> state_hash() const override;
    [[nodiscard]] const std::vector<ServiceTrace>& trace() const noexcept;
    [[nodiscard]] std::uint64_t trace_dropped() const noexcept;
    [[nodiscard]] PolicySnapshotStats policy_snapshot_stats() const noexcept;
    [[nodiscard]] std::string_view freestore(gc::PlayerId) const noexcept;
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> goal_functions(gc::PlayerId) const;
    [[nodiscard]] bool goal_enabled(gc::PlayerId, std::string_view category, std::string_view type) const;
    [[nodiscard]] bool plan_enabled(gc::PlayerId, std::string_view category, std::string_view type, std::string_view name) const;
private:
    struct Impl;
    explicit Manager(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace eawr::script::foc::galactic_ai
