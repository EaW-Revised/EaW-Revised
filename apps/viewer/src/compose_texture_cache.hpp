#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/scene/texture_prefetch.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include <cstdint>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace eawr::presentation::godot_backend {

// What the initial compose has a texture read-ahead do to the BaseTexture of a surface drawn with this effect (#5989):
// the opaque, lit materials (every selector that is not a transparent-phase effect; ships, but also stations, props,
// asteroids and debris) get a mip chain when the texture stops above 1x1, because a ship seen from afar otherwise samples
// the full-size hull texture at 16-64 texels per screen pixel and its surface shimmers. Chosen by effect phase and by the texture's format and chain state (the reader skips
// anything it cannot complete), never by file or mod name.
//
// The additive effects (the ship window lights, MeshAdditive and its variants) are transparent-phase selectors and so are
// NOT in the class: chaining them dims the lights at distance (retail's look), and the owner prefers the original
// brightness. Putting them in is this one function: return colour_mip_chain for them too.
[[nodiscard]] inline scene::TextureProcessing texture_processing_for(const scene::LegacySelector& selector) noexcept {
    return selector.transparent ? scene::TextureProcessing::none : scene::TextureProcessing::colour_mip_chain;
}

// Decoded textures the initial space compose keeps between the surfaces that name them (docs/performance/
// memory-attribution.md, "Compose texture release"). A texture is decoded once per path while it is held; the renderer
// copies what it needs on upload, so the held copy only saves a repeat decode for the next surface naming the same
// path. Bounded by bytes, least recently used first: an unbounded hold (`unbounded_bytes`, the behaviour of earlier
// builds) kept every decoded texture of a 600-texture mod scene (about 2 GB) alive until the compose returned.
// Load-time only: main-thread, no locking.
class ComposeTextureCache final {
public:
    static constexpr std::uint64_t unbounded_bytes = std::numeric_limits<std::uint64_t>::max();

    struct Stats final {
        std::uint64_t decodes{};      // loader calls that produced a texture
        std::uint64_t hits{};         // requests served from the held copy
        std::uint64_t evictions{};    // held copies dropped to stay within the budget
        std::uint64_t redecodes{};    // decodes of a path an eviction had dropped earlier
        std::uint64_t held_bytes{};   // mip payload currently held
        std::uint64_t peak_held_bytes{};
        std::uint64_t budget_bytes{unbounded_bytes};
    };

    explicit ComposeTextureCache(const std::uint64_t budget_bytes = unbounded_bytes) { stats_.budget_bytes = budget_bytes; }
    // The index holds iterators into `order_`: a copy or move would point into the other object.
    ComposeTextureCache(const ComposeTextureCache&) = delete;
    ComposeTextureCache& operator=(const ComposeTextureCache&) = delete;
    ComposeTextureCache(ComposeTextureCache&&) = delete;
    ComposeTextureCache& operator=(ComposeTextureCache&&) = delete;

    // Applies to later requests; entries over the new budget leave on the next request.
    void set_budget(const std::uint64_t budget_bytes) { stats_.budget_bytes = budget_bytes; }

    // The key a texture is held under: the path, plus the processing when it changes the result, so one file wanted both
    // plain (by another effect) and with a chain is two held textures.
    [[nodiscard]] static std::string key(const std::string& path, const scene::TextureProcessing processing) {
        switch (processing) {
        case scene::TextureProcessing::colour_mip_chain: return path + "|chain";
        case scene::TextureProcessing::normal_mip_chain: return path + "|normal-chain";
        default: return path;
        }
    }

    // The held texture for `path`, decoding it with `load` (returns std::optional<assets::Texture>) on a miss. A load
    // that returns nothing is not held, so the next request tries again. The returned pointer stays valid after the
    // entry is evicted: a caller mid-upload is never affected by the budget.
    template<class Load>
    [[nodiscard]] std::shared_ptr<const assets::Texture> get(const std::string& path, Load&& load) {
        if (const auto found = index_.find(path); found != index_.end()) {
            order_.splice(order_.begin(), order_, found->second);
            ++stats_.hits;
            return found->second->texture;
        }
        std::optional<assets::Texture> decoded = load();
        if (!decoded) return nullptr;
        ++stats_.decodes;
        if (evicted_.erase(path) != 0) ++stats_.redecodes;
        auto texture = std::make_shared<const assets::Texture>(std::move(*decoded));
        const std::uint64_t bytes = payload_bytes(*texture);
        order_.push_front({path, texture, bytes});
        index_.emplace(path, order_.begin());
        stats_.held_bytes += bytes;
        // The newest entry always stays: one texture over the budget is held until the next request replaces it.
        while (stats_.held_bytes > stats_.budget_bytes && order_.size() > 1) {
            const Entry& oldest = order_.back();
            stats_.held_bytes -= oldest.bytes;
            ++stats_.evictions;
            evicted_.insert(oldest.path);
            index_.erase(oldest.path);
            order_.pop_back();
        }
        if (stats_.held_bytes > stats_.peak_held_bytes) stats_.peak_held_bytes = stats_.held_bytes;
        return texture;
    }

    // Where a miss gets its decode: a read-ahead (scene::TexturePrefetch, owned by the caller and outliving its use here)
    // when one is set, else assets::load_texture on the calling thread. Same result either way, except that `processing`
    // (a mip chain, tens of milliseconds for a 2048 texture) is only ever applied beside a read-ahead: without one (a
    // reinforcement composed during the battle, the Safe render thread) the texture is returned as decoded, so no
    // gameplay-time step grows by the chain's cost.
    void set_prefetch(scene::TexturePrefetch* prefetch) noexcept { prefetch_ = prefetch; }
    [[nodiscard]] core::Result<assets::Texture> decode(const vfs::Vfs& filesystem, const std::string_view path,
        const scene::TextureProcessing processing = scene::TextureProcessing::none) const {
        return prefetch_ != nullptr ? prefetch_->acquire(path, processing) : assets::load_texture(filesystem, path);
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct Entry final {
        std::string path;
        std::shared_ptr<const assets::Texture> texture;
        std::uint64_t bytes{};
    };

    [[nodiscard]] static std::uint64_t payload_bytes(const assets::Texture& texture) noexcept {
        std::uint64_t bytes = 0;
        for (const assets::MipLevel& mip : texture.mips) bytes += mip.bytes.size();
        return bytes;
    }

    std::list<Entry> order_;  // most recently used first
    std::unordered_map<std::string, std::list<Entry>::iterator> index_;
    std::unordered_set<std::string> evicted_;
    Stats stats_;
    scene::TexturePrefetch* prefetch_{};
};

// Budget of the initial compose's texture hold: 512 MiB holds the textures of the largest single BFC model (12 x 4096^2
// BC2 = 256 MiB) twice over, and cut the sampled load peak of the BFC skirmish on the GTX 970 by 1.6 GB (docs/performance/
// memory-attribution.md, "Compose texture release").
inline constexpr std::uint64_t compose_texture_default_budget_bytes = 512ULL << 20U;

} // namespace eawr::presentation::godot_backend
