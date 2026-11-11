#pragma once
#include "eawr/checkpoint/checkpoint.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/sim/galactic/offer_pool.hpp"

namespace eawr::platform::save_detail {
// Unexecuted local transport input is outside the completed simulation hash.
// The checkpoint container's integrity digest still covers this optional section.
inline constexpr std::uint16_t input_section = 0x8001;
struct Input {
    std::vector<LiveGameOrder> orders;
    std::vector<sim::galactic::GameCommand> commands;
};
// The section's two recordings carry the setup. With a pool (version 4) its production offer tables are written as
// indices into that pool, which must be the pool of the checkpoint the section joins (decode_input reads it from the
// container's offer-pool section); without one (version 3) the tables stay inline. A table the pool lacks is refused.
[[nodiscard]] core::Result<checkpoint::Section> encode_input(
    const sim::galactic::GalacticSetup&, std::uint64_t, const Input&, sim::galactic::OfferPool* pool = nullptr);
// The offer pool of a galaxy checkpoint container (its required section 21).
[[nodiscard]] core::Result<sim::galactic::OfferPool> read_offer_pool(const checkpoint::Checkpoint&);
[[nodiscard]] core::Result<Input> decode_input(const checkpoint::Checkpoint&);
// WGP-190 section 0x8002 v2: authored identity and accepted custom startup values (never hashed).
// Null when the session has no authored campaign or the seat's player has no faction name.
[[nodiscard]] std::optional<LiveSaveCampaign> save_campaign(const LiveSaveOptions&,
    const sim::galactic::GalacticSetup&, sim::galactic::SeatId);
[[nodiscard]] checkpoint::Section encode_campaign(const LiveSaveCampaign&);
// Absent section: no identity. A present but malformed section fails.
[[nodiscard]] core::Result<std::optional<LiveSaveCampaign>> decode_campaign(const checkpoint::Checkpoint&);
} // namespace eawr::platform::save_detail
