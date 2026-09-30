#pragma once

// Localised text database (MasterTextFile_<LANGUAGE>.dat), rules UI-T1 to
// UI-T4 in docs/ui/ui-layer.md. Engine-free: values stay UTF-16 as stored.

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::data::ui {

namespace diagnostic_codes {
inline constexpr std::string_view text_database_layout = "EAWR-UI-0101";
inline constexpr std::string_view text_database_record = "EAWR-UI-0102";
inline constexpr std::string_view text_duplicate_key = "EAWR-UI-0103";
inline constexpr std::string_view text_missing_key = "EAWR-UI-0104";
inline constexpr std::string_view text_language = "EAWR-UI-0105";
} // namespace diagnostic_codes

struct TextEntry final {
    std::uint32_t crc{};
    std::string key;
    std::u16string value;
};

// Every record index (file order) of a key stored more than once.
struct TextDuplicate final {
    std::string key;
    std::vector<std::size_t> records;
    bool values_equal{};
};

struct TextDatabase final {
    assets::Source source;
    // File order. Keys are exact bytes: the CRC is over the key as stored, and
    // keys that differ only in case are distinct records.
    std::vector<TextEntry> entries;
    // MasterTextFile is sorted by CRC; CreditsText is in display order.
    bool sorted_by_crc{};
    std::vector<TextDuplicate> duplicates;
    // One warning per duplicated key.
    std::vector<core::Diagnostic> diagnostics;
    // Record indices ordered by (crc, file index); built by the loader.
    std::vector<std::uint32_t> crc_order;

    // Exact key; among duplicates the first record in file order wins (UI-T2,
    // open until observed).
    [[nodiscard]] const TextEntry* find(std::string_view key) const noexcept;
};

// Resolves keys for display (UI-T3): a missing key renders as its own text
// and records one diagnostic the first time it is asked for.
class TextLookup final {
public:
    explicit TextLookup(const TextDatabase& database) noexcept : database_(&database) {}

    [[nodiscard]] std::u16string text(std::string_view key);
    [[nodiscard]] const std::vector<core::Diagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    const TextDatabase* database_;
    std::set<std::string, std::less<>> missing_;
    std::vector<core::Diagnostic> diagnostics_;
};

// CRC-32 (IEEE 802.3, reflected, as zlib) over the key bytes exactly as given.
[[nodiscard]] std::uint32_t text_key_crc(std::string_view key) noexcept;

// UI-T4: Data/Text/MasterTextFile_<LANGUAGE>.dat. The language is one word of
// ASCII letters (for example ENGLISH or German); anything else is rejected.
[[nodiscard]] core::Result<std::string> text_database_path(std::string_view language);

[[nodiscard]] std::string to_utf8(std::u16string_view text);

[[nodiscard]] core::Result<TextDatabase> load_text_database(
    std::span<const std::byte> bytes, assets::Source source);
[[nodiscard]] core::Result<TextDatabase> load_text_database(
    const vfs::Vfs& filesystem, std::string_view logical_path);
[[nodiscard]] core::Result<TextDatabase> load_language_text_database(
    const vfs::Vfs& filesystem, std::string_view language);

} // namespace eawr::data::ui
