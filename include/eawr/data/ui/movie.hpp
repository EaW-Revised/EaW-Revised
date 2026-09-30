#pragma once

// HUD movies (G12, #237; docs/ui/hud-movies.md). A movie name (a story
// COMMANDBAR_MOVIE or MULTIMEDIA argument, a faction's intro movie) resolves
// through the effective data/xml/movies.xml, and its Movie_File resolves
// independently through the same VFS, so a mod can replace either. The file
// is identified by its signature, never by its extension. FoC ships Bink 1,
// which the player cannot decode; the player converts it once with their own
// FFmpeg (tools/ui/convert_hud_movie.py) into a local Theora cache that the
// presentation layer plays. Engine-free.

#include "eawr/core/result.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace eawr::data::ui {

namespace diagnostic_codes {
inline constexpr std::string_view movie_catalog = "EAWR-UI-0701";     // movies.xml missing or malformed
inline constexpr std::string_view movie_unknown = "EAWR-UI-0702";     // name not declared in movies.xml
inline constexpr std::string_view movie_file = "EAWR-UI-0703";        // Movie_File empty or not in the VFS
inline constexpr std::string_view movie_format = "EAWR-UI-0704";      // not Bink 1 (Bink 2 or unknown)
inline constexpr std::string_view movie_unconverted = "EAWR-UI-0705"; // no Theora cache entry for these bytes
inline constexpr std::string_view movie_converter = "EAWR-UI-0706";   // no usable FFmpeg for the conversion
inline constexpr std::string_view movie_conversion = "EAWR-UI-0707";  // conversion or cache write failed
inline constexpr std::string_view movie_undecodable = "EAWR-UI-0708"; // cache entry decodes no frame
} // namespace diagnostic_codes

inline constexpr std::string_view movies_xml_path = "data/xml/movies.xml";

// Bumped when the conversion recipe changes, so old cache entries are not used.
inline constexpr std::string_view movie_cache_recipe = "hud-v1";

enum class MovieFormat { bink1, bink2, ogg, unknown };

// From the first bytes: `BIK` Bink 1, `KB2` Bink 2, `OggS` Ogg.
[[nodiscard]] MovieFormat movie_format(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::string_view to_string(MovieFormat format) noexcept;

struct HudMovie {
    std::string name;          // as declared in movies.xml
    vfs::AssetRecord source;   // the Movie_File as the effective VFS supplies it
    MovieFormat format{MovieFormat::unknown};
    bool alpha{};              // <Alpha>: the portrait is composited over the HUD
    float offset_x{};          // <Commandbar_Offset>, reference units
    float offset_y{};
    // SHA-256 of the source bytes plus the recipe and the alpha packing, so
    // a mod that replaces the bytes selects another entry and identical files
    // under several names share one.
    std::string cache_key;
};

// Resolves `name` (ASCII case-insensitive; a later declaration of the same
// name wins) and identifies its file. Errors: movie_catalog, movie_unknown,
// movie_file, movie_format. Only Bink 1 resolves: it is all FoC and the
// surveyed mods ship, and the only format the conversion recipe covers.
[[nodiscard]] core::Result<HudMovie> resolve_hud_movie(const vfs::Vfs& filesystem, std::string_view name);

// The file name of the movie's converted Theora entry in the player's movie
// cache directory: `<cache_key>.ogv`.
[[nodiscard]] std::string movie_cache_file(const HudMovie& movie);

} // namespace eawr::data::ui
