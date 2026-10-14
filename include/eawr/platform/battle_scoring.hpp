#pragma once

#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/tactical_scoring.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/tactical/snapshot.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace eawr::platform {

// FD-71/72: owned pre-mode scoring. A separate instance may consume snapshots
// for derived results (WBF-45/46), without writing the authoritative AI gate.
class BattleScoring final : public script::authoritative::TacticalScoring {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<BattleScoring>> create(
        const vfs::Vfs& files, const data::Catalog& catalog, const units::UnitTables& tables,
        std::map<sim::tactical::PlayerId, std::string> factions, std::string map);
    ~BattleScoring();
    [[nodiscard]] static core::Result<std::unique_ptr<BattleScoring>> from_scripts(
        script::authoritative::ModuleManifest manifest, std::string module, std::string map);
    [[nodiscard]] core::Result<std::unique_ptr<BattleScoring>> fresh() const;
    [[nodiscard]] core::Result<std::optional<bool>> visit(const sim::tactical::TacticalSnapshot&) override;
    [[nodiscard]] core::Result<std::string> save() override;
    [[nodiscard]] core::Result<void> load(std::string_view) override;
    [[nodiscard]] core::Result<void> observe(const sim::tactical::TacticalSnapshot& snapshot);
    [[nodiscard]] core::Result<std::string> query(sim::tactical::PlayerId player, std::string_view control);
    [[nodiscard]] std::uint64_t pumps() const noexcept;
    [[nodiscard]] double combat_rating(sim::tactical::TypeId type) const noexcept;
private:
    struct Impl;
    [[nodiscard]] static core::Result<std::unique_ptr<BattleScoring>> initialize(std::unique_ptr<Impl> state);
    [[nodiscard]] core::Result<void> advance(const sim::tactical::TacticalSnapshot&, std::uint64_t target);
    explicit BattleScoring(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace eawr::platform
