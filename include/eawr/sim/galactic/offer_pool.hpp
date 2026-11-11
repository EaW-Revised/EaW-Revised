#pragma once

#include "eawr/sim/galactic/shared_rows.hpp"
#include "eawr/sim/galactic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eawr::sim::galactic {

// Safety cap on the table entries (row references) one pool may hold. A restored table is materialized once, so this
// bounds the decoded memory of an untrusted pool the way the byte limit bounds an inline save. A 145-planet, 1150-offer
// campaign references ~170 thousand rows in the worst case where no two planets share a table.
inline constexpr std::size_t max_offer_pool_references = 4'194'304;

// The checkpoint's one copy of the static production offer data (GC saves, section "offer-pool").
//
// Every planet carries an immutable, location-adjusted offer table. A large layered campaign has ~1150 offers on each of
// ~145 planets, and the session setup, the live production state and the direct owner state each used to write every
// table. The pool stores each distinct offer row once and each distinct planet table once (as row indices); the three
// encoders then write one u32 table index per planet. Equality is byte-exact (ProductionOffer::operator==), so a live
// table that differs from the setup's still round-trips: it simply gets its own table entry.
//
// Writing: intern() assigns indices in first-seen order, so the pool is deterministic for a given encode order.
// Reading: decode() materializes each distinct table once; tables() hands out shared storage, so planets with equal
// tables share one row vector after a restore.
class OfferPool {
public:
    OfferPool() = default;

    // The index of `table` in this pool, adding it (and any new rows) when no equal table is present.
    [[nodiscard]] std::uint32_t intern(const SharedRows<ProductionOffer>& table);
    // The table at `index`, or nullptr when the pool has no such table (a malformed reference).
    [[nodiscard]] const SharedRows<ProductionOffer>* table(std::uint32_t index) const noexcept;

    [[nodiscard]] std::size_t row_count() const noexcept { return rows_.size(); }
    [[nodiscard]] std::size_t table_count() const noexcept { return tables_.size(); }
    [[nodiscard]] bool empty() const noexcept { return tables_.empty(); }

    // The section payload: u32 row count, the rows (the replay's offer row layout), u32 table count, then per table a
    // u32 length and that many u32 row indices.
    [[nodiscard]] std::vector<std::uint8_t> encode() const;
    // Strict decode: every index in range, no trailing bytes. nullopt on any malformed input.
    [[nodiscard]] static std::optional<OfferPool> decode(std::span<const std::uint8_t> bytes);

private:
    void ensure_index();
    [[nodiscard]] std::uint32_t intern_row(const ProductionOffer& row, std::vector<std::uint8_t>& scratch);

    std::vector<ProductionOffer> rows_;
    std::vector<std::vector<std::uint32_t>> table_indices_;
    std::vector<SharedRows<ProductionOffer>> tables_;
    // Lookup structures for intern(); built lazily so a decoded pool that is only read never pays for them.
    bool indexed_{true};
    std::unordered_multimap<std::uint64_t, std::uint32_t> row_lookup_, table_lookup_;
    // Tables seen by storage identity (a decoded pool seeds it with its own tables, which restored planets share).
    // Holding the shared table keeps its storage (and so its address) alive.
    std::unordered_map<const void*, std::pair<SharedRows<ProductionOffer>, std::uint32_t>> by_storage_;
};

} // namespace eawr::sim::galactic
