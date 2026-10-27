#include "eawr/data/ui/movie.hpp"
#include "eawr/core/text.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/data/tag_trace.hpp"

#include "ui_internal.hpp"
// Relative: the viewer build compiles this file without src/data on its path.
#include "../xml_internal.hpp"

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <utility>

namespace eawr::data::ui {
namespace {

core::Result<HudMovie> failure(const std::string_view code, std::string message, std::string path,
    std::optional<std::string> source = {}) {
    return core::Result<HudMovie>::failure(core::Diagnostic{std::string(code), core::Severity::error,
        std::move(message), std::move(path), {}, {}, std::move(source)});
}

std::string trimmed(const std::string_view text) {
    return core::trim(text, core::xml_whitespace);
}

// Trimmed character data of the first child element `name`, if any.
std::string child_text(const pugi::xml_node node, const std::string_view name) {
    for (const pugi::xml_node child : node.children()) {
        if (child.type() == pugi::node_element && detail::iequals(child.name(), name)) {
            return trimmed(child.child_value());
        }
    }
    return {};
}

bool declares(const pugi::xml_node node, const std::string_view name) {
    for (const pugi::xml_attribute attribute : node.attributes()) {
        if (detail::iequals(attribute.name(), "Name")) return detail::iequals(trimmed(attribute.value()), name);
    }
    return false;
}

} // namespace

MovieFormat movie_format(const std::span<const std::byte> bytes) noexcept {
    const auto starts_with = [bytes](const std::string_view magic) {
        if (bytes.size() < magic.size()) return false;
        for (std::size_t index = 0; index < magic.size(); ++index) {
            if (static_cast<char>(bytes[index]) != magic[index]) return false;
        }
        return true;
    };
    if (starts_with("BIK")) return MovieFormat::bink1;
    if (starts_with("KB2")) return MovieFormat::bink2;
    if (starts_with("OggS")) return MovieFormat::ogg;
    return MovieFormat::unknown;
}

std::string_view to_string(const MovieFormat format) noexcept {
    switch (format) {
    case MovieFormat::bink1: return "Bink 1";
    case MovieFormat::bink2: return "Bink 2";
    case MovieFormat::ogg: return "Ogg";
    case MovieFormat::unknown: break;
    }
    return "unknown";
}

core::Result<HudMovie> resolve_hud_movie(const vfs::Vfs& filesystem, const std::string_view name) {
    const std::string registry(movies_xml_path);
    const auto record = filesystem.stat(registry);
    const auto bytes = filesystem.open(registry);
    if (!record || !bytes) return failure(diagnostic_codes::movie_catalog, "movies.xml is missing", registry);
    auto parsed = parse_document(bytes.value(), record.value());
    if (!parsed) {
        core::Diagnostic error = parsed.error();
        error.code = std::string(diagnostic_codes::movie_catalog);
        return core::Result<HudMovie>::failure(std::move(error));
    }

    pugi::xml_node selected;
    for (const pugi::xml_node node : parsed.value().root.children()) {
        if (node.type() == pugi::node_element && detail::iequals(node.name(), "Movie") && declares(node, name)) {
            selected = node;
        }
    }
    if (!selected) {
        return failure(diagnostic_codes::movie_unknown, "movie \"" + std::string(name) + "\" is not declared",
            registry, record.value().source_id);
    }

    const std::string file = child_text(selected, "Movie_File");
    if (file.empty()) {
        return failure(diagnostic_codes::movie_file, "movie \"" + std::string(name) + "\" names no Movie_File",
            registry, record.value().source_id);
    }
    auto source = filesystem.stat(file);
    const auto movie_bytes = filesystem.open(file);
    if (!source || !movie_bytes) {
        const auto logical = vfs::canonicalize(file);
        return failure(diagnostic_codes::movie_file,
            "Movie_File of \"" + std::string(name) + "\" is not in the game data", logical ? logical.value() : file);
    }

    HudMovie movie;
    movie.name = std::string(name);
    movie.source = std::move(source.value());
    movie.format = movie_format(movie_bytes.value());
    if (movie.format != MovieFormat::bink1) {
        return failure(diagnostic_codes::movie_format,
            "movie \"" + movie.name + "\" is " + std::string(to_string(movie.format)) + ", not Bink 1",
            movie.source.canonical_path, movie.source.source_id);
    }
    const std::string alpha = child_text(selected, "Alpha");
    movie.alpha = detail::iequals(alpha, "true") || detail::iequals(alpha, "yes") || alpha == "1";
    std::istringstream offset(child_text(selected, "Commandbar_Offset"));
    if (!(offset >> movie.offset_x >> movie.offset_y)) movie.offset_x = movie.offset_y = 0.0F;

    const auto& content = movie_bytes.value();
    movie.cache_key = core::sha256_hex({reinterpret_cast<const std::uint8_t*>(content.data()), content.size()}) +
        "-" + std::string(movie_cache_recipe) + (movie.alpha ? "-alpha" : "-rgb");
    return core::Result<HudMovie>::success(std::move(movie));
}

std::string movie_cache_file(const HudMovie& movie) {
    return movie.cache_key + ".ogv";
}

core::Result<SpeechMovie> resolve_speech_movie(const vfs::Vfs& filesystem, const SpeechMovieRequest& request) {
    auto movie = resolve_hud_movie(filesystem, request.movie);
    if (!movie) return core::Result<SpeechMovie>::failure(movie.error());
    return core::Result<SpeechMovie>::success({request.speech, std::move(movie.value())});
}

CommandBarMovieStyle command_bar_movie_style(const vfs::Vfs& filesystem) {
    CommandBarMovieStyle style;
    const auto document = load_document(filesystem, "data/xml/gameconstants.xml");
    if (!document) return style;
    for (const auto& node : document.value().root.children) {
        if (detail::iequals(node.name, "CB_Movie_Offset")) {
            std::istringstream input(node.raw_text);
            float x{}, y{};
            if (input >> x >> y && std::isfinite(x) && std::isfinite(y)) {
                style.offset_x = x; style.offset_y = y;
            }
            tag_trace::used(node);
        } else if (detail::iequals(node.name, "CB_Movie_Color")) {
            std::istringstream input(node.raw_text);
            std::array<float, 4> colour{};
            if (input >> colour[0] >> colour[1] >> colour[2] >> colour[3]
                && std::all_of(colour.begin(), colour.end(), [](float v) { return std::isfinite(v); })) {
                for (auto& channel : colour) channel = std::clamp(channel / 255.0F, 0.0F, 1.0F);
                style.colour = colour;
            }
            tag_trace::used(node);
        }
    }
    return style;
}

} // namespace eawr::data::ui
