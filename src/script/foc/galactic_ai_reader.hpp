#pragma once

// GC AI S3a: the AI world reader's structural index and fog facts (WGA-06/07/08, WGUI-18, WGA-185).
// Everything here is a pure function of one completed galactic snapshot plus authored constants. It is built by
// a partitioned prepass when the AI attaches to a completed tick and is frozen while decisions run: no memo in this
// file is written from inside a decision pass. Process-local data (slots, buffers) never reaches a hash or archive.
#include "ai_world.hpp"
#include "eawr/sim/galactic/snapshot.hpp"
#include "eawr/sim/world.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace eawr::script::foc::ai {

// Membership and slot lookup of identities in an open-addressing table. Slots are insertion ranks; the table
// layout is never iterated and never leaves the process.
class FlatIdMap {
public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    explicit FlatIdMap(std::size_t expected) {
        std::size_t capacity = 16;
        while (capacity < expected * 2) capacity *= 2;
        table_.assign(capacity, 0);
    }
    [[nodiscard]] std::size_t find(sim::EntityId id) const noexcept {
        for (auto slot = hash(id) & (table_.size() - 1);; slot = (slot + 1) & (table_.size() - 1)) {
            if (!table_[slot]) return npos;
            if (keys_[table_[slot] - 1] == id) return table_[slot] - 1;
        }
    }
    [[nodiscard]] bool contains(sim::EntityId id) const noexcept { return find(id) != npos; }
    // The slot of the identity and whether this call added it.
    std::pair<std::size_t, bool> insert(sim::EntityId id) {
        if ((keys_.size() + 1) * 2 > table_.size()) grow();
        for (auto slot = hash(id) & (table_.size() - 1);; slot = (slot + 1) & (table_.size() - 1)) {
            if (!table_[slot]) { keys_.push_back(id); table_[slot] = static_cast<std::uint32_t>(keys_.size()); return {keys_.size() - 1, true}; }
            if (keys_[table_[slot] - 1] == id) return {table_[slot] - 1, false};
        }
    }
    [[nodiscard]] const std::vector<sim::EntityId>& keys() const noexcept { return keys_; }
private:
    static std::size_t hash(sim::EntityId id) noexcept {
        std::uint64_t value = id;
        value ^= value >> 30U; value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27U; value *= 0x94d049bb133111ebULL;
        return static_cast<std::size_t>(value ^ (value >> 31U));
    }
    void grow() {
        table_.assign(table_.size() * 2, 0);
        for (std::size_t index = 0; index < keys_.size(); ++index) {
            auto slot = hash(keys_[index]) & (table_.size() - 1);
            while (table_[slot]) slot = (slot + 1) & (table_.size() - 1);
            table_[slot] = static_cast<std::uint32_t>(index + 1);
        }
    }
    std::vector<std::uint32_t> table_;
    std::vector<sim::EntityId> keys_;
};

// One planet as the object resolver reads it (the world's authored planet row, already derived).
struct ReaderPlanet {
    sim::galactic::PlanetId id{};
    sim::tactical::PlayerId owner{};
    sim::math::Vec3 position{};
    sim::tactical::TypeId type{};
    const sim::galactic::PlanetProduction* production{};
};

// One resolved object identity. The first resolution of an identity wins; later duplicates are dropped.
struct ReaderObject {
    sim::EntityId id{};
    sim::tactical::TypeId type{};
    sim::tactical::PlayerId owner{};
    sim::math::Vec3 position{};
    sim::galactic::PlanetId planet{}; // containing planet, zero in transit or unparented
    sim::galactic::IntelligenceContainer container{};
    bool structure{};
    friend bool operator==(const ReaderObject&, const ReaderObject&) = default;
};

// Immutable per structural generation: motion-only ticks reuse it and only refresh positions.
struct StructuralIndex {
    std::vector<ReaderObject> objects;  // ascending identity
    std::vector<sim::EntityId> transit; // ascending, unique: fleets and payloads that are enroute
    [[nodiscard]] const ReaderObject* find(sim::EntityId id) const noexcept;
    [[nodiscard]] sim::galactic::PlanetId containing(sim::EntityId id) const noexcept { // zero when none
        const auto* object = find(id); return object ? object->planet : 0;
    }
    [[nodiscard]] bool arriving(sim::EntityId id) const noexcept;
    friend bool operator==(const StructuralIndex&, const StructuralIndex&) = default;
};

// Ordered roster traversal (planets, fleets and members, landed forces, production objects, nested payloads).
[[nodiscard]] StructuralIndex build_structural_index(std::span<const ReaderPlanet> planets,
    const sim::galactic::GalacticSnapshot& snapshot);

// Authored per-type object traits (WGUI-18) and fixture visibility records, in the form fog reads them.
struct ReaderTypeTraits { sim::tactical::TypeId type{}; bool present{}, hero{}, named_hero{}; };
struct ReaderTeam { sim::tactical::PlayerId player{}; sim::tactical::TeamId team{}; };
struct ReaderLegacyVisibility { sim::tactical::PlayerId player{}; sim::galactic::PlanetId planet{}; };
struct ReaderFog {
    bool uses_fog{};
    const sim::galactic::GalacticSnapshot* snapshot{};
    std::span<const ReaderTypeTraits> traits;      // ascending type
    std::span<const ReaderTeam> teams;             // ascending player
    std::span<const ReaderLegacyVisibility> legacy; // explicit mask-zero visible fixture rows, original order
    std::span<const detail::ViewPlayer> viewers;   // ascending id
};

// Fill `units` (resized to the index) with identity, ownership, team, position and fog bits.
[[nodiscard]] core::Result<void> fill_view_units(std::vector<detail::ViewUnit>& units, const StructuralIndex&,
    const ReaderFog&, const sim::PartitionExecutor&);

// The AI's read side of one completed tick: the structural index and the published world view. The view is
// double-buffered: the publication before the current one is reused once no reader holds it. A roster epoch names
// one resolved object roster; it is process-local and never saved, hashed or compared across processes.
class AiWorldReader {
public:
    [[nodiscard]] const std::shared_ptr<detail::WorldView>& view() const noexcept { return view_; }
    [[nodiscard]] const StructuralIndex* index() const noexcept { return index_.get(); }
    [[nodiscard]] sim::galactic::PlanetId containing(sim::EntityId id) const noexcept { return index_ ? index_->containing(id) : 0; }
    [[nodiscard]] bool arriving(sim::EntityId id) const noexcept { return index_ && index_->arriving(id); }
    // A buffer to fill for the next publication. `aligned`: it already holds the current roster, so only the
    // positions of the current view can differ.
    [[nodiscard]] std::shared_ptr<detail::WorldView> take_buffer(bool& aligned);
    // Same roster as the current view (motion, clocks).
    void publish(std::shared_ptr<detail::WorldView> view);
    // A new resolved roster.
    void publish(std::shared_ptr<detail::WorldView> view, std::shared_ptr<const StructuralIndex> index);
private:
    std::shared_ptr<detail::WorldView> view_, spare_;
    std::shared_ptr<const StructuralIndex> index_;
    std::uint64_t view_epoch_{}, spare_epoch_{}, epochs_{};
};

} // namespace eawr::script::foc::ai
