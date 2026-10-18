#pragma once

#include "encoding_internal.hpp"

namespace eawr::checkpoint::detail {

// Shared by direct restore and checkpoint inspection; leave the reader at the
// load-fixup tail so neither consumer relies on byte offsets into the session.
struct SessionPrefix {
    std::uint64_t completed{};
    Digest expected_hash{};
    std::uint64_t strategic{}, tactical{};
    std::span<const std::uint8_t> state, replay;
};
inline SessionPrefix read_session_prefix(Reader& in) {
    const auto blob = [&] {
        const auto size = in.u64();
        return in.take(size > in.remaining() ? in.remaining() + 1 : static_cast<std::size_t>(size));
    };
    SessionPrefix result;
    result.completed = in.u64(); result.expected_hash = in.digest();
    result.strategic = in.u64(); result.tactical = in.u64();
    result.state = blob(); result.replay = blob();
    return result;
}

} // namespace eawr::checkpoint::detail
