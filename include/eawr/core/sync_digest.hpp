#pragma once

#include "eawr/core/sha256.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string>

namespace eawr::core {
// Portable XXH64, implemented from the published algorithm specification:
// https://github.com/Cyan4973/xxHash/blob/dev/doc/xxhash_spec.md
// Explicit little-endian reads and unsigned arithmetic make the fingerprint
// independent of compiler, alignment, host byte order and worker count.
inline std::uint64_t sync_fingerprint(std::span<const std::uint8_t> bytes, std::uint64_t seed = 0) noexcept {
    constexpr std::uint64_t p1 = 0x9e3779b185ebca87ULL, p2 = 0xc2b2ae3d27d4eb4fULL;
    constexpr std::uint64_t p3 = 0x165667b19e3779f9ULL, p4 = 0x85ebca77c2b2ae63ULL, p5 = 0x27d4eb2f165667c5ULL;
    const auto read = [&](std::size_t offset, unsigned count) {
        std::uint64_t word{};
        for (unsigned i = 0; i < count; ++i) word |= std::uint64_t(bytes[offset + i]) << (8U * i);
        return word;
    };
    const auto round = [&](std::uint64_t value, std::uint64_t word) { return std::rotl(value + word * p2, 31) * p1; };
    std::size_t offset{};
    auto digest = seed + p5;
    if (bytes.size() >= 32) {
        std::array<std::uint64_t, 4> lanes{seed + p1 + p2, seed + p2, seed, seed - p1};
        do {
            for (std::size_t i = 0; i < lanes.size(); ++i) lanes[i] = round(lanes[i], read(offset + i * 8, 8));
            offset += 32;
        } while (bytes.size() - offset >= 32);
        digest = std::rotl(lanes[0], 1) + std::rotl(lanes[1], 7) + std::rotl(lanes[2], 12) + std::rotl(lanes[3], 18);
        for (const auto lane : lanes) digest = (digest ^ round(0, lane)) * p1 + p4;
    }
    digest += bytes.size();
    while (bytes.size() - offset >= 8) {
        digest = std::rotl(digest ^ round(0, read(offset, 8)), 27) * p1 + p4;
        offset += 8;
    }
    if (bytes.size() - offset >= 4) {
        digest = std::rotl(digest ^ (read(offset, 4) * p1), 23) * p2 + p3;
        offset += 4;
    }
    while (offset < bytes.size()) digest = std::rotl(digest ^ (std::uint64_t(bytes[offset++]) * p5), 11) * p1;
    digest = (digest ^ (digest >> 33U)) * p2;
    digest = (digest ^ (digest >> 29U)) * p3;
    return digest ^ (digest >> 32U);
}

// A desync fingerprint, not an archive integrity/security hash. Two fixed
// seeds cover every canonical byte; SHA only composes this small framed record.
// Checkpoints retain their full payload SHA-256 and native canonical digest.
inline std::string sync_state_digest(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint64_t, 2> seeds{0, 0x454157522d53594eULL};
    std::array<std::uint8_t, 32> framed{'E', 'A', 'W', 'R', 'S', 'Y', 'N', 1};
    const auto put = [&](std::size_t offset, std::uint64_t word) {
        for (unsigned i = 0; i < 8; ++i) framed[offset + i] = static_cast<std::uint8_t>(word >> (8U * i));
    };
    put(8, bytes.size());
    for (std::size_t i = 0; i < seeds.size(); ++i) put(16 + i * 8, sync_fingerprint(bytes, seeds[i]));
    return sha256_hex(framed);
}
} // namespace eawr::core
