#pragma once

#include "eawr/sim/commands.hpp"
#include "eawr/sim/tactical/abilities.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace eawr::presentation::audio {

// What the ability sounds remember about one ability of one unit from the previous snapshot read. A timed
// one is remembered as such once it has read frames left, with its latest count: the snapshot of its last
// active tick reads zero, like an untimed ability, so BA-51 tells a natural end from a switch-off by this
// history.
struct AbilityMemory final {
    bool active{};
    bool timed{};
    std::uint32_t remaining{};
    std::uint64_t started{};
    bool positioned{};
};

// The previous tick's AbilityMemory per (entity, kind), for a caller that visits the instances of every snapshot
// in ascending entity order (the snapshot's own contract) and each instance's abilities in a fixed order. Rows
// are written in that visiting order, so the rows of the unit now being visited are found by one forward cursor
// instead of a keyed lookup, and two reusable buffers swap each tick (a steady state allocates nothing). The
// answers equal a std::map keyed by (entity, kind) that is rebuilt every tick: `before` is read only from the
// previous tick, a repeated (entity, kind) resolves to the LAST row written, and a unit absent from a tick is
// forgotten. If the visiting order is ever not strictly ascending the lookup scans every row with the same result.
class AbilityHistory final {
public:
    void begin_tick() {
        next_.clear();
        group_begin_ = group_end_ = 0;
        visiting_ordered_ = true;
        any_ = false;
    }
    void begin_instance(const sim::EntityId entity) {
        if (any_ && entity <= last_entity_) visiting_ordered_ = false;
        any_ = true;
        last_entity_ = entity;
        entity_ = entity;
        if (!cursor_valid()) return;
        group_begin_ = group_end_;
        while (group_begin_ < rows_.size() && rows_[group_begin_].entity < entity) ++group_begin_;
        group_end_ = group_begin_;
        while (group_end_ < rows_.size() && rows_[group_end_].entity == entity) ++group_end_;
    }
    // The previous tick's memory of `kind` for the unit begin_instance() named, or nullptr.
    [[nodiscard]] const AbilityMemory* before(const sim::tactical::AbilityKind kind) const {
        if (cursor_valid()) {
            for (std::size_t row = group_end_; row > group_begin_; --row) {
                if (rows_[row - 1].kind == kind) return &rows_[row - 1].memory;
            }
            return nullptr;
        }
        for (std::size_t row = rows_.size(); row > 0; --row) {
            if (rows_[row - 1].entity == entity_ && rows_[row - 1].kind == kind) return &rows_[row - 1].memory;
        }
        return nullptr;
    }
    void record(const sim::EntityId entity, const sim::tactical::AbilityKind kind, const AbilityMemory& memory) {
        next_.push_back({entity, kind, memory});
    }
    void end_tick() {
        rows_.swap(next_);
        rows_ordered_ = visiting_ordered_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }

private:
    // The cursor needs the stored rows and this tick's visits both ascending; otherwise every lookup scans.
    [[nodiscard]] bool cursor_valid() const noexcept { return rows_ordered_ && visiting_ordered_; }
    struct Row final {
        sim::EntityId entity{};
        sim::tactical::AbilityKind kind{};
        AbilityMemory memory{};
    };
    std::vector<Row> rows_;
    std::vector<Row> next_;
    std::size_t group_begin_{}, group_end_{};
    bool rows_ordered_{true};
    bool visiting_ordered_{true};
    bool any_{};
    sim::EntityId last_entity_{};
    sim::EntityId entity_{};
};

} // namespace eawr::presentation::audio
