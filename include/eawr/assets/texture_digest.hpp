#pragma once

#include "eawr/assets/assets.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace eawr::assets {

// A fast non-cryptographic 64-bit digest (word-wise multiply/xor mixing). The renderer's GPU texture identity is a pair of
// these over the same bytes, one with the default seed and one seeded with 1.
class ContentDigest final {
public:
    ContentDigest() = default;
    explicit ContentDigest(const std::uint64_t seed) : state_(initial ^ (seed * 0x9e3779b97f4a7c15ULL)) {}
    void bytes(const void* data, const std::size_t size) {
        const auto* begin = static_cast<const unsigned char*>(data);
        std::size_t offset = 0;
        for (; offset + 8 <= size; offset += 8) {
            std::uint64_t word{};
            std::memcpy(&word, begin + offset, 8);
            mix(word);
        }
        std::uint64_t tail{};
        if (offset < size) std::memcpy(&tail, begin + offset, size - offset);
        mix(tail ^ (static_cast<std::uint64_t>(size) << 56U));
    }
    // The same bytes into two digests in one pass: each ends in exactly the state `bytes` leaves it in, but the two serial
    // multiply/rotate chains run side by side instead of one after the other.
    static void bytes_both(ContentDigest& first, ContentDigest& second, const void* data, const std::size_t size) {
        const auto* begin = static_cast<const unsigned char*>(data);
        std::size_t offset = 0;
        for (; offset + 8 <= size; offset += 8) {
            std::uint64_t word{};
            std::memcpy(&word, begin + offset, 8);
            first.mix(word);
            second.mix(word);
        }
        std::uint64_t tail{};
        if (offset < size) std::memcpy(&tail, begin + offset, size - offset);
        const std::uint64_t last = tail ^ (static_cast<std::uint64_t>(size) << 56U);
        first.mix(last);
        second.mix(last);
    }
    template <typename T>
    void scalar(const T value) { bytes(&value, sizeof(T)); }
    [[nodiscard]] std::uint64_t value() const {
        std::uint64_t h = state_;
        h ^= h >> 33U;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33U;
        return h;
    }

private:
    void mix(const std::uint64_t word) {
        state_ ^= word * 0x9e3779b97f4a7c15ULL;
        state_ = ((state_ << 31U) | (state_ >> 33U)) * 0xc2b2ae3d27d4eb4fULL;
    }
    static constexpr std::uint64_t initial = 0x84222325cbf29ce4ULL;
    std::uint64_t state_{initial};
};

// The identity pair of a texture: its format, size, level count and every stored mip level's bytes. Always computed from
// the pixels; Texture::content_digest only carries a result of this function.
[[nodiscard]] inline TextureDigest compute_texture_digest(const Texture& texture) {
    ContentDigest digest;
    ContentDigest check(1);
    for (ContentDigest* item : {&digest, &check}) {
        item->scalar(texture.format);
        item->scalar(texture.width);
        item->scalar(texture.height);
        item->scalar(static_cast<std::uint64_t>(texture.mips.size()));
    }
    for (const MipLevel& mip : texture.mips) {
        ContentDigest::bytes_both(digest, check, mip.bytes.data(), mip.bytes.size());
    }
    return {digest.value(), check.value()};
}

} // namespace eawr::assets
