#pragma once

#include "eawr/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The EAWRCKP checkpoint container (ADR-016, docs/checkpoint-format.md): one canonical,
// uncompressed, sectioned format for player saves, multiplayer resync and join. The container
// knows sections as bytes; each section's owner encodes and validates its own bytes.
namespace eawr::checkpoint {

using Digest = std::array<std::uint8_t, 32>;

inline constexpr std::array<std::uint8_t, 8> magic{'E', 'A', 'W', 'R', 'C', 'K', 'P', '\0'};
inline constexpr std::uint16_t format_version = 1;
inline constexpr std::size_t header_size = 304;
inline constexpr std::size_t section_header_size = 16;
// The multiplayer-readiness budget for one uncompressed checkpoint. A save over it fails; nothing
// is ever truncated.
inline constexpr std::size_t max_bytes = 64U * 1024U * 1024U;
inline constexpr std::size_t max_sections = 64;

inline constexpr std::uint16_t flag_child = 1U << 0U;    // a tactical battle is in progress
inline constexpr std::uint16_t flag_galaxy = 1U << 1U;   // galaxy sections are present
inline constexpr std::uint16_t flag_frontier = 1U << 2U; // at least one command is sealed
inline constexpr std::uint16_t known_flags = flag_child | flag_galaxy | flag_frontier;

inline constexpr std::uint16_t section_flag_required = 1U << 0U;

// Section IDs, written in ascending order. IDs from 0x8000 are never part of a state hash.
enum class SectionId : std::uint16_t {
    session = 1,
    seats = 2,
    players = 3,
    planets = 4,
    fleets = 5,
    production = 6,
    conflicts = 7,
    victory = 8,
    scripts = 9,
    child_log = 10,
    child_state = 11,
    mod_list = 12,
    tactical_return = 13,
    galactic_ai = 14,
    galactic_abilities = 15,
    campaign_scoring = 16, // coordinator-reserved, GHUD-S61 ledger bytes (empty when no counter exists)
    galactic_owner = 17, // complete direct owner/barrier state
    presentation_hints = 0x8000,
    // 0x8001: the live transport's queued input (platform-owned).
    campaign_identity = 0x8002, // #3035: authored campaign/faction a player save belongs to
};

[[nodiscard]] std::string_view section_name(std::uint16_t id) noexcept;
[[nodiscard]] bool known_section(std::uint16_t id) noexcept;

// What a build can load: a checkpoint loads only into a build whose every identity is equal
// ("saved by another build" otherwise). A zero digest means "none" (no Lua state, no campaign,
// no mods) and must be zero on both sides.
struct Identities {
    std::uint32_t rules_version{};
    std::uint32_t state_encoding_version{};
    Digest numeric_abi{};
    Digest lua_save{};
    Digest content{};
    Digest campaign{};
    Digest mod_list{};
    friend bool operator==(const Identities&, const Identities&) = default;
};

// The sealed-input frontier: the canonical key of the last sealed command and the digest of
// every input up to it. Its owner (the session that issues the commands) defines the digest.
struct InputFrontier {
    std::uint64_t tick{};
    std::uint32_t seat{};
    std::uint64_t sequence{};
    Digest input_digest{};
    friend bool operator==(const InputFrontier&, const InputFrontier&) = default;
};

struct Header {
    std::uint16_t flags{};
    Identities identities;
    std::uint64_t seed{};
    std::uint64_t completed_tick{};
    std::uint64_t galactic_frame{};
    InputFrontier frontier; // all zero unless flags has flag_frontier
    friend bool operator==(const Header&, const Header&) = default;
};

struct Section {
    std::uint16_t id{};
    std::uint16_t version{};
    bool required{};
    std::vector<std::uint8_t> bytes;
    friend bool operator==(const Section&, const Section&) = default;
};

struct Checkpoint {
    Header header;
    std::vector<Section> sections; // strictly ascending id
    [[nodiscard]] const Section* find(SectionId id) const noexcept;
    friend bool operator==(const Checkpoint&, const Checkpoint&) = default;
};

// Fails without output on an unordered or duplicate section, an unknown flag, more than
// max_sections sections or more than max_bytes in all.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> write_checkpoint(const Checkpoint& checkpoint);
// Validates the whole container: bounds, magic, version, flags, payload length and SHA-256,
// section framing and order, and refuses an unknown section marked required. An unknown optional
// section is kept and ignored. Section contents are their owners' to validate.
[[nodiscard]] core::Result<Checkpoint> parse_checkpoint(
    std::span<const std::uint8_t> bytes,
    std::string_view logical_path = {});
// "Saved by another build": names the first identity that differs.
[[nodiscard]] core::Result<void> check_identities(const Header& header, const Identities& build,
    std::string_view logical_path = {});

namespace diagnostic_codes {
inline constexpr std::string_view malformed = "EAWR-CKP-0101";
inline constexpr std::string_view version = "EAWR-CKP-0102";
inline constexpr std::string_view other_build = "EAWR-CKP-0103";
inline constexpr std::string_view resource_limit = "EAWR-CKP-0104";
inline constexpr std::string_view integrity = "EAWR-CKP-0105";
inline constexpr std::string_view section_order = "EAWR-CKP-0106";
inline constexpr std::string_view unknown_required_section = "EAWR-CKP-0107";
inline constexpr std::string_view section_invalid = "EAWR-CKP-0108";
inline constexpr std::string_view inconsistent = "EAWR-CKP-0109";
inline constexpr std::string_view restore_failed = "EAWR-CKP-0110";
inline constexpr std::string_view invalid_save = "EAWR-CKP-0111";
} // namespace diagnostic_codes

} // namespace eawr::checkpoint
