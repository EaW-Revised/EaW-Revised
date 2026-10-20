#pragma once

#include "eawr/campaign/game_session.hpp"
#include "encoding_internal.hpp"

namespace eawr::checkpoint::detail {

// Shared by direct restore and checkpoint inspection; leave the reader at the
// load-fixup tail so neither consumer relies on byte offsets into the session.
struct SessionPrefix {
    std::uint64_t completed{};
    Digest expected_hash{};
    std::uint64_t strategic{}, tactical{};
    std::span<const std::uint8_t> state, replay;
    campaign::HistoryBase base; // #4058: where the replay window's history starts
};
inline void write_history_base(Writer& out, const campaign::HistoryBase& base) {
    out.u64(base.at.tick); out.raw(base.at.digest);
    out.u8(base.at.last_key ? 1 : 0);
    if (base.at.last_key) { out.u64(base.at.last_key->tick); out.u32(base.at.last_key->seat.value); out.u64(base.at.last_key->sequence); }
    out.u64(base.executed_dropped); out.u64(base.native_dropped);
    out.u64(base.child_attempts.size());
    for (const auto& [conflict, attempts] : base.child_attempts) { out.u64(conflict); out.u64(attempts); }
}
// Marks the reader failed on a malformed or non-canonical record.
inline campaign::HistoryBase read_history_base(Reader& in) {
    const auto reject = [&] { (void)in.take(in.remaining() + 1); return campaign::HistoryBase{}; };
    campaign::HistoryBase base;
    base.at.tick = in.u64(); base.at.digest = in.digest();
    const auto key = in.u8();
    if (key > 1) return reject();
    if (key) { sim::galactic::CommandOrderKey value; value.tick = in.u64(); value.seat = sim::SeatId{in.u32()}; value.sequence = in.u64(); base.at.last_key = value; }
    base.executed_dropped = in.u64(); base.native_dropped = in.u64();
    const auto count = in.u64();
    if (count > in.remaining() / 16) return reject();
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto conflict = in.u64(); const auto attempts = in.u64();
        if (!base.child_attempts.empty() && conflict <= base.child_attempts.rbegin()->first) return reject();
        base.child_attempts.emplace(conflict, attempts);
    }
    return in.failed() ? campaign::HistoryBase{} : base;
}
inline SessionPrefix read_session_prefix(Reader& in) {
    const auto blob = [&] {
        const auto size = in.u64();
        return in.take(size > in.remaining() ? in.remaining() + 1 : static_cast<std::size_t>(size));
    };
    SessionPrefix result;
    result.completed = in.u64(); result.expected_hash = in.digest();
    result.strategic = in.u64(); result.tactical = in.u64();
    result.state = blob(); result.replay = blob();
    result.base = read_history_base(in);
    return result;
}

} // namespace eawr::checkpoint::detail
