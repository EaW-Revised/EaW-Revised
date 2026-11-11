#pragma once
#include "eawr/mods/chain.hpp"
#include "eawr/mods/profile.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::mods {

inline constexpr std::size_t kMaxDescriptionLayers = 256;
inline constexpr std::size_t kMaxDescriptionString = 4096;
inline constexpr std::size_t kMaxDescriptionBytes = 1024 * 1024;
enum class DescriptionKeyKind : std::uint8_t { unknown = 0, workshop = 1, mods_folder = 2, manual = 3 };
struct DescriptionEntry {
    DescriptionKeyKind kind = DescriptionKeyKind::unknown;
    std::string key; // Empty for unknown and manual identities.
    std::string name;
    std::optional<std::string> version;
    std::optional<std::array<std::uint8_t, 32>> checksum;
    std::optional<std::array<std::uint8_t, 32>> portable_tree{};
    friend bool operator==(const DescriptionEntry&, const DescriptionEntry&) = default;
};
struct ModDescription {
    bool complete = false; // False means the mounted chain was not fully described.
    std::vector<DescriptionEntry> entries;
    friend bool operator==(const ModDescription&, const ModDescription&) = default;
};

// Version 3, little endian. Both operations reject invalid or unrepresentable descriptions.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_description(const ModDescription& description);
[[nodiscard]] std::optional<ModDescription> decode_description(std::span<const std::uint8_t> bytes);
[[nodiscard]] ModDescription describe_chain(const Chain& chain, std::span<const ModRecord> records);
// The description of what a launch actually mounted: the profile's layers in priority order (layer zero first),
// resolved against `records`. Entries carry the portable key (incl. "#variant:"), name, authored version and, only for
// records whose content digest is ready, the 32-byte checksum. Paths and aliases never enter the result.
[[nodiscard]] ModDescription describe_mounted_profile(const Profile& profile, std::span<const ModRecord> records);

[[nodiscard]] bool matching_portable_trees(const ModDescription& host, const ModDescription& local);

enum class FindingKind { missing, extra, reordered, version_changed, content_changed, checksum_unavailable, unknown_identity, unknown_version, incomplete };
struct DescriptionFinding {
    FindingKind kind;
    DescriptionEntry mod;
    std::string saved_detail;
    std::string current_detail;
    friend bool operator==(const DescriptionFinding&, const DescriptionFinding&) = default;
};
[[nodiscard]] std::vector<DescriptionFinding> compare_descriptions(const ModDescription& saved, const ModDescription& current);
// Only definite differences prompt; unavailable identities, versions and checksums remain advisory findings.
[[nodiscard]] bool description_needs_confirmation(const ModDescription& saved, const ModDescription& current);
[[nodiscard]] std::string describe_finding(const DescriptionFinding& finding);
} // namespace eawr::mods
