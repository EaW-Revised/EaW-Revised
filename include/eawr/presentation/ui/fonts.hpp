#pragma once

// Font provisioning (docs/ui/ui-layer.md rule UI-F3, ticket UI-05 #191, owner
// decision D1 in #164). The four EmpireAtWar faces come from a local cache
// the player fills from their own FoC executable with
// tools/fonts/extract_eaw_fonts.py; they are never committed or shipped. Every
// other face is a system font. Engine-free: the cache is read through a VFS
// mount, and the Godot builder turns a resolved face into an engine font.

#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/sfnt.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

namespace diagnostic_codes {
inline constexpr std::string_view font_cache_face = "EAWR-UI-0402"; // a cached face is missing or invalid
} // namespace diagnostic_codes

// The faces FoC embeds in its executable, as game data names them.
inline constexpr std::array<std::string_view, 4> embedded_faces{
    "EmpireAtWar-Bold", "EmpireAtWar-Light", "EmpireAtWar-Medium", "EmpireAtWar-Stencil"};
// UI-F3: the game's default Unicode face, then EaW-Medium.
inline constexpr std::string_view unicode_face = "Arial Unicode MS";
inline constexpr std::string_view last_resort_face = "EmpireAtWar-Medium";
inline constexpr std::int32_t russian_minimum_point_size = 7;

// The logical folder of the font cache: the directory the extraction tool
// writes (by default out/fonts) is mounted as a loose layer with this prefix.
inline constexpr std::string_view font_cache_prefix = "fonts";

struct CachedFace final {
    std::string face;         // one of embedded_faces
    std::string logical_path; // fonts/<face>.ttf
    std::vector<std::byte> bytes;
    std::string sha256;
    data::ui::SfntFace names;
};

struct FontCache final {
    std::string directory; // where the cache was mounted from, for reports
    // Valid faces in embedded_faces order.
    std::vector<CachedFace> faces;
    // One warning per face that is missing or invalid.
    std::vector<core::Diagnostic> diagnostics;

    // Case-insensitive, as game data spells faces both ways (`EmpireAtWar-light`).
    [[nodiscard]] const CachedFace* find(std::string_view face) const noexcept;
};

// The logical path of an embedded face's cache file: `fonts/<face>.ttf`.
[[nodiscard]] std::string font_cache_path(std::string_view face);

// Reads the cache file of every embedded face from `filesystem`, which has the
// cache directory mounted at font_cache_prefix; an unmounted VFS is an empty
// cache. A file that is missing, too large, not a valid TrueType font or named
// for another face is left out with one EAWR-UI-0402 warning; loading the
// cache never fails as a whole.
[[nodiscard]] FontCache load_font_cache(const vfs::Vfs& filesystem, std::string directory = {});

[[nodiscard]] bool is_embedded_face(std::string_view face) noexcept;

// A system font the platform can provide: a family and a weight.
struct SystemFace final {
    std::string family;
    bool bold{};

    friend bool operator==(const SystemFace&, const SystemFace&) = default;
};

// Game data names system faces as GDI does, by family (`Arial Black`) or by
// family and style (`Arial Bold`). The name matches a family in `families`
// (case-insensitive) as a whole, or else as a family plus a trailing ` Bold`.
[[nodiscard]] std::optional<SystemFace> match_system_face(std::string_view face,
                                                          std::span<const std::string> families);

enum class FaceSource : std::uint8_t {
    cache,          // an embedded face from the font cache
    system,         // a system font
    engine_default, // nothing in the chain exists: the engine's own font (project-authored fallback)
};

[[nodiscard]] std::string_view to_string(FaceSource source) noexcept;

struct FontRequest final {
    std::string face;
    std::int32_t point_size{};
};

struct ResolvedFont final {
    std::string face; // the face used; empty for engine_default
    FaceSource source{FaceSource::engine_default};
    std::int32_t point_size{};
    bool substituted{}; // another face than the requested one
    // Faces tried before the one used (or all of them), in chain order.
    std::vector<std::string> unavailable;
};

// Whether a system face exists on this machine.
using SystemFaceProbe = std::function<bool(std::string_view face)>;

// UI-F3. The chain is the requested face, the default Unicode face, then
// EaW-Medium; Russian and Japanese (text DB language words, UI-T4, any case)
// start at the Unicode face, and Russian raises the size to at least 7 pt.
// Embedded faces come only from `cache`, other faces only from `system`.
// When nothing in the chain exists the engine's default font is used.
[[nodiscard]] ResolvedFont resolve_font(const FontRequest& request, std::string_view language,
                                        const FontCache& cache, const SystemFaceProbe& system);

// GDI's text cell for a face from the cache: its OS/2 usWinAscent and
// usWinDescent at `glyph_height` pixels, rounded. The original places text by
// this cell. Zero for other faces, which keep the engine's metrics.
struct TextCell final {
    std::int32_t ascent{};
    std::int32_t descent{};
};
[[nodiscard]] TextCell gdi_text_cell(const FontCache& cache, const ResolvedFont& resolved,
                                     std::int32_t glyph_height) noexcept;

} // namespace eawr::presentation::ui
