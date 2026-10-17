#pragma once

// Native font provisioning (#3963). The four EmpireAtWar faces are embedded in the
// player's own corruption/StarWarsG.exe; this copies them out into a font cache
// directory (the viewer uses Godot user data) with the same structural checks as
// tools/fonts/extract_eaw_fonts.py: every TrueType table directory in the executable
// is validated (table records, every table checksum, the whole-font checksum
// adjustment, required tables, name table) and the four faces must be found exactly.
// An executable that is not a pinned FoC build is refused with a readable message
// unless the caller opts in. Nothing retail is committed or shipped; the cache holds
// only what the player's own files contain. Engine-free; no Python needed.

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

// A StarWarsG.exe the project has verified, by SHA-256 of the whole file.
struct KnownFontBuild final {
    std::string_view sha256;
    std::string_view label;
};

// The size and SHA-256 a pinned build's face must reproduce exactly.
struct PinnedFontFace final {
    std::string_view face;
    std::size_t bytes{};
    std::string_view sha256;
};

// Tests substitute their own synthetic builds; players always get the defaults.
struct FontExtractPolicy final {
    std::span<const KnownFontBuild> builds;
    std::span<const PinnedFontFace> faces;
};

[[nodiscard]] const FontExtractPolicy& default_font_extract_policy() noexcept;

enum class FontExtractStatus : unsigned char {
    extracted, // the faces were written to the cache
    cached,    // valid faces from this executable were already in the cache
    failed,
};

// Mirrors extract_eaw_fonts.py's exit codes.
enum class FontExtractFailure : unsigned char {
    none,
    unreadable,    // no executable, unreadable, or not a Windows executable
    unknown_build, // not a pinned build and the opt-in was not given
    faces,         // faces missing, invalid, ambiguous, or differing in a pinned build
    output,        // the cache directory could not be written
};

struct FontExtractResult final {
    FontExtractStatus status{FontExtractStatus::failed};
    FontExtractFailure failure{FontExtractFailure::none};
    // A readable, player-facing explanation when status is failed.
    std::string message;
    std::vector<std::string> warnings;
    std::string executable_sha256;
    std::string build; // the pinned build's label, empty for an unknown build
};

// corruption/StarWarsG.exe under a game root (or the corruption folder itself),
// matching names case-insensitively; empty when there is none.
[[nodiscard]] std::filesystem::path locate_foc_executable(const std::filesystem::path& game_root);

// Makes `cache_directory` hold the four validated faces and fonts.json for the
// executable under `game_root`. A cache already valid for this exact executable is
// left alone (status cached). Never throws.
[[nodiscard]] FontExtractResult ensure_font_cache(const std::filesystem::path& game_root,
                                                  const std::filesystem::path& cache_directory,
                                                  bool allow_unknown_build,
                                                  const FontExtractPolicy& policy = default_font_extract_policy());

} // namespace eawr::presentation::ui
