#pragma once

#include "galactic_ai_execution.hpp"
#include "eawr/core/record_digest.hpp"
#include "galactic_ai_selection_content.hpp"

#include <set>
#include <stdexcept>
#include <type_traits>

namespace eawr::script::foc::galactic_ai {
// WGA-300..302/307/312: authoritative actor state, not tactical mobility.
// The caller must supply every fact; qualified=false refuses publication.
struct FreestoreFacts {
    FreestoreObject object;
    gc::TypeId current_type{};
    sim::EntityId parent{};
    bool parent_is_planet{}, current_type_member{}, hero{};
    bool ai_usable{}, dead{}, deleting{}, death_clone{}, qualified{};
    bool enroute{}, tracked_movement{};
    bool unit_ai{}, prefer_ground{true}; // WGA-409: retained preference, fresh true
    bool operator==(const FreestoreFacts&) const = default;
};
[[nodiscard]] bool freestore_admission(const FreestoreFacts&, gc::PlayerId) noexcept;
// WGA-316: ordinary construction only; never call over a loaded/live actor.
// Generated team/squadron actors subsequently copy their source AI-usability.
[[nodiscard]] FreestoreFacts ordinary_freestore_birth(FreestoreFacts);

// WGA-303..315: retained membership and explicit local lifecycle operations.
// Updating raw facts never silently rebuilds membership. The owner is nested
// in NativeActions' production envelope, not a seventh native contributor.
class QualifiedFreestore final {
    struct Operation;
public:
    using Changed = std::function<void()>;
    using Replay = std::function<core::Result<void>(QualifiedFreestore&)>;
    using IntentSink = std::function<void(Replay)>;
    // Install only on the selected decision frontier. clone() never inherits
    // this sink: abandoned Lua transactions must publish no tentative intent.
    void set_intent_sink(IntentSink sink) {
        if (!track_operations_) throw std::logic_error("freestore intent sink requires a tracked decision frontier");
        intent_sink_ = std::move(sink);
    }
    [[nodiscard]] const IntentSink& intent_sink() const noexcept { return intent_sink_; }
    class SuspendIntents {
    public:
        SuspendIntents(const SuspendIntents&) = delete;
        SuspendIntents& operator=(const SuspendIntents&) = delete;
        ~SuspendIntents() { --owner_.mutation_depth_; }
    private:
        friend class QualifiedFreestore;
        explicit SuspendIntents(QualifiedFreestore& owner) : owner_(owner) { ++owner_.mutation_depth_; }
        QualifiedFreestore& owner_;
    };
    [[nodiscard]] SuspendIntents suspend_intents() { return SuspendIntents(*this); }
    class OperationCursor {
    private:
        friend class QualifiedFreestore;
        OperationCursor(std::shared_ptr<const Operation> prefix, std::string base)
            : prefix_(std::move(prefix)), base_(std::move(base)) {}
        std::shared_ptr<const Operation> prefix_;
        std::string base_;
    };
    [[nodiscard]] OperationCursor operation_cursor() const { return {operations_, overlay_base_hash_}; }
    explicit QualifiedFreestore(Changed changed = {}) : changed_(std::move(changed)) {}
    [[nodiscard]] core::Result<void> update(FreestoreFacts);
    [[nodiscard]] core::Result<void> initialize(gc::PlayerId, std::span<const sim::EntityId> offers);
    [[nodiscard]] core::Result<bool> offer(gc::PlayerId, sim::EntityId);
    [[nodiscard]] core::Result<void> owner_changed(FreestoreFacts, bool new_manager_initialized);
    void lost(sim::EntityId);
    // WGA-300/163: delete-pending notification retains raw facts until loss.
    void remove_membership(gc::PlayerId player, sim::EntityId actor);
    [[nodiscard]] core::Result<void> prevent_ai_usage(gc::PlayerId, sim::EntityId, bool prevent);
    [[nodiscard]] core::Result<void> set_prefer_ground(sim::EntityId, bool);
    [[nodiscard]] bool contains(gc::PlayerId, sim::EntityId) const noexcept;
    [[nodiscard]] bool initialized(gc::PlayerId player) const noexcept { return stores_.contains(player); }
    [[nodiscard]] std::uint64_t object_count(gc::PlayerId, std::optional<bool> nontransport = {}) const;
    [[nodiscard]] QualifiedFreestore clone(Changed changed = {}, bool track_operations = false) const;
    void replace(const QualifiedFreestore&);
    // GFS-A22: validate the immutable operation prefix first, then re-admit only this overlay's new intents.
    // The return value counts refusals; earlier admissions remain applied and no intent is retried.
    [[nodiscard]] core::Result<std::size_t> commit_overlay(const QualifiedFreestore& base, const QualifiedFreestore& overlay);
    // Value-owned, chronological delta for an accepted immediate transaction.
    // Exporting never executes an operation or publishes a tentative clone.
    [[nodiscard]] core::Result<std::vector<Replay>> operations_since(const QualifiedFreestore& base) const;
    [[nodiscard]] core::Result<std::vector<Replay>> operations_since(const OperationCursor&) const;
    [[nodiscard]] core::Result<bool> reserve(gc::PlayerId, sim::EntityId, std::uint64_t plan);
    void release(gc::PlayerId, std::uint64_t plan);
    void release_object(gc::PlayerId, sim::EntityId, std::uint64_t plan);
    [[nodiscard]] FreestoreAssignment assign(gc::PlayerId, sim::EntityId, std::uint64_t plan);
    [[nodiscard]] core::Result<std::shared_ptr<const FreestorePool>> publish(const sim::PartitionExecutor&) const;
    [[nodiscard]] core::Result<std::shared_ptr<const FreestorePool>> publish(gc::PlayerId, const sim::PartitionExecutor&) const;
    [[nodiscard]] std::span<const sim::EntityId> additions(gc::PlayerId) const noexcept;
    [[nodiscard]] core::Result<void> consume_additions(gc::PlayerId, std::size_t count);
    [[nodiscard]] const FreestoreFacts* facts(sim::EntityId) const noexcept;
    [[nodiscard]] const std::map<sim::EntityId, FreestoreFacts>& inventory() const noexcept { return facts_; }
    [[nodiscard]] const std::map<sim::EntityId, std::uint64_t>* reservations(gc::PlayerId player) const noexcept {
        const auto found = stores_.find(player); return found == stores_.end() ? nullptr : &found->second->reservations;
    }
    [[nodiscard]] std::vector<std::uint8_t> encode() const;
    [[nodiscard]] std::string record_sync_hash(bool cold = false) const;
    [[nodiscard]] std::size_t record_byte_upper_bound() const noexcept { return records_.byte_size() + 1024; }
    [[nodiscard]] core::Result<void> restore(std::span<const std::uint8_t>, sim::EntityId next_id,
        std::span<const gc::GalacticPlayer> players);
private:
    struct Operation {
        mutable std::shared_ptr<const Operation> previous;
        Replay replay;
        ~Operation() {
            // A bounded barrier can still carry many intents; release its private tail iteratively.
            while (previous && previous.use_count() == 1) {
                auto parent = std::move(previous->previous);
                previous.reset(); previous = std::move(parent);
            }
        }
    };
    std::shared_ptr<const Operation> operations_;
    std::string overlay_base_hash_;
    bool track_operations_{};
    IntentSink intent_sink_;
    std::size_t mutation_depth_{};
    template<class Body, class Factory> auto mutate(Body body, Factory factory) {
        const bool capture = track_operations_ && mutation_depth_ == 0;
        Replay replay = capture ? factory() : Replay{};
        struct Depth { std::size_t& value; ~Depth() { --value; } } depth{mutation_depth_};
        ++mutation_depth_;
        auto result = body();
        if (capture && result) {
            bool admitted = true;
            if constexpr (std::is_same_v<decltype(result), core::Result<bool>>) admitted = result.value();
            if constexpr (std::is_same_v<decltype(result), core::Result<FreestoreAssignment>>)
                admitted = result.value() == FreestoreAssignment::assigned;
            if (admitted) {
                operations_ = std::make_shared<Operation>(Operation{operations_, std::move(replay)});
                if (intent_sink_) intent_sink_(operations_->replay);
            }
        }
        return result;
    }
    [[nodiscard]] core::Result<void> update_impl(FreestoreFacts);
    [[nodiscard]] core::Result<void> initialize_impl(gc::PlayerId, std::span<const sim::EntityId>);
    [[nodiscard]] core::Result<bool> offer_impl(gc::PlayerId, sim::EntityId);
    [[nodiscard]] core::Result<void> owner_changed_impl(FreestoreFacts, bool);
    void lost_impl(sim::EntityId);
    [[nodiscard]] core::Result<void> prevent_ai_usage_impl(gc::PlayerId, sim::EntityId, bool);
    [[nodiscard]] core::Result<void> set_prefer_ground_impl(sim::EntityId, bool);
    [[nodiscard]] core::Result<bool> reserve_impl(gc::PlayerId, sim::EntityId, std::uint64_t);
    void release_impl(gc::PlayerId, std::uint64_t);
    void release_object_impl(gc::PlayerId, sim::EntityId, std::uint64_t);
    [[nodiscard]] FreestoreAssignment assign_impl(gc::PlayerId, sim::EntityId, std::uint64_t);
    [[nodiscard]] core::Result<void> consume_additions_impl(gc::PlayerId, std::size_t);
    struct Store {
        std::set<sim::EntityId> members;
        std::map<sim::EntityId, std::uint64_t> reservations;
        std::vector<sim::EntityId> additions;
        mutable std::shared_ptr<const FreestorePool> published{};
        bool operator==(const Store& other) const {
            return members == other.members && reservations == other.reservations && additions == other.additions;
        }
    };
    void update_fact_record(sim::EntityId);
    void update_store_record(gc::PlayerId);
    [[nodiscard]] core::RecordDigest rebuild_records() const;
    core::RecordDigest records_;
    void changed() { published_.reset(); if (changed_) changed_(); }
    void changed(gc::PlayerId player) {
        published_.reset();
        if (stores_.contains(player)) write_store(player).published.reset();
        if (changed_) changed_();
    }
    [[nodiscard]] Store& write_store(gc::PlayerId player) {
        auto& store = stores_.at(player);
        if (store.use_count() != 1) store = std::make_shared<Store>(*store);
        return *store;
    }
    void remove(gc::PlayerId, sim::EntityId);
    std::map<sim::EntityId, FreestoreFacts> facts_;
    // Native membership, reservations and callback order detach only for the writing player.
    std::map<gc::PlayerId, std::shared_ptr<Store>> stores_;
    std::set<sim::EntityId> lost_; // WGA-436: identities never reused, so a handle to one is a removed object
    Changed changed_;
    mutable std::shared_ptr<const FreestorePool> published_; // derived, reused until an explicit write
};
// WGA-408/409/410: first refusal in the native consumer's gate order.
enum class LandingGate { admitted, human, safety, transit, transport_parent, parent_planet,
    preference, grandparent_planet, capacity, surface };
[[nodiscard]] LandingGate automatic_landing_gate(bool human, const FreestoreObject&,
    const FreestoreFacts&, const FreestoreFacts* parent, const FreestoreSurface*) noexcept;
// WGA-303: fresh offered identities only, in planet/landed/fleet order.
[[nodiscard]] core::Result<std::vector<sim::EntityId>> initial_freestore_offers(
    const gc::GalacticSnapshot&, std::span<const FreestoreType>);
// Qualified fresh actor facts supply current transport representation per id.
[[nodiscard]] core::Result<std::vector<sim::EntityId>> initial_freestore_offers(
    const gc::GalacticSnapshot&, const QualifiedFreestore&);
[[nodiscard]] core::Result<void> register_qualified_freestore_bindings(authoritative::ScriptScheduler&,
    std::weak_ptr<QualifiedFreestore>);
// Resolve the final installed owner at invocation, after early Lua binding.
[[nodiscard]] core::Result<void> register_qualified_freestore_bindings(authoritative::ScriptScheduler&,
    std::function<std::shared_ptr<QualifiedFreestore>()>);
// The completed native pool must come from QualifiedFreestore::publish.
// Route/type/assignment producers remain explicit; missing inputs refuse.
using FreestoreSelectionFacts = std::function<core::Result<std::pair<Number, bool>>(
    gc::PlayerId, gc::PlanetId, sim::EntityId, const ai::PlanDef&)>;
[[nodiscard]] ai::GalacticFreestoreSelectionProvider qualified_freestore_selection(
    std::shared_ptr<const FreestoreInput>, std::shared_ptr<const ai::GalacticWorld>,
    std::shared_ptr<const ai::Engine>, FreestoreSelectionFacts);
// Everything that does not depend on the actor (goal type, stage list, stage positions) is resolved once per
// (player, target, plan) call: the batch returns the per-actor reader for that call.
using FreestoreActorSelection = std::function<core::Result<std::pair<Number, bool>>(sim::EntityId)>;
using FreestoreSelectionBatch = std::function<FreestoreActorSelection(gc::PlayerId, gc::PlanetId, const ai::PlanDef&)>;
[[nodiscard]] ai::GalacticFreestoreSelectionProvider qualified_freestore_batch_selection(
    std::shared_ptr<const FreestoreInput>, std::shared_ptr<const ai::GalacticWorld>,
    std::shared_ptr<const ai::Engine>, FreestoreSelectionBatch);
} // namespace eawr::script::foc::galactic_ai
