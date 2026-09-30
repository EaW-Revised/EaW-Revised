#include "eawr/data/ui/sfnt.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace eawr::data::ui {
namespace {

constexpr std::size_t max_tables = 128U;
constexpr std::size_t max_face_bytes = 4U * 1024U * 1024U;
constexpr std::uint32_t head_magic = 0x5F0F3CF5U;
constexpr std::uint32_t font_checksum = 0xB1B0AFBAU;
constexpr std::size_t head_adjustment = 8U;
constexpr std::array<std::string_view, 9> required_tables{
    "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp", "name", "post"};

struct Table final {
    std::string tag;
    std::uint32_t checksum{};
    std::size_t offset{};
    std::size_t length{};
};

core::Result<SfntFace> invalid(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::sfnt_invalid);
    diagnostic.severity = core::Severity::error;
    diagnostic.message = std::move(message);
    return core::Result<SfntFace>::failure(std::move(diagnostic));
}

std::uint32_t read_u16(const std::span<const std::byte> bytes, const std::size_t offset) {
    return (std::to_integer<std::uint32_t>(bytes[offset]) << 8U) | std::to_integer<std::uint32_t>(bytes[offset + 1U]);
}

std::uint32_t read_u32(const std::span<const std::byte> bytes, const std::size_t offset) {
    return (read_u16(bytes, offset) << 16U) | read_u16(bytes, offset + 2U);
}

// Sum of big-endian words over [offset, offset + length), zero-padded to four
// bytes, skipping the head table's adjustment word at `skip` when given.
std::uint32_t checksum(const std::span<const std::byte> bytes, const std::size_t offset, const std::size_t length,
                       const std::size_t skip = SIZE_MAX) {
    std::uint32_t sum = 0U;
    for (std::size_t word = offset; word < offset + length; word += 4U) {
        if (word == skip) continue;
        std::uint32_t value = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            const std::size_t at = word + index;
            const std::uint32_t byte = at < offset + length ? std::to_integer<std::uint32_t>(bytes[at]) : 0U;
            value |= byte << (8U * (3U - index));
        }
        sum += value;
    }
    return sum;
}

void append_utf8(std::string& output, const std::uint32_t code) {
    if (code < 0x80U) {
        output.push_back(static_cast<char>(code));
    } else if (code < 0x800U) {
        output.push_back(static_cast<char>(0xC0U | (code >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else if (code < 0x10000U) {
        output.push_back(static_cast<char>(0xE0U | (code >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (code >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
}

// UTF-16BE (Unicode and Windows platforms) or Mac Roman, whose upper half
// becomes U+FFFD: the faces this reads name themselves in ASCII.
std::string decode_name(const std::span<const std::byte> text, const bool utf16) {
    std::string output;
    if (!utf16) {
        for (const std::byte byte : text) {
            const auto value = std::to_integer<std::uint32_t>(byte);
            append_utf8(output, value < 0x80U ? value : 0xFFFDU);
        }
        return output;
    }
    for (std::size_t index = 0U; index + 1U < text.size(); index += 2U) {
        std::uint32_t unit = read_u16(text, index);
        if (unit >= 0xD800U && unit < 0xDC00U && index + 3U < text.size()) {
            const std::uint32_t low = read_u16(text, index + 2U);
            if (low >= 0xDC00U && low < 0xE000U) {
                unit = 0x10000U + ((unit - 0xD800U) << 10U) + (low - 0xDC00U);
                index += 2U;
            }
        }
        append_utf8(output, unit >= 0xD800U && unit < 0xE000U ? 0xFFFDU : unit);
    }
    return output;
}

std::string trim(std::string text) {
    const auto space = [](const char character) { return character == ' ' || character == '\t'; };
    while (!text.empty() && space(text.back())) text.pop_back();
    const auto first = std::find_if_not(text.begin(), text.end(), space);
    text.erase(text.begin(), first);
    return text;
}

core::Result<SfntFace> read_names(const std::span<const std::byte> table) {
    if (table.size() < 6U) return invalid("name table is shorter than its header");
    const std::uint32_t version = read_u16(table, 0U);
    const std::size_t count = read_u16(table, 2U);
    const std::size_t strings = read_u16(table, 4U);
    if (version > 1U || 6U + 12U * count > table.size() || strings > table.size()) {
        return invalid("name table header is out of range");
    }
    // name id -> (rank, text); Windows English first, then any Windows,
    // Unicode, Macintosh.
    std::map<std::uint32_t, std::pair<int, std::string>> ranked;
    for (std::size_t record = 0U; record < count; ++record) {
        const std::size_t at = 6U + 12U * record;
        const std::uint32_t platform = read_u16(table, at);
        const std::uint32_t encoding = read_u16(table, at + 2U);
        const std::uint32_t language = read_u16(table, at + 4U);
        const std::uint32_t name_id = read_u16(table, at + 6U);
        const std::size_t length = read_u16(table, at + 8U);
        const std::size_t begin = strings + read_u16(table, at + 10U);
        if (begin + length > table.size()) {
            return invalid("name record " + std::to_string(name_id) + " is out of range");
        }
        const bool utf16 = platform == 0U || platform == 3U;
        if (!utf16 && !(platform == 1U && encoding == 0U)) continue;
        const int rank = platform == 3U ? (language == 0x409U ? 0 : 1) : platform == 0U ? 2 : 3;
        const auto found = ranked.find(name_id);
        if (found != ranked.end() && found->second.first <= rank) continue;
        ranked[name_id] = {rank, trim(decode_name(table.subspan(begin, length), utf16))};
    }
    const auto name = [&](const std::uint32_t id) {
        const auto found = ranked.find(id);
        return found == ranked.end() ? std::string{} : found->second.second;
    };
    SfntFace face{name(4U), name(1U), name(2U), name(6U)};
    if (face.full_name.empty()) return invalid("name table has no full name");
    return core::Result<SfntFace>::success(std::move(face));
}

} // namespace

core::Result<SfntFace> read_sfnt_face(const std::span<const std::byte> bytes) {
    if (bytes.size() < 12U) return invalid("shorter than a TrueType table directory");
    const std::uint32_t version = read_u32(bytes, 0U);
    if (version != 0x00010000U && version != 0x74727565U /* 'true' */) return invalid("not a TrueType font");
    const std::size_t count = read_u16(bytes, 4U);
    if (count < 1U || count > max_tables) return invalid("table count out of range");
    std::size_t selector = 0U;
    while ((std::size_t{2} << selector) <= count) ++selector;
    const std::size_t search_range = std::size_t{16} << selector;
    if (read_u16(bytes, 6U) != search_range || read_u16(bytes, 8U) != selector
        || read_u16(bytes, 10U) != count * 16U - search_range) {
        return invalid("table directory search fields disagree with the table count");
    }
    const std::size_t directory_end = 12U + 16U * count;
    if (directory_end > bytes.size()) return invalid("table directory runs past the end of the file");
    std::vector<Table> tables;
    for (std::size_t index = 0U; index < count; ++index) {
        const std::size_t at = 12U + 16U * index;
        Table table;
        for (std::size_t character = 0U; character < 4U; ++character) {
            const auto value = std::to_integer<unsigned char>(bytes[at + character]);
            if (value < 0x20U || value > 0x7EU) return invalid("table tag is not printable");
            table.tag.push_back(static_cast<char>(value));
        }
        table.checksum = read_u32(bytes, at + 4U);
        table.offset = read_u32(bytes, at + 8U);
        table.length = read_u32(bytes, at + 12U);
        if (!tables.empty() && tables.back().tag >= table.tag) return invalid("table tags are not sorted");
        if (table.offset % 4U != 0U || table.offset < directory_end || table.offset + table.length > max_face_bytes) {
            return invalid("table '" + table.tag + "' is misplaced");
        }
        if (table.offset + table.length > bytes.size()) return invalid("the font runs past the end of the file");
        tables.push_back(std::move(table));
    }
    std::vector<const Table*> by_offset;
    for (const Table& table : tables) by_offset.push_back(&table);
    std::sort(by_offset.begin(), by_offset.end(),
              [](const Table* left, const Table* right) { return left->offset < right->offset; });
    for (std::size_t index = 1U; index < by_offset.size(); ++index) {
        if (by_offset[index]->offset < by_offset[index - 1U]->offset + by_offset[index - 1U]->length) {
            return invalid("tables '" + by_offset[index - 1U]->tag + "' and '" + by_offset[index]->tag + "' overlap");
        }
    }
    const auto find = [&](const std::string_view tag) -> const Table* {
        for (const Table& table : tables) {
            if (table.tag == tag) return &table;
        }
        return nullptr;
    };
    std::string missing;
    for (const std::string_view tag : required_tables) {
        if (find(tag) == nullptr) missing += (missing.empty() ? "" : ", ") + std::string(tag);
    }
    if (!missing.empty()) return invalid("required tables missing: " + missing);
    const Table& head = *find("head");
    if (head.length < 54U || read_u32(bytes, head.offset + 12U) != head_magic) {
        return invalid("head table has no magic number");
    }
    const std::size_t adjustment_at = head.offset + head_adjustment;
    for (const Table& table : tables) {
        const std::size_t skip = &table == &head ? adjustment_at : SIZE_MAX;
        if (checksum(bytes, table.offset, table.length, skip) != table.checksum) {
            return invalid("table '" + table.tag + "' checksum mismatch");
        }
    }
    std::size_t end = directory_end;
    for (const Table& table : tables) end = std::max(end, table.offset + table.length);
    if (font_checksum - checksum(bytes, 0U, end, adjustment_at) != read_u32(bytes, adjustment_at)) {
        return invalid("whole-font checksum adjustment mismatch");
    }
    const Table& name = *find("name");
    auto face = read_names(bytes.subspan(name.offset, name.length));
    if (!face) return face;
    face.value().units_per_em = static_cast<std::uint16_t>(read_u16(bytes, head.offset + 18U));
    if (const Table& hhea = *find("hhea"); hhea.length >= 8U) {
        face.value().ascender = static_cast<std::int16_t>(read_u16(bytes, hhea.offset + 4U));
        face.value().descender = static_cast<std::int16_t>(read_u16(bytes, hhea.offset + 6U));
    }
    if (const Table* os2 = find("OS/2"); os2 != nullptr && os2->length >= 78U) {
        face.value().win_ascent = static_cast<std::uint16_t>(read_u16(bytes, os2->offset + 74U));
        face.value().win_descent = static_cast<std::uint16_t>(read_u16(bytes, os2->offset + 76U));
    }
    return face;
}

} // namespace eawr::data::ui
