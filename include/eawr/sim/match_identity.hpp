#pragma once

#include "eawr/core/result.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::sim {

struct IdentityDomain {
    std::string name;
    std::uint32_t version{1};
    std::array<std::uint8_t, 32> digest{};
    friend bool operator==(const IdentityDomain&, const IdentityDomain&) = default;
};

// MPS-6: GC can append campaign/story domains without a second identity loader.
// Build identity is handshake-only; content identity survives executable rebuilds.
struct MatchIdentity {
    std::uint32_t version{1};
    std::vector<IdentityDomain> domains; // strictly increasing ASCII names
    std::array<std::uint8_t, 32> sha256{};
    friend bool operator==(const MatchIdentity&, const MatchIdentity&) = default;
};

inline constexpr std::string_view match_identity_mismatch_code = "EAWR-MATCH-0001";
inline constexpr std::string_view match_identity_invalid_code = "EAWR-MATCH-0002";
inline constexpr std::string_view match_identity_encoding = "eawr-match-identity";

[[nodiscard]] inline IdentityDomain identity_domain(std::string name, std::string_view bytes,
    std::uint32_t version = 1) {
    return {std::move(name), version, core::sha256(std::span(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()))};
}

// Domain separator, LE u32 version/count, then LE u32 name length, ASCII name,
// LE u32 domain version and 32 digest bytes per domain. SHA excludes itself.
[[nodiscard]] inline std::vector<std::uint8_t> canonical_match_identity(const MatchIdentity& value) {
    std::vector<std::uint8_t> bytes;
    const auto word = [&](std::uint32_t number) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(number >> shift));
    };
    bytes.insert(bytes.end(), match_identity_encoding.begin(), match_identity_encoding.end());
    word(value.version);
    word(static_cast<std::uint32_t>(value.domains.size()));
    for (const auto& domain : value.domains) {
        word(static_cast<std::uint32_t>(domain.name.size()));
        bytes.insert(bytes.end(), domain.name.begin(), domain.name.end());
        word(domain.version);
        bytes.insert(bytes.end(), domain.digest.begin(), domain.digest.end());
    }
    return bytes;
}

[[nodiscard]] inline MatchIdentity compute_match_identity(MatchIdentity components) {
    std::sort(components.domains.begin(), components.domains.end(), [](const auto& a, const auto& b) {
        return a.name < b.name;
    });
    components.sha256 = core::sha256(canonical_match_identity(components));
    return components;
}

[[nodiscard]] inline core::Result<void> validate_match_identity(const MatchIdentity& identity);

[[nodiscard]] inline std::optional<core::Diagnostic> describe_mismatch(
    const MatchIdentity& expected, const MatchIdentity& actual) {
    std::string different;
    if (expected.version != actual.version) different = "identity version";
    std::size_t left = 0, right = 0;
    while (different.empty() && (left < expected.domains.size() || right < actual.domains.size())) {
        if (left == expected.domains.size()) different = actual.domains[right].name;
        else if (right == actual.domains.size()) different = expected.domains[left].name;
        else if (expected.domains[left].name < actual.domains[right].name) different = expected.domains[left].name;
        else if (actual.domains[right].name < expected.domains[left].name) different = actual.domains[right].name;
        else if (expected.domains[left] != actual.domains[right]) different = expected.domains[left].name;
        else { ++left; ++right; }
    }
    if (different.empty() && expected.sha256 != actual.sha256) different = "combined SHA-256";
    if (different.empty()) {
        if (const auto valid = validate_match_identity(expected); !valid) return valid.error();
        return std::nullopt;
    }
    core::Diagnostic diagnostic;
    diagnostic.code = match_identity_mismatch_code;
    diagnostic.message = "match identity mismatch: " + different;
    return diagnostic;
}

[[nodiscard]] inline core::Result<void> validate_match_identity(const MatchIdentity& identity) {
    bool valid = identity.version == 1 && !identity.domains.empty() && identity.domains.size() <= 64;
    std::string_view previous;
    for (const auto& domain : identity.domains) {
        valid = valid && domain.name.size() <= 128 && domain.name > previous && domain.version != 0;
        for (const auto c : domain.name) valid = valid && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-');
        previous = domain.name;
    }
    valid = valid && core::sha256(canonical_match_identity(identity)) == identity.sha256;
    if (valid) return core::Result<void>::success();
    core::Diagnostic diagnostic;
    diagnostic.code = match_identity_invalid_code;
    diagnostic.message = "invalid match identity version, domains or SHA-256";
    return core::Result<void>::failure(std::move(diagnostic));
}

struct MatchAgreement {
    std::string build;
    MatchIdentity content;
};

[[nodiscard]] inline std::optional<core::Diagnostic> describe_mismatch(
    const MatchAgreement& expected, const MatchAgreement& actual) {
    if (expected.build.empty() || actual.build.empty()) {
        core::Diagnostic diagnostic;
        diagnostic.code = match_identity_invalid_code;
        diagnostic.message = "missing handshake build identity";
        return diagnostic;
    }
    if (expected.build != actual.build) {
        core::Diagnostic diagnostic;
        diagnostic.code = match_identity_mismatch_code;
        diagnostic.message = "match identity mismatch: build";
        return diagnostic;
    }
    return describe_mismatch(expected.content, actual.content);
}

} // namespace eawr::sim
