#pragma once
#include "eawr/checkpoint/checkpoint.hpp"
#include "eawr/platform/live_game_session.hpp"

namespace eawr::platform::save_detail {
// Unexecuted local transport input is outside the completed simulation hash.
// The checkpoint container's integrity digest still covers this optional section.
inline constexpr std::uint16_t input_section = 0x8001;
struct Input {
    std::vector<LiveGameOrder> orders;
    std::vector<sim::galactic::GameCommand> commands;
};
[[nodiscard]] core::Result<checkpoint::Section> encode_input(
    const sim::galactic::GalacticSetup&, std::uint64_t, const Input&);
[[nodiscard]] core::Result<Input> decode_input(const checkpoint::Checkpoint&);
// WGP-190 section 0x8002 v2: authored identity and accepted custom startup values (never hashed).
// Null when the session has no authored campaign or the seat's player has no faction name.
[[nodiscard]] std::optional<LiveSaveCampaign> save_campaign(const LiveSaveOptions&,
    const sim::galactic::GalacticSetup&, sim::galactic::SeatId);
[[nodiscard]] checkpoint::Section encode_campaign(const LiveSaveCampaign&);
// Absent section: no identity. A present but malformed section fails.
[[nodiscard]] core::Result<std::optional<LiveSaveCampaign>> decode_campaign(const checkpoint::Checkpoint&);
} // namespace eawr::platform::save_detail
