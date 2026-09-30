#pragma once

// TrueType (sfnt) face validation for the EmpireAtWar fonts the player
// extracts from their own FoC executable (docs/ui/ui-layer.md, UI-05 #191).
// It applies the checks of tools/fonts/extract_eaw_fonts.py: the table
// directory, every table checksum, the whole-font checksum adjustment, the
// required TrueType tables and the name table. Engine-free.

#include "eawr/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace eawr::data::ui {

namespace diagnostic_codes {
inline constexpr std::string_view sfnt_invalid = "EAWR-UI-0401";
} // namespace diagnostic_codes

struct SfntFace final {
    std::string full_name;       // name ID 4, as game data names a face: `EmpireAtWar-Bold`
    std::string family;          // name ID 1
    std::string subfamily;       // name ID 2
    std::string postscript_name; // name ID 6
    // Vertical metrics in font units: head unitsPerEm, the hhea ascender and
    // descender, and the OS/2 usWinAscent and usWinDescent that GDI builds its
    // text cell from. Zero when OS/2 is absent or too short to hold them.
    std::uint16_t units_per_em{};
    std::int16_t ascender{};
    std::int16_t descender{};
    std::uint16_t win_ascent{};
    std::uint16_t win_descent{};
};

// The names of the TrueType font in `bytes`, or an EAWR-UI-0401 error saying
// why it is not a valid one. Names are UTF-8 and trimmed; Windows English
// records win over other Windows, Unicode and Macintosh records.
[[nodiscard]] core::Result<SfntFace> read_sfnt_face(std::span<const std::byte> bytes);

} // namespace eawr::data::ui
