#include "ui/theme_builder.hpp"

#include "eawr/core/diagnostic.hpp"

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/transform2d.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

[[nodiscard]] String text(const std::string_view value) {
    return String::utf8(value.data(), static_cast<int64_t>(value.size()));
}

[[nodiscard]] Color colour(const data::ui::Rgba& value) {
    return {static_cast<float>(value.red) / 255.0F, static_cast<float>(value.green) / 255.0F,
            static_cast<float>(value.blue) / 255.0F, static_cast<float>(value.alpha) / 255.0F};
}

[[nodiscard]] std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

[[nodiscard]] Vector2i scaled(const std::uint32_t width, const std::uint32_t height, const double scale_x,
                              const double scale_y) {
    return {std::max(1, static_cast<int>(std::lround(width * scale_x))),
            std::max(1, static_cast<int>(std::lround(height * scale_y)))};
}

} // namespace

Ref<Image> texture_image(const assets::Texture& texture, std::string& failure) {
    if (texture.mips.empty()) {
        failure = "decoded without a base mip level";
        return {};
    }
    const assets::MipLevel& mip = texture.mips.front();
    Image::Format format = Image::FORMAT_MAX;
    std::uint32_t bytes_per_pixel{};
    switch (texture.format) {
    case assets::PixelFormat::rgba8:
    case assets::PixelFormat::bgra8: format = Image::FORMAT_RGBA8; bytes_per_pixel = 4; break;
    case assets::PixelFormat::bgr8: format = Image::FORMAT_RGB8; bytes_per_pixel = 3; break;
    case assets::PixelFormat::l8: format = Image::FORMAT_L8; bytes_per_pixel = 1; break;
    case assets::PixelFormat::a8: format = Image::FORMAT_LA8; bytes_per_pixel = 1; break;
    case assets::PixelFormat::bc1: format = Image::FORMAT_DXT1; break;
    case assets::PixelFormat::bc2: format = Image::FORMAT_DXT3; break;
    case assets::PixelFormat::bc3: format = Image::FORMAT_DXT5; break;
    case assets::PixelFormat::bc4: format = Image::FORMAT_RGTC_R; break;
    case assets::PixelFormat::bc5: format = Image::FORMAT_RGTC_RG; break;
    case assets::PixelFormat::bc7: format = Image::FORMAT_BPTC_RGBA; break;
    }
    if (bytes_per_pixel != 0 && mip.row_pitch != texture.width * bytes_per_pixel) {
        failure = "base mip is not tightly packed";
        return {};
    }
    std::vector<std::byte> bytes = mip.bytes;
    if (texture.format == assets::PixelFormat::bgra8) {
        for (std::size_t at = 0; at + 3U < bytes.size(); at += 4U) std::swap(bytes[at], bytes[at + 2U]);
    } else if (texture.format == assets::PixelFormat::bgr8) {
        for (std::size_t at = 0; at + 2U < bytes.size(); at += 3U) std::swap(bytes[at], bytes[at + 2U]);
    } else if (texture.format == assets::PixelFormat::a8) {
        // One stored alpha byte per pixel becomes a white luminance/alpha pair.
        std::vector<std::byte> expanded(bytes.size() * 2U, std::byte{0xFF});
        for (std::size_t index = 0; index < bytes.size(); ++index) expanded[index * 2U + 1U] = bytes[index];
        bytes = std::move(expanded);
    }
    PackedByteArray packed;
    packed.resize(static_cast<int64_t>(bytes.size()));
    if (!bytes.empty()) std::memcpy(packed.ptrw(), bytes.data(), bytes.size());
    Ref<Image> image = Image::create_from_data(static_cast<int32_t>(texture.width), static_cast<int32_t>(texture.height),
                                               false, format, packed);
    if (image.is_null() || image->is_empty()) {
        failure = "could not be adopted as an engine image";
        return {};
    }
    if (image->is_compressed() && image->decompress() != OK) {
        failure = "block-compressed payload could not be decompressed";
        return {};
    }
    image->convert(Image::FORMAT_RGBA8);
    if (texture.source_origin == assets::ImageOrigin::bottom_left) image->flip_y();
    return image;
}

UiTextures::UiTextures(const assets::MegaTextureAtlas& atlas, const vfs::Vfs& filesystem) : filesystem_(filesystem) {
    page_ = texture_image(atlas.page, page_failure_);
    if (!page_failure_.empty()) page_failure_ = atlas.page.source.logical_path + ": " + page_failure_;
}

Ref<Texture2D> UiTextures::empty() {
    if (empty_.is_null()) {
        const Ref<Image> image = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
        image->fill(Color(0, 0, 0, 0));
        empty_ = ImageTexture::create_from_image(image);
        empty_->set_meta(empty_piece_meta, true);
    }
    return empty_;
}

Ref<Texture2D> UiTextures::texture(const model::ThemeTexture& slot, const double scale_x, const double scale_y) {
    Ref<Image> source;
    Rect2i region;
    std::string key;
    if (slot.origin == model::TextureOrigin::atlas) {
        if (page_.is_null()) return {};
        const assets::AtlasRectangle& rect = slot.rectangle;
        region = Rect2i(static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y), static_cast<int32_t>(rect.width),
                        static_cast<int32_t>(rect.height));
        if (rect.width == 0 || rect.height == 0 || region.get_end().x > page_->get_width()
            || region.get_end().y > page_->get_height()) {
            return {};
        }
        source = page_;
        key = "atlas:" + lower(slot.texture);
    } else if (slot.origin == model::TextureOrigin::standalone) {
        auto found = standalone_.find(slot.logical_path);
        if (found == standalone_.end()) {
            Ref<Image> image;
            std::string failure;
            const std::string path = lower(slot.logical_path);
            if (path.ends_with(".jpg") || path.ends_with(".jpeg")) {
                // A mod's JPEG (the asset loader reads TGA and DDS): the engine decodes it.
                auto bytes = filesystem_.open(slot.logical_path);
                if (bytes && !model::jpeg_refusal(bytes.value()).empty()) {
                    failure = model::jpeg_refusal(bytes.value());  // never hand an unbounded JPEG to the decoder
                } else if (bytes) {
                    PackedByteArray packed;
                    packed.resize(static_cast<int64_t>(bytes.value().size()));
                    if (!bytes.value().empty()) std::memcpy(packed.ptrw(), bytes.value().data(), bytes.value().size());
                    image.instantiate();
                    if (image->load_jpg_from_buffer(packed) != OK) {
                        image.unref();
                        failure = "JPEG could not be decoded";
                    } else {
                        image->convert(Image::FORMAT_RGBA8);
                    }
                } else {
                    failure = core::format_diagnostic(bytes.error());
                }
            } else if (auto loaded = assets::load_texture(filesystem_, slot.logical_path)) {
                image = texture_image(loaded.value(), failure);
            } else {
                failure = core::format_diagnostic(loaded.error());
            }
            if (image.is_null()) problems_.push_back(slot.logical_path + ": " + failure);
            found = standalone_.emplace(slot.logical_path, image).first;
        }
        if (found->second.is_null()) return {};
        source = found->second;
        region = Rect2i(0, 0, source->get_width(), source->get_height());
        key = "file:" + lower(slot.logical_path);
    } else {
        return {};
    }
    const Vector2i size = scaled(static_cast<std::uint32_t>(region.size.x), static_cast<std::uint32_t>(region.size.y),
                                 scale_x, scale_y);
    key += "__" + std::to_string(size.x) + "x" + std::to_string(size.y);
    if (const auto cached = cache_.find(key); cached != cache_.end()) return cached->second;
    // Texel resolution; the size override scales it at draw time, so a piece
    // is resampled once, by the GPU.
    Ref<ImageTexture> texture = ImageTexture::create_from_image(source->get_region(region));
    texture->set_size_override(size);
    cache_.emplace(key, texture);
    return texture;
}

Ref<Theme> build_theme(const model::ThemeModel& theme_model, UiTextures& textures, FontProvider& fonts,
                       ThemeBuildSummary* summary) {
    Ref<Theme> theme;
    theme.instantiate();
    ThemeBuildSummary counts;
    std::map<std::string, Ref<FontVariation>> variations;
    const auto add_style = [&](const model::ThemeStyle& style) {
        const StringName type(text(style.name));
        if (!style.base.empty()) theme->set_type_variation(type, StringName(text(style.base)));
        ++counts.styles;
        for (const model::ThemeTexture& slot : style.textures) {
            Ref<Texture2D> texture = textures.texture(slot, theme_model.scale_x, theme_model.scale_y);
            if (texture.is_null()) {
                texture = textures.empty();
                ++counts.empty_icons;
            }
            theme->set_icon(StringName(text(slot.slot)), type, texture);
            ++counts.icons;
        }
        for (const model::ThemeFont& font : style.fonts) {
            const float width_scale = font.pixels.glyph_height > 0 && font.pixels.glyph_height != font.pixels.width_em
                ? static_cast<float>(font.pixels.width_em) / static_cast<float>(font.pixels.glyph_height)
                : 1.0F;
            std::ostringstream key;
            key << font.resolved.face << '|' << static_cast<int>(font.resolved.source) << '|'
                << font.character_padding << '|' << std::lround(width_scale * 1000.0F);
            Ref<FontVariation>& variation = variations[key.str()];
            if (variation.is_null()) {
                variation.instantiate();
                variation->set_base_font(fonts.font(font.resolved));
                if (font.character_padding != 0) {
                    variation->set_spacing(TextServer::SPACING_GLYPH, font.character_padding);
                }
                // UI-F2: glyphs as high as the stretched size, as wide as the unstretched face.
                if (width_scale != 1.0F) variation->set_variation_transform(Transform2D(width_scale, 0, 0, 1, 0, 0));
            }
            const String role = text(data::ui::to_string(font.role));
            theme->set_font(StringName(role), type, variation);
            theme->set_font_size(StringName(role), type, font.pixels.glyph_height);
            theme->set_color(StringName(role + String("_top")), type, colour(font.top_color));
            theme->set_color(StringName(role + String("_bottom")), type, colour(font.bottom_color));
            theme->set_constant(StringName(role + String("_emboss")), type, font.emboss ? 1 : 0);
            theme->set_constant(StringName(role + String("_outline")), type, font.outline ? 1 : 0);
            theme->set_constant(StringName(role + String("_cell_ascent")), type, font.cell_ascent);
            theme->set_constant(StringName(role + String("_cell_descent")), type, font.cell_descent);
            ++counts.fonts;
        }
    };
    add_style(theme_model.defaults);
    // Godot follows a Control's theme_type_variation only when a theme
    // declares it as a variation, so the default style is one too, of an
    // empty base type.
    theme->set_type_variation(StringName(text(model::theme_type)), StringName(text(kit_base_type)));
    theme->set_constant("scale_permille", text(model::theme_type),
                        static_cast<int32_t>(std::lround(std::min(theme_model.scale_x, theme_model.scale_y) * 1000.0)));
    for (const model::ThemeStyle& style : theme_model.variations) add_style(style);
    counts.font_variations = variations.size();
    if (summary != nullptr) *summary = counts;
    return theme;
}

} // namespace eawr::presentation::godot_backend
