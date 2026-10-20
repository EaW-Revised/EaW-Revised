#include "battle_source_internal.hpp"

namespace eawr::presentation::godot_backend {

BattleSource::~BattleSource() = default;

void BattleSourceState::select_arrivals() {
    arrivals_.reset();
    if (!start_ || !tables_) return;
    arrivals_ = start_->arrivals;
    // Restored starts can precede economy binding; finalize their deployment mapping here.
    if (arrivals_->rows.empty() || (setup_ && setup_->campaign))
        arrivals_ = battle::plan_start_arrivals(*start_, *tables_, &economy_);
}

BattlePreparation::BattlePreparation(const live_session::Options& options)
    : state_(std::make_unique<BattleSourceState>(options)) {}
BattlePreparation::~BattlePreparation() = default;
BattleSourceState& BattlePreparation::state() noexcept { return *state_; }

const BattleSource* local_battle_source(const std::string_view fixture) {
    static const LocalSkirmishSource m2(LocalStartKind::m2);
    static const LocalSkirmishSource selected(LocalStartKind::selected);
    static const AttractSource attract;
    static const ReplaySource replay;
    static const MeleeSource melee;
    if (fixture == "attract") return &attract;
    if (fixture == "replay") return &replay;
    if (fixture == "melee") return &melee;
    if (fixture == "m2") return &m2;
    if (fixture == "skirmish") return &selected;
    return nullptr;
}

const BattleSource* battle_source(const live_session::Options& options) {
    static const LanSource lan_m2(LocalStartKind::m2);
    static const LanSource lan_selected(LocalStartKind::selected);
    static const BorrowedBattleSource borrowed;
    const bool recorded = options.fixture == "replay" || options.fixture == "melee";
    if (options.borrowed_battle && recorded) return &borrowed;
    if (options.lan && options.fixture == "m2") return &lan_m2;
    if (options.lan && options.fixture == "skirmish") return &lan_selected;
    return local_battle_source(options.fixture);
}

} // namespace eawr::presentation::godot_backend
