#include "eawr/data/ui/text_database.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <sstream>

namespace eawr::data::ui {
namespace {
using assets::Source;

constexpr std::size_t record_bytes = 3U * sizeof(std::uint32_t);
constexpr std::size_t max_language_length = 32U;
constexpr std::size_t max_file_size = 512U * 1024U * 1024U;

struct RecordHeader final {
    std::uint32_t crc{};
    std::uint32_t value_units{};
    std::uint32_t key_bytes{};
};

core::Diagnostic error(const Source& source, const std::string_view code, std::string message,
                       const std::optional<std::uint64_t> offset = std::nullopt) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = core::Severity::error;
    diagnostic.message = std::move(message);
    diagnostic.logical_path = source.logical_path;
    if (offset) diagnostic.column = *offset + 1U;
    diagnostic.source_id = source.source_id;
    return diagnostic;
}

template <typename T>
core::Result<T> fail(const Source& source, const std::string_view code,
                     std::string message,
                     const std::optional<std::uint64_t> offset = std::nullopt) {
    return core::Result<T>::failure(error(source, code, std::move(message), offset));
}

std::uint32_t read_u32(const std::span<const std::byte> bytes, const std::size_t offset) {
    std::uint32_t value{};
    for (std::size_t index = 0U; index < 4U; ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (8U * index);
    }
    return value;
}

constexpr std::array<std::uint32_t, 256> crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0U; index < 256U; ++index) {
        std::uint32_t value = index;
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            value = (value >> 1U) ^ ((value & 1U) != 0U ? 0xedb88320U : 0U);
        }
        table[index] = value;
    }
    return table;
}
constexpr auto crc_lookup = crc_table();

bool printable_ascii(const std::byte value) {
    const auto character = std::to_integer<unsigned char>(value);
    return character >= 0x20U && character <= 0x7eU;
}

bool ascii_letter(const char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

std::u16string widen_key(const std::string_view key) {
    std::u16string result;
    result.reserve(key.size());
    for (const char character : key) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(byte < 0x80U ? static_cast<char16_t>(byte) : char16_t{0xfffdU});
    }
    return result;
}

core::Diagnostic warning(const Source& source, const std::string_view code,
                         std::string message) {
    auto diagnostic = error(source, code, std::move(message));
    diagnostic.severity = core::Severity::warning;
    return diagnostic;
}

// Groups equal keys inside each equal-CRC run of the (crc, index) order.
void report_duplicates(TextDatabase& database) {
    const auto& entries = database.entries;
    const auto& order = database.crc_order;
    for (std::size_t begin = 0U; begin < order.size();) {
        std::size_t end = begin + 1U;
        while (end < order.size() && entries[order[end]].crc == entries[order[begin]].crc) ++end;
        std::vector<bool> grouped(end - begin, false);
        for (std::size_t first = begin; first < end; ++first) {
            if (grouped[first - begin]) continue;
            TextDuplicate duplicate;
            duplicate.key = entries[order[first]].key;
            duplicate.records.push_back(order[first]);
            duplicate.values_equal = true;
            for (std::size_t other = first + 1U; other < end; ++other) {
                if (grouped[other - begin] || entries[order[other]].key != duplicate.key) continue;
                grouped[other - begin] = true;
                duplicate.records.push_back(order[other]);
                duplicate.values_equal = duplicate.values_equal &&
                    entries[order[other]].value == entries[order[first]].value;
            }
            if (duplicate.records.size() < 2U) continue;
            std::ostringstream message;
            message << "text key '" << duplicate.key << "' is stored "
                    << duplicate.records.size() << " times (records";
            for (const auto record : duplicate.records) message << ' ' << record;
            message << ", values " << (duplicate.values_equal ? "equal" : "differ")
                    << "); the first record wins";
            database.diagnostics.push_back(warning(
                database.source, diagnostic_codes::text_duplicate_key, message.str()));
            database.duplicates.push_back(std::move(duplicate));
        }
        begin = end;
    }
    std::sort(database.duplicates.begin(), database.duplicates.end(),
              [](const TextDuplicate& left, const TextDuplicate& right) {
                  return left.records.front() < right.records.front();
              });
}
} // namespace

std::uint32_t text_key_crc(const std::string_view key) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const char character : key) {
        crc = (crc >> 8U) ^ crc_lookup[(crc ^ static_cast<unsigned char>(character)) & 0xffU];
    }
    return ~crc;
}

core::Result<std::string> text_database_path(const std::string_view language) {
    if (language.empty() || language.size() > max_language_length ||
        !std::all_of(language.begin(), language.end(), ascii_letter)) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(diagnostic_codes::text_language);
        diagnostic.message = "text language must be 1 to 32 ASCII letters, got '" +
            std::string(language) + "'";
        return core::Result<std::string>::failure(std::move(diagnostic));
    }
    return core::Result<std::string>::success(
        "Data/Text/MasterTextFile_" + std::string(language) + ".dat");
}

std::string to_utf8(const std::u16string_view text) {
    std::string result;
    result.reserve(text.size());
    const auto put = [&result](const std::uint32_t code) {
        if (code < 0x80U) {
            result.push_back(static_cast<char>(code));
        } else if (code < 0x800U) {
            result.push_back(static_cast<char>(0xc0U | (code >> 6U)));
            result.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
        } else if (code < 0x10000U) {
            result.push_back(static_cast<char>(0xe0U | (code >> 12U)));
            result.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
            result.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
        } else {
            result.push_back(static_cast<char>(0xf0U | (code >> 18U)));
            result.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3fU)));
            result.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
            result.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
        }
    };
    for (std::size_t index = 0U; index < text.size(); ++index) {
        const std::uint32_t unit = text[index];
        if (unit >= 0xd800U && unit <= 0xdbffU && index + 1U < text.size() &&
            text[index + 1U] >= 0xdc00U && text[index + 1U] <= 0xdfffU) {
            put(0x10000U + ((unit - 0xd800U) << 10U) + (text[index + 1U] - 0xdc00U));
            ++index;
        } else if (unit >= 0xd800U && unit <= 0xdfffU) {
            put(0xfffdU);
        } else {
            put(unit);
        }
    }
    return result;
}

const TextEntry* TextDatabase::find(const std::string_view key) const noexcept {
    const auto crc = text_key_crc(key);
    auto match = std::lower_bound(crc_order.begin(), crc_order.end(), crc,
                                  [this](const std::uint32_t record, const std::uint32_t value) {
                                      return entries[record].crc < value;
                                  });
    for (; match != crc_order.end() && entries[*match].crc == crc; ++match) {
        if (entries[*match].key == key) return &entries[*match];
    }
    return nullptr;
}

std::u16string TextLookup::text(const std::string_view key) {
    if (const auto* entry = database_->find(key)) return entry->value;
    if (missing_.insert(std::string(key)).second) {
        diagnostics_.push_back(warning(
            database_->source, diagnostic_codes::text_missing_key,
            "text key '" + std::string(key) + "' is not in the text database; rendering the key"));
    }
    return widen_key(key);
}

core::Result<TextDatabase> load_text_database(const std::span<const std::byte> bytes,
                                              Source source) {
    const auto layout = diagnostic_codes::text_database_layout;
    if (bytes.size() > max_file_size) {
        return fail<TextDatabase>(source, layout, "text database exceeds 512 MiB safety limit", 0U);
    }
    if (source.stored_size != 0U && source.stored_size != bytes.size()) {
        return fail<TextDatabase>(source, layout,
                                  "provenance size does not match supplied text database bytes", 0U);
    }
    if (bytes.size() < sizeof(std::uint32_t)) {
        return fail<TextDatabase>(source, layout, "text database record-count header is truncated", 0U);
    }
    const std::uint32_t count = read_u32(bytes, 0U);
    const std::uint64_t table_end = sizeof(std::uint32_t) + std::uint64_t{count} * record_bytes;
    if (table_end > bytes.size()) {
        return fail<TextDatabase>(source, layout, "text database record table is truncated",
                                  sizeof(std::uint32_t));
    }

    std::vector<RecordHeader> headers(count);
    std::uint64_t payload{};
    for (std::uint32_t index = 0U; index < count; ++index) {
        const std::size_t offset = sizeof(std::uint32_t) + record_bytes * index;
        headers[index] = {read_u32(bytes, offset), read_u32(bytes, offset + 4U),
                          read_u32(bytes, offset + 8U)};
        payload += 2U * std::uint64_t{headers[index].value_units} + headers[index].key_bytes;
    }
    if (table_end + payload > bytes.size()) {
        return fail<TextDatabase>(source, layout, "text database value or key data is truncated",
                                  table_end);
    }
    if (table_end + payload != bytes.size()) {
        return fail<TextDatabase>(source, layout, "text database has unexplained trailing bytes",
                                  table_end + payload);
    }

    TextDatabase result;
    result.source = std::move(source);
    result.entries.resize(count);
    auto cursor = static_cast<std::size_t>(table_end);
    for (std::uint32_t index = 0U; index < count; ++index) {
        auto& value = result.entries[index].value;
        value.resize(headers[index].value_units);
        for (auto& unit : value) {
            unit = static_cast<char16_t>(std::to_integer<std::uint16_t>(bytes[cursor]) |
                                         (std::to_integer<std::uint16_t>(bytes[cursor + 1U]) << 8U));
            cursor += 2U;
        }
    }
    for (std::uint32_t index = 0U; index < count; ++index) {
        const auto raw = bytes.subspan(cursor, headers[index].key_bytes);
        const auto offset = cursor;
        cursor += raw.size();
        if (raw.empty()) {
            return fail<TextDatabase>(result.source, diagnostic_codes::text_database_record,
                                      "text database record " + std::to_string(index) +
                                          " has an empty key", offset);
        }
        if (!std::all_of(raw.begin(), raw.end(), printable_ascii)) {
            return fail<TextDatabase>(result.source, diagnostic_codes::text_database_record,
                                      "text database record " + std::to_string(index) +
                                          " key is not printable ASCII", offset);
        }
        auto& entry = result.entries[index];
        entry.key.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
        entry.crc = headers[index].crc;
        if (text_key_crc(entry.key) != entry.crc) {
            std::ostringstream message;
            message << "text database record " << index << " key '" << entry.key
                    << "' does not match its stored CRC-32";
            return fail<TextDatabase>(result.source, diagnostic_codes::text_database_record,
                                      message.str(), sizeof(std::uint32_t) + record_bytes * index);
        }
    }

    result.sorted_by_crc = std::is_sorted(
        result.entries.begin(), result.entries.end(),
        [](const TextEntry& left, const TextEntry& right) { return left.crc < right.crc; });
    result.crc_order.resize(count);
    for (std::uint32_t index = 0U; index < count; ++index) result.crc_order[index] = index;
    std::stable_sort(result.crc_order.begin(), result.crc_order.end(),
                     [&entries = result.entries](const std::uint32_t left, const std::uint32_t right) {
                         return entries[left].crc < entries[right].crc;
                     });
    report_duplicates(result);
    return core::Result<TextDatabase>::success(std::move(result));
}

core::Result<TextDatabase> load_text_database(const vfs::Vfs& filesystem,
                                              const std::string_view logical_path) {
    auto record = filesystem.stat(logical_path);
    if (!record) return core::Result<TextDatabase>::failure(record.error());
    if (record.value().size > max_file_size) {
        return fail<TextDatabase>(assets::source_from(record.value()),
                                  diagnostic_codes::text_database_layout,
                                  "text database exceeds 512 MiB safety limit before read");
    }
    auto bytes = filesystem.open(logical_path);
    if (!bytes) return core::Result<TextDatabase>::failure(bytes.error());
    return load_text_database(bytes.value(), assets::source_from(record.value()));
}

core::Result<TextDatabase> load_language_text_database(const vfs::Vfs& filesystem,
                                                       const std::string_view language) {
    auto path = text_database_path(language);
    if (!path) return core::Result<TextDatabase>::failure(path.error());
    return load_text_database(filesystem, path.value());
}

} // namespace eawr::data::ui
