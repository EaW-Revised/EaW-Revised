#include "battle_source_internal.hpp"

namespace eawr::presentation::godot_backend {

LanSource::LanSource(const LocalStartKind kind) : kind_(kind) {}

bool LanSource::prepare(BattlePreparation& prepared, const vfs::Vfs& files,
    const data::Catalog& catalog, std::string_view, std::string& failure) const {
    auto& state = prepared.state();
    // Keep the agreed recording override inside the shared builder, before the
    // per-peer player and ship bindings. Recovery keeps the same AI installer.
    return state.prepare_local(files, catalog, failure, kind_);
}

BattleAiCapability LanSource::ai_capability() const noexcept { return BattleAiCapability::configurable; }

} // namespace eawr::presentation::godot_backend
