#pragma once

#include "eawr/sim/tactical/session.hpp"
#include "session_components.hpp"
#include "eawr/sim/tactical/pathfind.hpp"
#include "fighters_internal.hpp"
#include <algorithm>
#include <array>
#include <iterator>
#include <set>
#include <type_traits>
#include <utility>

namespace eawr::sim::tactical::session_detail {

// WBP-05/16: retained transactional maps. Only journaled entries are synchronized;
// a failed tick restores its scratch on the next attempt, without copying either map.
template <typename State>
struct ObjectStage {
    std::map<EntityId, State> values;
    std::vector<EntityId> dirty;
    bool initialized{};
    void touch(const EntityId id) {
        if (std::find(dirty.begin(), dirty.end(), id) == dirty.end()) dirty.push_back(id);
    }
    void sync(std::map<EntityId, State>& destination, const std::map<EntityId, State>& source) {
        for (const auto id : dirty) {
            const auto found = source.find(id);
            if (found == source.end()) destination.erase(id);
            else destination.insert_or_assign(id, found->second);
        }
    }
    void begin(const std::map<EntityId, State>& committed, const std::size_t reserve) {
        if (!initialized) { values = committed; initialized = true; }
        else sync(values, committed);
        dirty.clear();
        dirty.reserve(reserve);
    }
    void commit(std::map<EntityId, State>& committed) {
        sync(committed, values);
        dirty.clear();
    }
};

struct CaptureJob {
    EntityId id{};
    std::size_t slot{};
    const CaptureProfile* profile{};
    PadState next;
    PlayerId before{};
    PlayerId owner{};
    std::uint64_t inspected{};
};

struct AsteroidFieldInput { SpaceBody body; math::Fixed radius; };
struct NebulaInput {
    SpaceBody body;
    math::Vec3 soft_center{};
    math::Fixed radius{};
    math::Fixed x_extent{};
    math::Fixed y_extent{};
    math::Vec2 forward{};
    bool nebula{};
    bool ion_storm{};
};
struct NebulaScratch {
    std::vector<std::optional<NebulaInput>> slots;
    std::vector<std::optional<core::Diagnostic>> errors;
    std::vector<NebulaInput> volumes;
    std::vector<SpaceBody> bodies;
    SpaceIndex index;
    std::array<std::vector<std::uint32_t>, tick_partition_count> candidates;
    std::vector<std::vector<Event>> events;
    std::vector<std::uint8_t> members;
    std::vector<std::uint8_t> previous;
    math::Fixed reach{};
};
struct AsteroidScratch {
    std::vector<std::optional<AsteroidFieldInput>> field_slots;
    std::vector<std::optional<core::Diagnostic>> errors;
    std::vector<AsteroidFieldInput> fields;
    std::vector<SpaceBody> bodies;
    SpaceIndex index;
    std::array<std::vector<std::uint32_t>, tick_partition_count> candidates;
    std::vector<std::vector<AsteroidImpact>> impacts;
    std::vector<std::vector<Hit>> redirected;
    std::vector<std::uint64_t> queries;
    std::vector<std::uint64_t> examined;
};

struct CaptureScratch {
    std::vector<SpaceBody> bodies;
    SpaceIndex index;
    std::vector<std::uint32_t> positions;
    std::vector<CaptureCandidate> candidates;
    std::optional<core::Diagnostic> error;
};

struct IncomePayment {
    PlayerId owner{};
    std::int64_t amount{};
    bool split{};
};

struct RespawnBatch {
    std::vector<RespawnState> objects;
};

// A unit as the step stages it: its public state and, when durable, its health; when it has a
// motion profile, its movement, its speed and its roll at the staged tick; when it has a
// combat profile, its targets and fire cycle; when a group move delays its plan, that plan.
struct PlacementCache {
    math::Vec3 position{};
    math::Quat rotation{};
    math::Mat3x4 matrix{};
};

struct LiveUnit {
    UnitState state;
    std::optional<DurabilityState> durability;
    std::optional<MotionState> motion;
    math::Fixed speed{};
    math::Fixed roll{};
    std::optional<CombatState> combat;
    std::optional<FormationWait> formation{};
    std::optional<AbilityState> abilities{}; // #76: a type with abilities
    std::optional<Approach> approach{};
    std::optional<IonStunState> ion_stun{}; // #561: while an ion stun runs (IS-03)
    std::optional<std::uint64_t> arrival_vulnerable_until{}; // WR-41: independent of movement release
    CombatBonuses upgrade_bonuses{}; // WPR-51, recomputed from held objects
    std::optional<DurabilityProfile> upgraded_durability{};
    std::optional<PlacementCache> placement_cache{}; // tick scratch, carried through staging
    std::uint32_t matrix_builds{};
    std::uint32_t banked_matrix_builds{};
    std::optional<std::uint64_t> asteroid_contact{}; // WHZ-13: retained on a service gate failure
    bool engines_recovered{}; // tick scratch: EN-08 recovery requires a capital move replan
    std::optional<NebulaContact> nebula{};
    // Derived tick scratch: partitioned ability service prepares O(1) damage consumers.
    math::Fixed cause_damage_mode{math::Fixed::from_raw(math::Fixed::scale)};
    math::Fixed take_damage_mode{math::Fixed::from_raw(math::Fixed::scale)};
    bool in_tractor_beam{}; // derived from live source/category entries, never hashed
};

// The already sorted unit vector is also the command stage. This small map-shaped view
// keeps the command code's ID lookups while avoiding one allocated node per live unit.
class UnitStage final {
public:
    explicit UnitStage(std::vector<LiveUnit> units = {}) : units_(std::move(units)) {}
    template <bool Constant>
    class Iterator {
        using Base = std::conditional_t<Constant, std::vector<LiveUnit>::const_iterator,
            std::vector<LiveUnit>::iterator>;
        using Unit = std::conditional_t<Constant, const LiveUnit, LiveUnit>;
    public:
        using value_type = std::pair<EntityId, Unit&>;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        Iterator(Base position, Base end, const std::set<EntityId>* removed)
            : position_(position), end_(end), removed_(removed) { skip_removed(); }
        struct Arrow {
            value_type value;
            const value_type* operator->() const { return &value; }
        };
        value_type operator*() const { return {position_->state.entity_id, *position_}; }
        Arrow operator->() const { return {**this}; }
        Iterator& operator++() { ++position_; skip_removed(); return *this; }
        Iterator operator++(int) { auto before = *this; ++*this; return before; }
        friend bool operator==(const Iterator&, const Iterator&) = default;
        Base base() const { return position_; }
    private:
        void skip_removed() {
            while (position_ != end_ && removed_->contains(position_->state.entity_id)) ++position_;
        }
        Base position_;
        Base end_;
        const std::set<EntityId>* removed_;
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;
    iterator begin() { return iterator(units_.begin(), units_.end(), &removed_); }
    iterator end() { return iterator(units_.end(), units_.end(), &removed_); }
    const_iterator begin() const { return const_iterator(units_.begin(), units_.end(), &removed_); }
    const_iterator end() const { return const_iterator(units_.end(), units_.end(), &removed_); }
    iterator find(EntityId id) { return iterator(locate(id), units_.end(), &removed_); }
    const_iterator find(EntityId id) const { return const_iterator(locate(id), units_.end(), &removed_); }
    LiveUnit& at(EntityId id) { return units_.at(static_cast<std::size_t>(locate(id) - units_.begin())); }
    const LiveUnit& at(EntityId id) const { return units_.at(static_cast<std::size_t>(locate(id) - units_.begin())); }
    void emplace(EntityId id, LiveUnit unit) {
        const auto position = lower(id);
        if (position == units_.end() || position->state.entity_id != id) units_.insert(position, std::move(unit));
        else if (removed_.erase(id) != 0) *position = std::move(unit);
    }
    // Deaths leave tombstones until the systems phase: no per-death vector shift, and every
    // other staged unit retains its address through the ordered commands.
    void erase(iterator position) { removed_.insert(position->first); }
    std::size_t size() const { return units_.size() - removed_.size(); }
    std::vector<LiveUnit> release() {
        if (!removed_.empty()) std::erase_if(units_, [this](const LiveUnit& unit) {
            return removed_.contains(unit.state.entity_id);
        });
        return std::move(units_);
    }
private:
    std::vector<LiveUnit>::iterator lower(EntityId id) { return std::lower_bound(units_.begin(), units_.end(), id,
        [](const LiveUnit& unit, EntityId value) { return unit.state.entity_id < value; }); }
    std::vector<LiveUnit>::const_iterator lower(EntityId id) const { return std::lower_bound(units_.begin(), units_.end(), id,
        [](const LiveUnit& unit, EntityId value) { return unit.state.entity_id < value; }); }
    std::vector<LiveUnit>::iterator locate(EntityId id) { const auto position = lower(id);
        return position != units_.end() && position->state.entity_id == id && !removed_.contains(id) ? position : units_.end(); }
    std::vector<LiveUnit>::const_iterator locate(EntityId id) const { const auto position = lower(id);
        return position != units_.end() && position->state.entity_id == id && !removed_.contains(id) ? position : units_.end(); }
    std::vector<LiveUnit> units_;
    std::set<EntityId> removed_;
};

// A reinforced unit's population share (#530, PU-21): its owner and its share.
struct PopulationShare {
    PlayerId owner{};
    std::int64_t share{};
};

// A squadron's orders as its craft read them in the craft phase (#75): its leader, what it
// attacks or the point it holds.
struct SquadronFrame {
    const SquadronState* state{};
    const SquadronProfile* profile{};
    EntityId leader{};
    bool attacking{};
    math::Vec3 target_position{};
    math::Fixed target_radius{};
    bool target_craft{};
    bool approach{}; // FA-07
    DogfightFlight dogfight{DogfightFlight::none}; // #457, FD-01 to FD-03
    CombatCell cell{};
    math::Vec3 hold{};
    bool moving{};
    std::optional<LaneFlight> lane{}; // FO-10, FO-11 (#599)
    // FT-02, FO-05, FO-06: whether the squadron scans this frame, and from where (the point it
    // holds, or its leader on an attack-move's way).
    bool scans{};
    bool scan_from_leader{};
    std::span<const EntityId> live_roster{}; // WSQ-51: surviving members fill slots without holes
};

// A squadron as the squadron phase stages it (#271).
struct SquadronOutcome {
    bool container_live{};
    std::vector<EntityId> members; // live craft, ascending
    math::Vec3 centre{};
    math::Mat3x4 transform{};
    std::optional<core::Diagnostic> error;
};

// Per-unit predictions at the window boundaries of the tick's two possible layer starts
// (AV-02, AV-03): `current` from the layer's rolled anchor, `fresh` from this frame (a layer
// rebuilt by a plan this tick). Empty for a unit at rest.
struct TrackSamples {
    std::vector<Prediction> current;
    std::vector<Prediction> fresh;
};

// The tracking state of one step (#71): which dynamic layers a submission rebuilt this
// tick, the samples of every tracked unit and the layer views built from them.
struct Tracking {
    std::uint64_t frame{};
    std::uint32_t interval{};
    std::uint32_t windows{};
    std::array<std::uint64_t, 4> current_start{};
    std::map<EntityId, TrackSamples> samples;
    std::array<std::optional<TrackingLayerView>, 4> views;
    std::optional<std::vector<TrackedLeaf>> statics;
    std::set<EntityId> suspended; // WR-33: arrivals do not participate in ordinary tracking
};

} // namespace eawr::sim::tactical::session_detail
