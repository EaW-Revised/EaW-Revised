#pragma once

#include "eawr/assets/assets.hpp"
#include "upload_identity.hpp"
#include "gpu_memory_ledger.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace eawr::presentation::godot_backend::detail {

// The loader's captured content owner must outlive this recipe. A generated or
// transformed texture keeps its snapshot unless the loader reproduces it exactly.
// One exception: a texture whose mip chain was generated after loading (assets::Source::authored_levels, #5989) is
// verified against its file's authored levels alone and then released like any other. A later read gives those authored
// levels back, never the generated ones: regenerating a chain costs tens of milliseconds a texture on the thread that
// asks, which a re-admission during play must not pay. The generated levels return with the next load.
using TextureLoader = std::function<std::optional<assets::Texture>(const assets::Source&)>;

class TextureRecipe final {
public:
    TextureRecipe(const assets::Texture& texture, TextureLoader loader, const GpuItem item)
        : retained_(std::make_shared<const assets::Texture>(texture)), loader_(std::move(loader)), item_(item) {}

    [[nodiscard]] const GpuItem& item() const { return item_; }

    [[nodiscard]] const assets::Texture* retained() const { return retained_.get(); }

    // Where the pixels came from, also after the CPU snapshot was released (texture census).
    [[nodiscard]] const assets::Source& source() const { return retained_ ? retained_->source : source_; }

    // Called only after successful GPU admission. Verify once, without retaining
    // the decoded control, then release every mip allocation of the snapshot.
    void uploaded() {
        if (verified_ || !retained_) return;
        verified_ = true;
        if (!loader_ || retained_->source.logical_path.empty()) { loader_ = {}; return; }
        auto decoded = loader_(retained_->source);
        const std::size_t levels = retained_->source.authored_levels != 0 ? retained_->source.authored_levels : retained_->mips.size();
        if (!decoded || !same_content(*retained_, *decoded, levels)) { loader_ = {}; return; }
        source_ = retained_->source;
        identity_ = identity(*decoded);
        retained_.reset();
    }

    [[nodiscard]] std::shared_ptr<const assets::Texture> read() const {
        if (retained_) return retained_;
        if (!loader_) return {};
        auto decoded = loader_(source_);
        // Loose content can change after loading. Fail admission rather than
        // silently restore different pixels under the original upload identity.
        if (!decoded || identity(*decoded) != identity_) return {};
        return std::make_shared<const assets::Texture>(std::move(*decoded));
    }

private:
    [[nodiscard]] static UploadIdentity identity(const assets::Texture& texture) {
        UploadIdentityWriter writer;
        write_texture(writer, texture);
        writer.scalar(texture.has_alpha);
        writer.scalar(texture.source_origin);
        for (const auto& mip : texture.mips) {
            writer.scalar(mip.width);
            writer.scalar(mip.height);
            writer.scalar(mip.row_pitch);
        }
        return writer.digest();
    }
    // The first `levels` levels of `a` against all of `b`, which must hold exactly that many.
    [[nodiscard]] static bool same_content(const assets::Texture& a, const assets::Texture& b, const std::size_t levels) {
        if (a.width != b.width || a.height != b.height || a.format != b.format
            || a.has_alpha != b.has_alpha || a.source_origin != b.source_origin
            || a.mips.size() < levels || b.mips.size() != levels) return false;
        for (std::size_t i = 0; i < levels; ++i) {
            const auto& x = a.mips[i];
            const auto& y = b.mips[i];
            if (x.width != y.width || x.height != y.height || x.row_pitch != y.row_pitch || x.bytes != y.bytes)
                return false;
        }
        return true;
    }

    std::shared_ptr<const assets::Texture> retained_;
    TextureLoader loader_;
    assets::Source source_;
    UploadIdentity identity_{};
    GpuItem item_;
    bool verified_{};
};

} // namespace eawr::presentation::godot_backend::detail
