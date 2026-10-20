#include "battle_source_internal.hpp"

namespace eawr::presentation::godot_backend {

bool BattleSourceState::prepare_borrowed(std::string& failure) {
    return prepare_native(*options_.borrowed_battle, failure);
}

bool BorrowedBattleSource::prepare(BattlePreparation& prepared, const vfs::Vfs&,
    const data::Catalog&, std::string_view, std::string& failure) const {
    // The child feed and its script owner already exist; bind their prepared
    // presentation only. start() retains the notification/command-source handoff.
    return prepared.state().prepare_borrowed(failure);
}

BattleAiCapability BorrowedBattleSource::ai_capability() const noexcept { return BattleAiCapability::borrowed; }

} // namespace eawr::presentation::godot_backend
