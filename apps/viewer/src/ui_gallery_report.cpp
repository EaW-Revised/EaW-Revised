#include "ui_gallery_internal.hpp"

namespace eawr::presentation::godot_backend {

namespace {

[[nodiscard]] std::string json(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) output << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
            else output << static_cast<char>(character);
        }
    }
    output << '"';
    return output.str();
}

[[nodiscard]] bool write_bytes(const std::filesystem::path& path, const PackedByteArray& bytes) {
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.ptr()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}
} // namespace

bool UiGalleryMode::State::capture() {
    Ref<Image> image = host->get_viewport()->get_texture()->get_image();
    if (image.is_null() || image->is_empty()) {
        failure = "the viewport could not be read back";
        return false;
    }
    FixedCamera requested;
    requested.width = width;
    requested.height = height;
    CaptureResult read_back;
    read_back.width = static_cast<std::uint32_t>(image->get_width());
    read_back.height = static_cast<std::uint32_t>(image->get_height());
    if (const std::string problem = capture_size_problem(read_back, requested); !problem.empty()) {
        failure = problem;
        return false;
    }
    image->convert(Image::FORMAT_RGBA8);
    const PackedByteArray pixels = image->get_data();
    for (Sample& sample : samples) {
        std::set<std::uint32_t> seen;
        const auto x0 = std::max<std::int64_t>(0, static_cast<std::int64_t>(sample.rect.position.x));
        const auto y0 = std::max<std::int64_t>(0, static_cast<std::int64_t>(sample.rect.position.y));
        const std::int64_t x1 = std::min<std::int64_t>(image->get_width(),
                                                       static_cast<std::int64_t>(sample.rect.get_end().x));
        const std::int64_t y1 = std::min<std::int64_t>(image->get_height(),
                                                       static_cast<std::int64_t>(sample.rect.get_end().y));
        for (std::int64_t y = y0; y < y1 && seen.size() < 4096U; ++y) {
            for (std::int64_t x = x0; x < x1; ++x) {
                const std::int64_t at = (y * image->get_width() + x) * 4;
                seen.insert((static_cast<std::uint32_t>(pixels[at]) << 16U)
                            | (static_cast<std::uint32_t>(pixels[at + 1]) << 8U) | pixels[at + 2]);
            }
        }
        sample.colours = seen.size();
    }
    const PackedByteArray png_bytes = image->save_png_to_buffer();
    capture_sha256 = core::sha256_hex(
        std::span<const std::uint8_t>(png_bytes.ptr(), static_cast<std::size_t>(png_bytes.size())));
    if (!options.capture_path.empty()) {
        if (!write_bytes(options.capture_path, png_bytes)) {
            failure = "the capture PNG could not be written";
            return false;
        }
        png = std::array<std::int32_t, 2>{image->get_width(), image->get_height()};
    }
    return true;
}

bool UiGalleryMode::State::write_report() const {
    if (options.report_path.empty()) return false;
    std::ostringstream output;
    output << "{\n  \"mode\": \"ui_gallery\",\n  \"status\": " << json(status) << ",\n  \"failure\": " << json(failure)
           << ",\n  \"page\": " << json(!options.hud.empty() ? "hud" : options.dialog.empty() ? "controls" : options.dialog)
           << ",\n  \"mod\": "
           << (options.mod_root.empty() ? "false" : "true") << ",\n  \"rules\": "
           << json(options.rules) << ",\n  \"backend\": {\"engine\": " << json(backend.engine)
           << ", \"rendering_method\": " << json(backend.rendering_method) << ", \"adapter_vendor\": "
           << json(backend.adapter_vendor) << ", \"adapter_name\": " << json(backend.adapter_name)
           << ", \"driver_api\": " << json(backend.driver_api) << "},\n  \"viewport\": {\"width\": " << width
           << ", \"height\": " << height << "}";
    if (movie) {
        const Ref<Texture2D> texture = movie_player ? movie_player->get_video_texture() : Ref<Texture2D>();
        output << ",\n  \"movie\": {\"name\": " << json(movie->name)
               << ", \"source\": " << json(movie->source.canonical_path)
               << ", \"source_id\": " << json(movie->source.source_id)
               << ", \"cache_key\": " << json(movie->cache_key)
               << ", \"alpha\": " << (movie->alpha ? "true" : "false")
               << ", \"texture\": [" << (texture.is_valid() ? texture->get_width() : 0) << ", "
               << (texture.is_valid() ? texture->get_height() : 0) << "]"
               << ", \"position\": " << (movie_player ? movie_player->get_stream_position() : 0.0)
               << ", \"distinct_frames\": " << movie_frame_hashes.size()
               << ", \"loops\": " << movie_loops
               << ", \"playing\": " << (movie_player && movie_player->is_playing() ? "true" : "false") << "}";
    }
    if (hud) output << ",\n  \"hud\": " << hud->report_json();
    if (atlas) {
        output << ",\n  \"atlas\": {\"mtd\": " << json(atlas->directory.source.logical_path) << ", \"entries\": "
               << atlas->directory.entries.size() << ", \"page\": " << json(atlas->page.source.logical_path)
               << ", \"width\": " << atlas->page.width << ", \"height\": " << atlas->page.height << ", \"format\": "
               << json(assets::to_string(atlas->page.format)) << "}";
    }
    if (fonts) {
        const model::FontCache& cache = fonts->cache();
        output << ",\n  \"font_cache\": {\"directory\": " << json(cache.directory) << ", \"source\": "
               << json(font_cache_source) << ", \"faces\": [";
        for (std::size_t index = 0; index < cache.faces.size(); ++index) {
            output << (index ? ", " : "") << json(cache.faces[index].face);
        }
        output << "], \"diagnostics\": " << cache.diagnostics.size() << "}";
    }
    if (theme_model) {
        const model::ThemeModel& model_value = *theme_model;
        std::map<std::string, std::size_t> origins;
        for (const model::ThemeTexture& slot : model_value.defaults.textures) ++origins[std::string(model::to_string(slot.origin))];
        std::size_t chained{};
        for (const model::ThemeStyle& style : model_value.variations) chained += style.base != model::theme_type ? 1U : 0U;
        output << ",\n  \"theme\": {\"scale_x\": " << model_value.scale_x << ", \"scale_y\": " << model_value.scale_y
               << ", \"font_screen_height\": " << model_value.font_screen_height << ", \"default_slots\": "
               << model_value.defaults.textures.size() << ", \"origins\": {";
        bool first = true;
        for (const auto& [origin, count] : origins) {
            output << (first ? "" : ", ") << json(origin) << ": " << count;
            first = false;
        }
        output << "}, \"variations\": " << model_value.variations.size() << ", \"chained\": " << chained
               << ", \"unmatched\": " << model_value.unmatched.size() << ",\n    \"built\": {\"styles\": "
               << summary.styles << ", \"icons\": " << summary.icons << ", \"empty_icons\": " << summary.empty_icons
               << ", \"fonts\": " << summary.fonts << ", \"font_variations\": " << summary.font_variations << "}";
        output << ",\n    \"fonts\": [";
        for (std::size_t index = 0; index < model_value.defaults.fonts.size(); ++index) {
            const model::ThemeFont& font = model_value.defaults.fonts[index];
            output << (index ? ",\n      " : "\n      ") << "{\"role\": " << json(data::to_string(font.role))
                   << ", \"face\": " << json(font.face) << ", \"point_size\": " << font.point_size
                   << ", \"resolved\": " << json(font.resolved.face) << ", \"source\": "
                   << json(model::to_string(font.resolved.source)) << ", \"substituted\": "
                   << (font.resolved.substituted ? "true" : "false") << ", \"em_height\": " << font.pixels.em_height
                   << ", \"glyph_height\": " << font.pixels.glyph_height << "}";
        }
        output << "],\n    \"diagnostics\": [";
        for (std::size_t index = 0; index < model_value.diagnostics.size(); ++index) {
            output << (index ? ",\n      " : "\n      ") << json(core::format_diagnostic(model_value.diagnostics[index]));
        }
        output << "]}";
    }
    output << ",\n  \"notes\": [";
    std::vector<std::string> all_notes = notes;
    if (textures) all_notes.insert(all_notes.end(), textures->problems().begin(), textures->problems().end());
    for (std::size_t index = 0; index < all_notes.size(); ++index) {
        output << (index ? ", " : "") << json(all_notes[index]);
    }
    output << "],\n  \"controls\": [";
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const Sample& sample = samples[index];
        output << (index ? ",\n    " : "\n    ") << "{\"kind\": " << json(sample.kind) << ", \"state\": "
               << json(sample.state) << ", \"variation\": " << json(sample.variation) << ", \"rect\": ["
               << sample.rect.position.x << ", " << sample.rect.position.y << ", " << sample.rect.size.x << ", "
               << sample.rect.size.y << "], \"colours\": " << sample.colours << "}";
    }
    output << "]";
    if (dialog) {
        output << ",\n  \"dialog\": {\"name\": " << json(options.dialog) << ", \"frame\": [" << dialog->layout.frame.x
               << ", " << dialog->layout.frame.y << ", " << dialog->layout.frame.width << ", "
               << dialog->layout.frame.height << "], \"gadgets\": [";
        for (std::size_t index = 0; index < dialog->gadgets.size(); ++index) {
            const BuiltGadget& gadget = dialog->gadgets[index];
            output << (index ? ",\n    " : "\n    ") << "{\"id\": " << json(gadget.id) << ", \"statement\": "
                   << json(gadget.statement) << ", \"kind\": " << json(gadget.kind) << ", \"variation\": "
                   << json(gadget.variation) << ", \"caption\": " << json(gadget.caption) << ", \"rect\": ["
                   << gadget.rect.x << ", " << gadget.rect.y << ", " << gadget.rect.width << ", "
                   << gadget.rect.height << "]}";
        }
        output << "]}";
    }
    output << ",\n  \"capture\": {\"png\": "
           << (png ? "{\"width\": " + std::to_string((*png)[0]) + ", \"height\": " + std::to_string((*png)[1]) + "}"
                   : std::string("null"))
           << ", \"sha256\": " << json(capture_sha256) << "}\n}\n";
    std::error_code error;
    if (!options.report_path.parent_path().empty()) {
        std::filesystem::create_directories(options.report_path.parent_path(), error);
    }
    std::ofstream file(options.report_path, std::ios::binary | std::ios::trunc);
    file << output.str();
    return static_cast<bool>(file);
}
} // namespace eawr::presentation::godot_backend
