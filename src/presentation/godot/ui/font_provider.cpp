#include "ui/font_provider.hpp"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <cstring>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
}

} // namespace

FontProvider::FontProvider(model::FontCache cache) : cache_(std::move(cache)) {
    std::vector<model::CachedFace> readable;
    for (model::CachedFace& face : cache_.faces) {
        PackedByteArray bytes;
        bytes.resize(static_cast<int64_t>(face.bytes.size()));
        std::memcpy(bytes.ptrw(), face.bytes.data(), face.bytes.size());
        Ref<FontFile> file;
        file.instantiate();
        file->set_data(bytes);
        // The engine reads the face on first use; a face it cannot read has no name.
        if (file->get_font_name().is_empty()) {
            core::Diagnostic diagnostic;
            diagnostic.code = std::string(model::diagnostic_codes::font_cache_face);
            diagnostic.severity = core::Severity::warning;
            diagnostic.message = face.face + " cache file could not be loaded by the engine";
            diagnostic.logical_path = face.logical_path;
            cache_.diagnostics.push_back(std::move(diagnostic));
            continue;
        }
        files_.emplace(face.face, file);
        readable.push_back(std::move(face));
    }
    cache_.faces = std::move(readable);
    const PackedStringArray families = OS::get_singleton()->get_system_fonts();
    for (int64_t index = 0; index < families.size(); ++index) system_families_.push_back(utf8(families[index]));
}

model::ResolvedFont FontProvider::resolve(const model::FontRequest& request, const std::string_view language) const {
    const model::SystemFaceProbe probe = [this](const std::string_view face) {
        return model::match_system_face(face, system_families_).has_value();
    };
    return model::resolve_font(request, language, cache_, probe);
}

Ref<FontFile> FontProvider::cached_font(const std::string_view face) const {
    const model::CachedFace* cached = cache_.find(face);
    if (cached == nullptr) return {};
    const auto found = files_.find(cached->face);
    return found == files_.end() ? Ref<FontFile>() : found->second;
}

Ref<Font> FontProvider::font(const model::ResolvedFont& resolved) {
    if (resolved.source == model::FaceSource::cache) {
        if (Ref<FontFile> file = cached_font(resolved.face); file.is_valid()) return file;
    }
    if (resolved.source == model::FaceSource::system) {
        if (const auto match = model::match_system_face(resolved.face, system_families_)) {
            Ref<SystemFont>& font = system_fonts_[resolved.face];
            if (font.is_null()) {
                font.instantiate();
                PackedStringArray names;
                names.push_back(String::utf8(match->family.c_str()));
                font->set_font_names(names);
                font->set_font_weight(match->bold ? 700 : 400);
            }
            return font;
        }
    }
    return ThemeDB::get_singleton()->get_fallback_font();
}

} // namespace eawr::presentation::godot_backend
