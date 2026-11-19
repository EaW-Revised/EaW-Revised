#include "galactic_ai_outbox.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace eawr::script::foc::galactic_ai {

LaneIdentities OutboxBatch::begin(std::uint64_t snapshot_tick, LaneIdentities adopted_frontier, std::uint64_t adopted_floor,
    LaneIdentities live) {
    abandon(); // a batch left open by a failed attempt never reaches the world
    const auto gap = identity_gap(schedule_);
    contents_.snapshot_tick = snapshot_tick; virtual_tick_ = 0; open_ = true;
    contents_.floor = adopted_floor ? adopted_floor : live.id;
    const LaneIdentities start{std::max(adopted_frontier.id, live.id + gap), std::max(adopted_frontier.invasion, live.invasion + gap)};
    // The frontier is the end of the batch's fixed block, known now rather than after the window.
    const auto block = identity_block(schedule_);
    contents_.frontier = {start.id + block, start.invasion + block};
    return start;
}

IntentSink OutboxBatch::sink() {
    return [this](sim::galactic::PlayerId player, IntentRecord record) {
        auto* slice = open_slice();
        if (!slice) return;
        contents_.frontier.id = std::max(contents_.frontier.id, record.next_id);
        contents_.frontier.invasion = std::max(contents_.frontier.invasion, record.next_invasion);
        auto hooks = factory_ ? factory_(player, record) : IntentHooks{};
        slice->intents.push_back(PlayerIntent{player, std::move(record), std::move(hooks)});
    };
}

OutboxSlice* OutboxBatch::open_slice() {
    if (!open_ || !virtual_tick_ || virtual_tick_ > schedule_.batch_ticks) return nullptr; // outside a virtual tick: nothing to apply
    // Slices are created in virtual-tick order, so the open one is the last.
    auto& slices = contents_.slices;
    if (slices.empty() || slices.back().virtual_tick != virtual_tick_) {
        OutboxSlice slice;
        slice.snapshot_tick = contents_.snapshot_tick; slice.virtual_tick = virtual_tick_;
        slice.apply_tick = apply_tick(schedule_, contents_.snapshot_tick, virtual_tick_);
        slices.push_back(std::move(slice));
    }
    return &slices.back();
}

bool OutboxBatch::add_commands(std::vector<authoritative::ScriptCommand>& commands) {
    if (commands.empty()) return open_;
    auto* slice = open_slice();
    if (!slice) return false;
    slice->commands.insert(slice->commands.end(), std::make_move_iterator(commands.begin()), std::make_move_iterator(commands.end()));
    commands.clear();
    return true;
}

bool OutboxBatch::holds_slice(std::uint64_t snapshot, std::uint32_t virtual_tick_index) const noexcept {
    return std::any_of(contents_.slices.begin(), contents_.slices.end(), [&](const OutboxSlice& slice) {
        return slice.snapshot_tick == snapshot && slice.virtual_tick == virtual_tick_index;
    });
}

bool OutboxBatch::holds_invasion(std::uint64_t handle) const noexcept {
    if (!handle) return false;
    for (const auto& slice : contents_.slices) for (const auto& intent : slice.intents)
        if (intent.record.first_invasion <= handle && handle < intent.record.next_invasion) return true;
    return false;
}

std::uint64_t OutboxBatch::lowest_invasion() const noexcept {
    std::uint64_t floor{};
    for (const auto& slice : contents_.slices) for (const auto& intent : slice.intents)
        if (intent.record.first_invasion && (!floor || intent.record.first_invasion < floor)) floor = intent.record.first_invasion;
    return floor;
}

std::size_t OutboxBatch::intents() const noexcept {
    std::size_t total{};
    for (const auto& slice : contents_.slices) total += slice.intents.size();
    return total;
}

OutboxBatch::Contents OutboxBatch::take() {
    auto taken = std::move(contents_);
    abandon();
    return taken;
}

void OutboxBatch::restore_closed(Contents contents) {
    contents_ = std::move(contents); open_ = false; closed_ = true; virtual_tick_ = 0;
    // Closures are not saved: the player's hooks are derived from the request again.
    if (factory_) for (auto& slice : contents_.slices) for (auto& intent : slice.intents) intent.hooks = factory_(intent.player, intent.record);
}

void IntentOutbox::adopt(OutboxBatch::Contents contents) {
    // The batch began over the frontier and floor adopted so far; nothing but batches moves them, so its own values continue them.
    frontier_ = contents.frontier; floor_ = contents.floor;
    slices_.insert(slices_.end(), std::make_move_iterator(contents.slices.begin()), std::make_move_iterator(contents.slices.end()));
    ++generation_;
}

LaneIdentities IntentOutbox::begin_batch(std::uint64_t snapshot_tick, LaneIdentities live) {
    ++generation_;
    return own_.begin(snapshot_tick, frontier_, floor_, live);
}

void IntentOutbox::begin_virtual_tick(std::uint32_t virtual_tick) {
    own_.begin_virtual_tick(virtual_tick);
}

IntentSink IntentOutbox::sink() {
    return [this, inner = own_.sink()](sim::galactic::PlayerId player, IntentRecord record) {
        ++generation_;
        inner(player, std::move(record));
    };
}

void IntentOutbox::restore(LaneIdentities frontier, std::uint64_t floor, std::vector<OutboxSlice> slices) {
    own_.abandon();
    slices_ = std::move(slices); frontier_ = frontier; floor_ = slices_.empty() ? 0 : floor; ++generation_;
    // Closures are not saved: the player's hooks are derived from the request again.
    if (factory_) for (auto& slice : slices_) for (auto& intent : slice.intents) intent.hooks = factory_(intent.player, intent.record);
}

bool IntentOutbox::add_commands(std::vector<authoritative::ScriptCommand>& commands) {
    const bool kept = own_.add_commands(commands);
    if (kept) ++generation_;
    return kept;
}

void IntentOutbox::end_batch() {
    if (!own_.open()) return;
    own_.end();
    adopt(own_.take());
}

void IntentOutbox::abandon_batch() {
    if (!own_.open()) return;
    own_.abandon();
    if (slices_.empty()) floor_ = 0;
    ++generation_;
}

std::vector<OutboxSlice> IntentOutbox::take_due(std::uint64_t tick) {
    // An open batch is never released: its slices are not in the outbox until end_batch.
    std::vector<OutboxSlice> due;
    std::size_t taken{};
    while (taken < slices_.size() && slices_[taken].apply_tick <= tick) ++taken;
    due.reserve(taken);
    for (std::size_t index = 0; index < taken; ++index) due.push_back(std::move(slices_[index]));
    slices_.erase(slices_.begin(), slices_.begin() + static_cast<std::ptrdiff_t>(taken));
    if (taken) ++generation_;
    if (slices_.empty() && !own_.open()) floor_ = 0;
    return due;
}

std::uint64_t IntentOutbox::lane_invasion_floor() const noexcept {
    // Not kept and not saved: every pending invasion handle lies in a block the world reserved when its window began, so the
    // lowest pending handle judges staleness exactly as the world's counter at the oldest batch start would, and a reloaded
    // outbox derives the same value. With this floor no pending invasion is ever stale: their safety rests on the reserved block and
    // the readmission's in-use guard.
    std::uint64_t floor = own_.lowest_invasion();
    for (const auto& slice : slices_) for (const auto& intent : slice.intents)
        if (intent.record.first_invasion && (!floor || intent.record.first_invasion < floor)) floor = intent.record.first_invasion;
    return floor;
}

bool IntentOutbox::holds_invasion(std::uint64_t handle) const noexcept {
    if (!handle) return false;
    if (own_.holds_invasion(handle)) return true;
    for (const auto& slice : slices_) for (const auto& intent : slice.intents)
        if (intent.record.first_invasion <= handle && handle < intent.record.next_invasion) return true;
    return false;
}

std::size_t IntentOutbox::pending_intents() const noexcept {
    std::size_t total = own_.intents();
    for (const auto& slice : slices_) total += slice.intents.size();
    return total;
}

std::optional<std::uint64_t> IntentOutbox::next_apply_tick() const noexcept {
    if (slices_.empty()) return std::nullopt;
    return slices_.front().apply_tick;
}

} // namespace eawr::script::foc::galactic_ai
