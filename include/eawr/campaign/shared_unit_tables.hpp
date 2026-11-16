#pragma once

#include "eawr/campaign/setup_plan.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/units/unit_tables.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace eawr::vfs { class Vfs; }
namespace eawr::campaign {
// One immutable unit-table load of a prepared campaign's types, shared by the preparation steps that each used to run
// the same load (auto-resolver inputs, AI selection content, the galactic object-query catalog). It records the exact
// type list it was loaded for: a consumer uses it only when its own list is the same (names compared case-insensitively,
// duplicates folded as the loader folds them), and loads for itself otherwise. Built once, read-only afterwards.
class SharedUnitTables final {
public:
    // `files` and `catalog` are what the load was made from (the loader's other inputs); built_for() lets a consumer check that it
    // reads the same ones, since requested_exactly() compares the type list only. The Vfs address is compared and never
    // dereferenced: the load lives within one preparation call, which keeps the mount alive.
    SharedUnitTables(units::UnitTables tables, const std::vector<std::string>& requested,
        const vfs::Vfs* files = nullptr, const std::uint64_t catalog_generation = 0)
        : tables_(std::move(tables)), requested_(fold(requested)), files_(files), catalog_generation_(catalog_generation) {
        for (const auto& name : requested_) covered_.insert(name);
    }
    // True when the load was made from this mounted VFS and this catalog (an unbound load, as in tests, is built for nothing).
    [[nodiscard]] bool built_for(const vfs::Vfs& files, const data::Catalog& catalog) const noexcept {
        return files_ == &files && catalog_generation_ == catalog.generation();
    }
    [[nodiscard]] const units::UnitTables& tables() const noexcept { return tables_; }
    // True when the load was requested with exactly this list (same folded names, same first-occurrence order): the loader
    // queues types in that order and skips repeats, so equal folded lists give the same tables as a load of `names`.
    [[nodiscard]] bool requested_exactly(std::span<const std::string> names) const {
        return fold(names) == requested_;
    }
    // True when the load was requested with this type. The type's flag fields (targeting, unit_ai, selectable) and the
    // category enum equal those of an own load of any list holding the type; the row's index fields into the load's
    // tables (UnitType indices) differ between a superset load and an own load, so a consumer that reads them must
    // require requested_exactly instead.
    [[nodiscard]] bool covers(std::string_view name) const { return covered_.contains(lower(name)); }
    // Lower-case names in first-occurrence order.
    [[nodiscard]] static std::vector<std::string> fold(std::span<const std::string> names) {
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        for (const auto& name : names) {
            auto folded = lower(name);
            if (seen.insert(folded).second) out.push_back(std::move(folded));
        }
        return out;
    }
private:
    [[nodiscard]] static std::string lower(std::string_view name) {
        std::string out(name);
        for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }
    units::UnitTables tables_;
    std::vector<std::string> requested_;
    std::unordered_set<std::string> covered_;
    const vfs::Vfs* files_{};
    std::uint64_t catalog_generation_{};
};
// The unit-table load list of the AI selection content: every loadable bound original type, then every loadable planet
// type. The selection content, the object-query catalog and the shared load all use this list.
[[nodiscard]] std::vector<std::string> selection_unit_table_types(const data::Catalog&, const CampaignSetupPlan&);
// Loads the tables for selection_unit_table_types once (the same LoadInput as the selection content's own load).
[[nodiscard]] core::Result<std::shared_ptr<const SharedUnitTables>> prepare_shared_unit_tables(
    const vfs::Vfs&, const data::Catalog&, const CampaignSetupPlan&);
} // namespace eawr::campaign
