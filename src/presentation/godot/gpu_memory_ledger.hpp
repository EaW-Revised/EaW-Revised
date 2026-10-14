#pragma once

#include "eawr/assets/assets.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <utility>

namespace eawr::presentation::godot_backend::detail {

// #2856: process-wide accounting of the GPU resources the renderer uploads,
// by category and content identity. Presentation diagnostics only: it never
// feeds the simulation or decides what is uploaded. A content key is a
// 64-bit digest of exactly the bytes an upload hands to the GPU, so two
// uploads with one key are the same pixels or geometry uploaded twice.
enum class GpuCategory : std::uint8_t { texture, texture_array, mesh, count };

struct GpuItem final {
    GpuCategory category{GpuCategory::texture};
    // `key` and `check` are two independent digests of the same bytes; the
    // pair is the identity under which one GPU copy is shared.
    std::uint64_t key{}, check{};
    // Estimated device bytes (see texture_gpu_bytes / mesh_gpu_bytes).
    std::uint64_t bytes{};
    bool compressed{};
};

struct GpuCategoryTotals final {
    // Live resources and the bytes they hold, duplicates included.
    std::uint64_t live{}, live_bytes{};
    // Live distinct content keys and the bytes one copy of each would hold.
    std::uint64_t unique{}, unique_bytes{};
    // Live bytes in block-compressed (BCn) and uncompressed formats.
    std::uint64_t compressed_bytes{}, uncompressed_bytes{};
    // Every upload since the process started, freed ones included.
    std::uint64_t uploads{};
    // Requests served by an already resident copy instead of an upload.
    std::uint64_t reuses{};
};
using GpuTotals = std::array<GpuCategoryTotals, static_cast<std::size_t>(GpuCategory::count)>;

class GpuMemoryLedger final {
public:
    void add(const GpuItem& item) {
        std::lock_guard lock(mutex_);
        auto& totals = totals_[index(item)];
        ++totals.uploads;
        ++totals.live;
        totals.live_bytes += item.bytes;
        (item.compressed ? totals.compressed_bytes : totals.uncompressed_bytes) += item.bytes;
        auto& entry = keys_[{item.category, item.key}];
        if (entry.first++ == 0) {
            entry.second = item.bytes;
            ++totals.unique;
            totals.unique_bytes += item.bytes;
        }
    }

    void reuse(const GpuItem& item) {
        std::lock_guard lock(mutex_);
        ++totals_[index(item)].reuses;
    }

    void remove(const GpuItem& item) {
        std::lock_guard lock(mutex_);
        auto& totals = totals_[index(item)];
        const auto found = keys_.find({item.category, item.key});
        if (found == keys_.end() || totals.live == 0) return;
        --totals.live;
        totals.live_bytes -= item.bytes;
        (item.compressed ? totals.compressed_bytes : totals.uncompressed_bytes) -= item.bytes;
        if (--found->second.first == 0) {
            --totals.unique;
            totals.unique_bytes -= found->second.second;
            keys_.erase(found);
        }
    }

    // A resident upload rebuilt smaller in place (GM-04): same identity, fewer bytes.
    void resize(const GpuItem& item, const std::uint64_t bytes) {
        std::lock_guard lock(mutex_);
        auto& totals = totals_[index(item)];
        const auto found = keys_.find({item.category, item.key});
        if (found == keys_.end()) return;
        totals.live_bytes = totals.live_bytes - item.bytes + bytes;
        auto& kind = item.compressed ? totals.compressed_bytes : totals.uncompressed_bytes;
        kind = kind - item.bytes + bytes;
        if (found->second.second == item.bytes) {
            totals.unique_bytes = totals.unique_bytes - item.bytes + bytes;
            found->second.second = bytes;
        }
    }

    [[nodiscard]] GpuTotals totals() const {
        std::lock_guard lock(mutex_);
        return totals_;
    }

private:
    [[nodiscard]] static std::size_t index(const GpuItem& item) { return static_cast<std::size_t>(item.category); }

    mutable std::mutex mutex_;
    GpuTotals totals_{};
    // (category, key) -> (live references, bytes of one copy)
    std::map<std::pair<GpuCategory, std::uint64_t>, std::pair<std::uint64_t, std::uint64_t>> keys_;
};

[[nodiscard]] inline GpuMemoryLedger& gpu_memory_ledger() {
    static GpuMemoryLedger ledger;
    return ledger;
}

// Measurement switch (--eawr-texture-share off): every texture request gets
// its own GPU copy again, so one build reports both footprints.
[[nodiscard]] inline std::atomic<bool>& texture_sharing() {
    static std::atomic<bool> enabled{true};
    return enabled;
}

// A fast non-cryptographic 64-bit digest (word-wise multiply/xor mixing).
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

[[nodiscard]] inline bool block_compressed(const assets::PixelFormat format) {
    switch (format) {
    case assets::PixelFormat::bc1: case assets::PixelFormat::bc2: case assets::PixelFormat::bc3:
    case assets::PixelFormat::bc4: case assets::PixelFormat::bc5: case assets::PixelFormat::bc7: return true;
    default: return false;
    }
}

// The bytes uploaded for a texture: its stored mip chain. Three-channel
// sources are padded to four channels on the GPU.
[[nodiscard]] inline GpuItem texture_item(const assets::Texture& texture) {
    ContentDigest digest;
    ContentDigest check(1);
    std::uint64_t bytes{};
    for (ContentDigest* item : {&digest, &check}) {
        item->scalar(texture.format);
        item->scalar(texture.width);
        item->scalar(texture.height);
        item->scalar(static_cast<std::uint64_t>(texture.mips.size()));
    }
    for (const assets::MipLevel& mip : texture.mips) {
        digest.bytes(mip.bytes.data(), mip.bytes.size());
        check.bytes(mip.bytes.data(), mip.bytes.size());
        bytes += mip.bytes.size();
    }
    if (texture.format == assets::PixelFormat::bgr8) bytes = bytes / 3U * 4U;
    return {GpuCategory::texture, digest.value(), check.value(), bytes, block_compressed(texture.format)};
}

// A layered texture of RGBA8 layers at `edge` with a full mip chain.
[[nodiscard]] inline GpuItem texture_array_item(const std::span<const assets::Texture> layers, const std::uint32_t edge) {
    ContentDigest digest;
    digest.scalar(edge);
    for (const assets::Texture& layer : layers) digest.scalar(texture_item(layer).key);
    const std::uint64_t top = static_cast<std::uint64_t>(edge) * edge * 4U;
    return {GpuCategory::texture_array, digest.value(), 0, top * 4U / 3U * layers.size(), false};
}

// Estimated device bytes of one uploaded surface: position (12), packed
// normal and tangent (4 + 4), uv (8), colour (4), optional custom0 binormal
// (12), skin bones and weights (8 + 8), 16- or 32-bit indices.
[[nodiscard]] inline std::uint64_t surface_gpu_bytes(const std::size_t vertices, const std::size_t indices,
                                                     const bool skinned, const bool binormals) {
    const std::uint64_t stride = 32U + (binormals ? 12U : 0U) + (skinned ? 16U : 0U);
    return stride * vertices + indices * (vertices > 65535U ? 4U : 2U);
}

} // namespace eawr::presentation::godot_backend::detail
