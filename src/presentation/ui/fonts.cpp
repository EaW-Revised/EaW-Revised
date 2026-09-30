#include "eawr/presentation/ui/fonts.hpp"

#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace eawr::presentation::ui {
namespace {

constexpr std::uint64_t max_cache_file_bytes = 4U * 1024U * 1024U;

[[nodiscard]] char fold(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) noexcept {
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(),
                      [](const char a, const char b) { return fold(a) == fold(b); });
}

core::Diagnostic cache_warning(const std::string& path, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::font_cache_face);
    diagnostic.severity = core::Severity::warning;
    diagnostic.message = std::move(message);
    diagnostic.logical_path = path;
    return diagnostic;
}

} // namespace

const CachedFace* FontCache::find(const std::string_view face) const noexcept {
    for (const CachedFace& cached : faces) {
        if (ieq(cached.face, face)) return &cached;
    }
    return nullptr;
}

std::string font_cache_path(const std::string_view face) {
    return std::string(font_cache_prefix) + "/" + std::string(face) + ".ttf";
}

FontCache load_font_cache(const vfs::Vfs& filesystem, std::string directory) {
    FontCache cache;
    cache.directory = std::move(directory);
    for (const std::string_view face : embedded_faces) {
        const std::string path = font_cache_path(face);
        const auto record = filesystem.stat(path);
        if (!record) {
            cache.diagnostics.push_back(cache_warning(path, std::string(face) + " is not in the font cache; run "
                                                                "tools/fonts/extract_eaw_fonts.py"));
            continue;
        }
        if (record.value().size > max_cache_file_bytes) {
            cache.diagnostics.push_back(cache_warning(path, std::string(face) + " cache file is too large"));
            continue;
        }
        auto bytes = filesystem.open(path);
        if (!bytes) {
            cache.diagnostics.push_back(cache_warning(path, std::string(face) + " cache file could not be read: "
                                                                + bytes.error().message));
            continue;
        }
        CachedFace cached;
        cached.face = std::string(face);
        cached.logical_path = path;
        cached.bytes = std::move(bytes.value());
        auto names = data::ui::read_sfnt_face(cached.bytes);
        if (!names) {
            cache.diagnostics.push_back(cache_warning(path, std::string(face) + " cache file is not a valid "
                                                                "TrueType font: " + names.error().message));
            continue;
        }
        if (!ieq(names.value().full_name, face) && !ieq(names.value().postscript_name, face)) {
            cache.diagnostics.push_back(cache_warning(path, std::string(face) + " cache file holds "
                                                                + names.value().full_name));
            continue;
        }
        cached.names = std::move(names.value());
        cached.sha256 = core::sha256_hex(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(cached.bytes.data()), cached.bytes.size()));
        cache.faces.push_back(std::move(cached));
    }
    return cache;
}

bool is_embedded_face(const std::string_view face) noexcept {
    return std::any_of(embedded_faces.begin(), embedded_faces.end(),
                       [&](const std::string_view embedded) { return ieq(embedded, face); });
}

std::optional<SystemFace> match_system_face(const std::string_view face, const std::span<const std::string> families) {
    const auto family = [&](const std::string_view name) -> const std::string* {
        for (const std::string& candidate : families) {
            if (ieq(candidate, name)) return &candidate;
        }
        return nullptr;
    };
    if (const std::string* whole = family(face)) return SystemFace{*whole, false};
    constexpr std::string_view bold = " Bold";
    if (face.size() > bold.size() && ieq(face.substr(face.size() - bold.size()), bold)) {
        if (const std::string* stem = family(face.substr(0, face.size() - bold.size()))) return SystemFace{*stem, true};
    }
    return std::nullopt;
}

std::string_view to_string(const FaceSource source) noexcept {
    switch (source) {
    case FaceSource::cache: return "cache";
    case FaceSource::system: return "system";
    case FaceSource::engine_default: return "engine_default";
    }
    return "engine_default";
}

ResolvedFont resolve_font(const FontRequest& request, const std::string_view language, const FontCache& cache,
                          const SystemFaceProbe& system) {
    ResolvedFont resolved;
    const bool russian = ieq(language, "russian");
    const bool unicode_only = russian || ieq(language, "japanese");
    resolved.point_size = russian ? std::max(request.point_size, russian_minimum_point_size) : request.point_size;
    std::vector<std::string_view> chain;
    if (!unicode_only) chain.push_back(request.face);
    chain.push_back(unicode_face);
    chain.push_back(last_resort_face);
    for (std::size_t index = 0; index < chain.size(); ++index) {
        const std::string_view face = chain[index];
        const bool repeated = std::any_of(chain.begin(), chain.begin() + static_cast<std::ptrdiff_t>(index),
                                          [&](const std::string_view earlier) { return ieq(earlier, face); });
        if (repeated || face.empty()) continue;
        const bool embedded = is_embedded_face(face);
        const CachedFace* cached = embedded ? cache.find(face) : nullptr;
        if (cached != nullptr || (!embedded && system && system(face))) {
            resolved.face = cached != nullptr ? cached->face : std::string(face);
            resolved.source = cached != nullptr ? FaceSource::cache : FaceSource::system;
            resolved.substituted = !ieq(face, request.face);
            return resolved;
        }
        resolved.unavailable.emplace_back(face);
    }
    resolved.substituted = true;
    return resolved;
}

TextCell gdi_text_cell(const FontCache& cache, const ResolvedFont& resolved, const std::int32_t glyph_height) noexcept {
    if (resolved.source != FaceSource::cache) return {};
    const CachedFace* cached = cache.find(resolved.face);
    if (cached == nullptr || cached->names.units_per_em == 0U || cached->names.win_ascent == 0U) return {};
    const double pixels_per_unit = static_cast<double>(glyph_height) / cached->names.units_per_em;
    return {static_cast<std::int32_t>(std::lround(cached->names.win_ascent * pixels_per_unit)),
            static_cast<std::int32_t>(std::lround(cached->names.win_descent * pixels_per_unit))};
}

} // namespace eawr::presentation::ui
