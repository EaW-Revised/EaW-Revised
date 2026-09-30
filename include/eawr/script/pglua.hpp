#pragma once

#include "eawr/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::script {

struct RetailIdentity {
    std::string profile;
    std::string archive_sha256;
};

struct PgluaLocation {
    std::int32_t prototype_id{0};
    std::uint32_t pc{0};
};

struct PgluaPrototypeInfo {
    std::int32_t persistence_id{0};
    std::vector<std::uint32_t> path;
    std::uint32_t instruction_count{0};
};

struct ConvertedChunk {
    std::vector<std::byte> bytes;
    std::vector<PgluaPrototypeInfo> prototypes;
    std::vector<PgluaLocation> unsupported_execution;
};

// Decode the fixed-width retail representation, validate the bounded proven
// subset, and emit a standard Lua 5.0.2 chunk for this process's native ABI.
// Standalone synthetic chunks omit retail_identity.  Retail-profile callers
// must supply a recognized profile/archive pair.
[[nodiscard]] core::Result<ConvertedChunk> convert_pglua(
    std::span<const std::byte> input,
    std::string logical_source,
    std::optional<RetailIdentity> retail_identity = std::nullopt
);

namespace diagnostic_codes {
inline constexpr std::string_view invalid_instance = "EAWR-SCRIPT-0001";
inline constexpr std::string_view invalid_value = "EAWR-SCRIPT-0002";
inline constexpr std::string_view load_parse = "EAWR-SCRIPT-0003";
inline constexpr std::string_view lua_execution = "EAWR-SCRIPT-0004";
inline constexpr std::string_view missing_engine_api = "EAWR-SCRIPT-0005";
inline constexpr std::string_view unsupported_feature = "EAWR-SCRIPT-0006";
inline constexpr std::string_view resource_limit = "EAWR-SCRIPT-0007";
} // namespace diagnostic_codes

} // namespace eawr::script
