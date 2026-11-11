#include "galactic_ai_outbox.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace eawr::script::foc::galactic_ai {

LaneIdentities IntentOutbox::begin_batch(std::uint64_t snapshot_tick, LaneIdentities live) {
    abandon_batch(); // a batch left open by a failed attempt never reaches the world
    const auto gap = identity_gap(schedule_);
    before_batch_ = frontier_;
    batch_first_slice_ = slices_.size();
    snapshot_tick_ = snapshot_tick; virtual_tick_ = 0; open_ = true;
    if (!floor_) floor_ = live.id;
    const LaneIdentities start{std::max(frontier_.id, live.id + gap), std::max(frontier_.invasion, live.invasion + gap)};
    // The frontier is the end of the batch's fixed block, known now rather than after the window.
    const auto block = identity_block(schedule_);
    frontier_ = {start.id + block, start.invasion + block};
    ++generation_;
    return start;
}

void IntentOutbox::begin_virtual_tick(std::uint32_t virtual_tick) {
    virtual_tick_ = virtual_tick;
}

IntentSink IntentOutbox::sink() {
    return [this](sim::galactic::PlayerId player, IntentRecord record) { add(player, std::move(record)); };
}

OutboxSlice* IntentOutbox::open_slice() {
    if (!open_ || !virtual_tick_ || virtual_tick_ > schedule_.batch_ticks) return nullptr; // outside a virtual tick: nothing to apply
    // Slices of one batch are created in virtual-tick order, so the open one is the last.
    if (slices_.size() == batch_first_slice_ || slices_.back().virtual_tick != virtual_tick_) {
        OutboxSlice slice;
        slice.snapshot_tick = snapshot_tick_; slice.virtual_tick = virtual_tick_;
        slice.apply_tick = apply_tick(schedule_, snapshot_tick_, virtual_tick_);
        slices_.push_back(std::move(slice));
    }
    return &slices_.back();
}

void IntentOutbox::add(sim::galactic::PlayerId player, IntentRecord record) {
    auto* slice = open_slice();
    if (!slice) return;
    frontier_.id = std::max(frontier_.id, record.next_id);
    frontier_.invasion = std::max(frontier_.invasion, record.next_invasion);
    auto hooks = factory_ ? factory_(player, record) : IntentHooks{};
    ++generation_;
    slice->intents.push_back(PlayerIntent{player, std::move(record), std::move(hooks)});
}

void IntentOutbox::restore(LaneIdentities frontier, std::uint64_t floor, std::vector<OutboxSlice> slices) {
    slices_ = std::move(slices); frontier_ = frontier; floor_ = slices_.empty() ? 0 : floor; before_batch_ = frontier; ++generation_;
    open_ = false; batch_first_slice_ = 0; virtual_tick_ = 0;
    // Closures are not saved: the player's hooks are derived from the request again.
    if (factory_) for (auto& slice : slices_) for (auto& intent : slice.intents) intent.hooks = factory_(intent.player, intent.record);
}

bool IntentOutbox::add_commands(std::vector<authoritative::ScriptCommand>& commands) {
    if (commands.empty()) return open_;
    auto* slice = open_slice();
    if (!slice) return false;
    slice->commands.insert(slice->commands.end(), std::make_move_iterator(commands.begin()), std::make_move_iterator(commands.end()));
    commands.clear(); ++generation_;
    return true;
}

void IntentOutbox::end_batch() {
    open_ = false; virtual_tick_ = 0;
}

void IntentOutbox::abandon_batch() {
    if (!open_) return;
    slices_.resize(batch_first_slice_);
    if (slices_.empty()) floor_ = 0;
    frontier_ = before_batch_; ++generation_;
    open_ = false; virtual_tick_ = 0;
}

std::vector<OutboxSlice> IntentOutbox::take_due(std::uint64_t tick) {
    // An open batch is never released: its slices are not final until end_batch.
    const auto closed = open_ ? batch_first_slice_ : slices_.size();
    std::vector<OutboxSlice> due;
    std::size_t taken{};
    while (taken < closed && slices_[taken].apply_tick <= tick) ++taken;
    due.reserve(taken);
    for (std::size_t index = 0; index < taken; ++index) due.push_back(std::move(slices_[index]));
    slices_.erase(slices_.begin(), slices_.begin() + static_cast<std::ptrdiff_t>(taken));
    if (taken) ++generation_;
    if (slices_.empty() && !open_) floor_ = 0;
    batch_first_slice_ = open_ ? batch_first_slice_ - taken : 0;
    return due;
}

std::uint64_t IntentOutbox::lane_invasion_floor() const noexcept {
    // Not kept and not saved: every pending invasion handle lies in a block the world reserved when its window began, so the
    // lowest pending handle judges staleness exactly as the world's counter at the oldest batch start would, and a reloaded
    // outbox derives the same value. With this floor no pending invasion is ever stale: their safety rests on the reserved block and
    // the readmission's in-use guard.
    std::uint64_t floor{};
    for (const auto& slice : slices_) for (const auto& intent : slice.intents)
        if (intent.record.first_invasion && (!floor || intent.record.first_invasion < floor)) floor = intent.record.first_invasion;
    return floor;
}

bool IntentOutbox::holds_invasion(std::uint64_t handle) const noexcept {
    if (!handle) return false;
    for (const auto& slice : slices_) for (const auto& intent : slice.intents)
        if (intent.record.first_invasion <= handle && handle < intent.record.next_invasion) return true;
    return false;
}

std::size_t IntentOutbox::pending_intents() const noexcept {
    std::size_t total{};
    for (const auto& slice : slices_) total += slice.intents.size();
    return total;
}

std::optional<std::uint64_t> IntentOutbox::next_apply_tick() const noexcept {
    if (slices_.empty()) return std::nullopt;
    return slices_.front().apply_tick;
}

} // namespace eawr::script::foc::galactic_ai
