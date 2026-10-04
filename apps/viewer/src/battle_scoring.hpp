#pragma once

#include "eawr/core/result.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/tactical/snapshot.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace eawr::presentation::godot_backend {

// Derived results consumer: the mounted scoring module runs in the existing
// sandbox. Its outputs never enter world decisions or replay state (WBF-45/46).
class BattleScoring final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<BattleScoring>> create(
        const vfs::Vfs& files, const data::Catalog& catalog, const units::UnitTables& tables,
        std::map<sim::tactical::PlayerId, std::string> factions, std::string map);
    ~BattleScoring();
    [[nodiscard]] core::Result<void> observe(const sim::tactical::TacticalSnapshot& snapshot);
    [[nodiscard]] core::Result<std::string> query(sim::tactical::PlayerId player, std::string_view control);
    [[nodiscard]] std::uint64_t pumps() const noexcept;
    [[nodiscard]] double combat_rating(sim::tactical::TypeId type) const noexcept;
private:
    struct Impl;
    explicit BattleScoring(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace eawr::presentation::godot_backend
