#include "session_impl.hpp"
#include "session_encoding.hpp"
#include "../replay_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace eawr::sim::tactical {

void TacticalSession::Impl::append_canonical_fog_state(std::vector<std::uint8_t>& bytes) const {
        if (fog) {
            bytes.insert(bytes.end(), fog_tag.begin(), fog_tag.end());
            fog->append_state(bytes);
            if (setup.campaign) {
                bytes.insert(bytes.end(),{'G','C','F','W'});
                sim::detail::append_u64(bytes,setup.players.size());
                for(std::size_t i=0;i<setup.players.size();++i) sim::detail::append_i64(bytes,last_fog_multipliers[i].raw());
            }
        }
}

void TacticalSession::Impl::append_canonical_tracking_state(std::vector<std::uint8_t>& bytes) const {
        if (motion.avoidance) {
            bytes.insert(bytes.end(), tracking_tag.begin(), tracking_tag.end());
            for (const auto anchor : tracking_anchor) {
                sim::detail::append_u64(bytes, anchor);
            }
        }
}

} // namespace eawr::sim::tactical
