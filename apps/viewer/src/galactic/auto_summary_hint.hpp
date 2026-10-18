#pragma once

#include "eawr/platform/live_game_session.hpp"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace eawr::viewer::galactic {
// WGS-P01: one presentation snapshot, never the runtime outcome-history ring.
using platform::encode_auto_summary_hint;
using platform::decode_auto_summary_hint;
inline platform::LiveGameFrame frame_with_auto_summary(platform::LiveGameFrame frame,
    const std::shared_ptr<const platform::LiveAutoResolveOutcome>& saved) {
    if (!frame.auto_resolve_outcome) frame.auto_resolve_outcome = saved;
    return frame;
}
} // namespace eawr::viewer::galactic
