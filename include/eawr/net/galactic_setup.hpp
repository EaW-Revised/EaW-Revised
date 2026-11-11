#pragma once

#include "eawr/net/setup.hpp"
#include "eawr/sim/galactic/mp_roster.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace eawr::net {

// #3768 (GC MP M-GC1): the campaign setup offer is a second domain of the existing setup/readiness service. The
// offer envelope (kinds 40..44, wire revision 3), accept, preview, Start, refusal and the tick-zero hash barrier are
// unchanged; only SetupOffer::domain and the bytes in SetupOffer::setup differ. The skirmish domain's bytes are
// untouched. The payload carries the roster, never a galaxy: every peer derives the galaxy and the identities from
// local content and refuses an offer whose claims differ (campaign/mp_agreement.hpp).
inline constexpr std::string_view galactic_setup_domain = "galactic-campaign-v2";
// The identity domain holding the tick-zero digest each peer computed itself.
inline constexpr std::string_view galactic_tick_zero_domain = "gc-tick-zero";

enum class GalacticStart : std::uint8_t { new_campaign, checkpoint };
struct GalacticSetupPayload {
    sim::galactic::MpRoster roster;
    // Batch quotas every peer applies; they must equal the local session limits, so a peer built with other limits refuses.
    std::uint32_t command_quota{static_cast<std::uint32_t>(max_batch_commands)};
    std::uint32_t byte_quota{static_cast<std::uint32_t>(max_batch_bytes)};
    GalacticStart start{GalacticStart::new_campaign};
    std::uint64_t initial_tick{};
    // Frozen resume agreement seam; PR2 supplies distribution and owner activation.
    std::array<std::uint8_t, 32> checkpoint_digest{}, state_digest{}, frontier_digest{}, terms_digest{};
    friend bool operator==(const GalacticSetupPayload&, const GalacticSetupPayload&) = default;
};

// Canonical payload (little-endian): "GCSU", version:u32 = 3, campaign:text, difficulty:text, custom:u8,
// credits/start tech/max tech:u64, allow heroes:u8, vote timeout seconds:u32 (0: the campaign's own constant; a lobby
// always agrees a value, #6156), command quota:u32, byte quota:u32, factions (count:u32; player,
// faction id, flags:u32, name:text, active:u8, dropout:u8, AI configuration:blob), seats (count:u32; seat, player:u32,
// kind:u8, lobby slot:u32, active:u8, side:u32), leads (count:u32; side, lead:u32, observer AI:u8), tick-zero claim:32.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_galactic_setup(const GalacticSetupPayload& payload);
[[nodiscard]] core::Result<GalacticSetupPayload> decode_galactic_setup(std::span<const std::uint8_t> bytes);

// Message of the guest refusal when the host's offer differs from the offer the guest derived itself from the frozen
// room choices. For a campaign the message may continue with ": <part>" naming the first differing part of the payload,
// found by galactic_setup_difference (text only: the refusal frame is unchanged).
inline constexpr std::string_view setup_offer_differs_message = "host offer differs from frozen room choices";
inline constexpr std::string_view setup_difference_ai_configuration = "ai-configuration";
inline constexpr std::string_view setup_difference_start_state = "start-state";
// The part two canonical campaign payloads disagree on, or empty when they decode equal in the named parts, cannot be
// decoded, or differ only in the room choices (campaign, roster, seats), which the caller already compares.
[[nodiscard]] std::string_view galactic_setup_difference(std::span<const std::uint8_t> host, std::span<const std::uint8_t> local);

// The offer-level rules of the domain: a canonical payload, a roster the content-free rules accept, a peer for every
// active human seat and no other, the quotas the local session applies and an identity that carries the same tick-zero
// claim as the payload. Called by the offer encoder and decoder for galactic_setup_domain.
[[nodiscard]] core::Result<void> validate_galactic_offer(const SetupOffer& offer);

} // namespace eawr::net
