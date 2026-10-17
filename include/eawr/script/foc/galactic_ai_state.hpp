#pragma once

#include "eawr/script/foc/galactic_ai.hpp"
#include "eawr/core/record_digest.hpp"

namespace eawr::script::foc::galactic_ai {
// WGA-56/139: persist owned state, never the derived execution route matrix.
// Each component owns its domain/version and validation. The envelope preserves
// exact Number/Fixed words supplied by those components in sorted key order.
inline constexpr std::uint32_t native_state_version = 1;
inline constexpr std::uint32_t native_sync_digest_version = 2;
inline constexpr std::size_t native_state_max_bytes = 32U * 1024U * 1024U;
inline constexpr std::size_t native_state_max_components = 64;
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_native_state(const NativeState&);
[[nodiscard]] core::Result<NativeState> decode_native_state(std::span<const std::uint8_t>);
[[nodiscard]] core::Result<std::string> native_state_hash(const NativeState&);
using NativeDigests = std::map<std::string, std::string, std::less<>>;
// Versioned fixed-size composition; payload encoding remains unchanged.
[[nodiscard]] std::string compose_native_digests(const NativeDigests&);
[[nodiscard]] std::string compose_native_sync_digests(const NativeDigests&);

using FundingState = gc::funding::State;
// Complete C DTO: category priorities/balances, live and dead obligations,
// interval and tactical switch. Restoring references/semantic validation belongs
// to the live funding owner after player/goal identities have been restored.
void update_funding_records(core::RecordDigest&, gc::PlayerId, const gc::funding::Ledger*, const gc::funding::Ledger*);
void update_funding_metadata(core::RecordDigest&, const FundingState&);
[[nodiscard]] core::RecordDigest funding_records(const FundingState&);
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_funding_state(const FundingState&);
[[nodiscard]] core::Result<FundingState> decode_funding_state(std::span<const std::uint8_t>);
} // namespace eawr::script::foc::galactic_ai
