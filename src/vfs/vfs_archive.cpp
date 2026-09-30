#include "vfs_internal.hpp"
#include <array>
#include <fstream>
#include <limits>
#include <set>

namespace eawr::vfs {

constexpr std::uint64_t meg_header_size = 8;
constexpr std::uint64_t meg_entry_size = 20;
constexpr std::uint64_t max_meg_records = 4'000'000;
constexpr std::uint64_t max_meg_name_bytes = 256ULL * 1024ULL * 1024ULL;

core::Diagnostic error(
    const std::string_view code,
    std::string message,
    std::optional<std::string> logical_path,
    std::optional<std::string> source_id
) {
    return core::Diagnostic{
        .code = std::string(code),
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = std::move(logical_path),
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::move(source_id),
    };
}


std::uint16_t decode_u16(const std::array<unsigned char, 2>& bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t decode_u32(const std::array<unsigned char, 4>& bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool read_exact(std::ifstream& stream, void* target, const std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) return false;
    stream.read(static_cast<char*>(target), static_cast<std::streamsize>(size));
    return stream.gcount() == static_cast<std::streamsize>(size);
}

core::Result<ParsedMeg> parse_meg(
    const std::filesystem::path& archive_path,
    const std::string& source_id,
    const std::string& logical_prefix
) {
    std::error_code ec;
    const auto archive_size = std::filesystem::file_size(archive_path, ec);
    if (ec) {
        return core::Result<ParsedMeg>::failure(error(
            diagnostic_codes::native_io,
            "cannot determine archive size",
            std::nullopt,
            source_id
        ));
    }
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        return core::Result<ParsedMeg>::failure(error(
            diagnostic_codes::native_io,
            "cannot open archive",
            std::nullopt,
            source_id
        ));
    }
    std::array<unsigned char, 4> word{};
    if (!read_exact(input, word.data(), word.size())) {
        return core::Result<ParsedMeg>::failure(error(
            diagnostic_codes::truncated_archive, "truncated MEG header", std::nullopt, source_id));
    }
    const std::uint64_t filename_count = decode_u32(word);
    if (!read_exact(input, word.data(), word.size())) {
        return core::Result<ParsedMeg>::failure(error(
            diagnostic_codes::truncated_archive, "truncated MEG header", std::nullopt, source_id));
    }
    const std::uint64_t entry_count = decode_u32(word);
    if (filename_count > max_meg_records || entry_count > max_meg_records) {
        return core::Result<ParsedMeg>::failure(error(
            diagnostic_codes::archive_limit, "MEG table count exceeds the parser safety limit", std::nullopt, source_id));
    }
    if (entry_count > (archive_size - std::min(archive_size, meg_header_size)) / meg_entry_size) {
        return core::Result<ParsedMeg>::failure(error(
            diagnostic_codes::truncated_archive, "MEG entry table cannot fit in the archive", std::nullopt, source_id));
    }

    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(filename_count));
    std::uint64_t name_bytes = 0;
    for (std::uint64_t index = 0; index < filename_count; ++index) {
        std::array<unsigned char, 2> length_bytes{};
        if (!read_exact(input, length_bytes.data(), length_bytes.size())) {
            return core::Result<ParsedMeg>::failure(error(
                diagnostic_codes::truncated_archive, "truncated MEG filename length", std::nullopt, source_id));
        }
        const auto length = static_cast<std::uint64_t>(decode_u16(length_bytes));
        if (name_bytes > max_meg_name_bytes - length) {
            return core::Result<ParsedMeg>::failure(error(
                diagnostic_codes::archive_limit, "MEG filename table exceeds the parser safety limit", std::nullopt, source_id));
        }
        name_bytes += length;
        std::string name(static_cast<std::size_t>(length), '\0');
        if (!read_exact(input, name.data(), name.size())) {
            return core::Result<ParsedMeg>::failure(error(
                diagnostic_codes::truncated_archive, "truncated MEG filename", std::nullopt, source_id));
        }
        if (name.find('\0') != std::string::npos) {
            return core::Result<ParsedMeg>::failure(error(
                diagnostic_codes::archive_index, "MEG filename contains an embedded NUL", std::nullopt, source_id));
        }
        names.push_back(std::move(name));
    }

    struct RawEntry { std::uint32_t size; std::uint32_t offset; std::uint32_t name_index; };
    std::vector<RawEntry> raw_entries;
    raw_entries.reserve(static_cast<std::size_t>(entry_count));
    for (std::uint64_t index = 0; index < entry_count; ++index) {
        std::array<unsigned char, 20> bytes{};
        if (!read_exact(input, bytes.data(), bytes.size())) {
            return core::Result<ParsedMeg>::failure(error(
                diagnostic_codes::truncated_archive, "truncated MEG entry table", std::nullopt, source_id));
        }
        auto field = [&bytes](const std::size_t at) {
            std::array<unsigned char, 4> value{bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]};
            return decode_u32(value);
        };
        raw_entries.push_back(RawEntry{field(8), field(12), field(16)});
    }

    ParsedMeg parsed;
    parsed.probe = ArchiveProbe{
        .source_id = source_id,
        .archive_size = archive_size,
        .entry_count = entry_count,
        .filename_count = filename_count,
        .format = "MEG-v1",
    };
    parsed.entries.reserve(raw_entries.size());
    std::set<std::string> seen;
    for (const auto& raw : raw_entries) {
        if (raw.name_index >= names.size()) {
            return core::Result<ParsedMeg>::failure(error(
                diagnostic_codes::archive_index, "MEG entry has an invalid filename index", std::nullopt, source_id));
        }
        const auto offset = static_cast<std::uint64_t>(raw.offset);
        const auto size = static_cast<std::uint64_t>(raw.size);
        if (offset > archive_size || size > archive_size - offset) {
            return core::Result<ParsedMeg>::failure(error(
                diagnostic_codes::archive_bounds, "MEG entry extends beyond the archive", names[raw.name_index], source_id));
        }
        std::string original = names[raw.name_index];
        if (!logical_prefix.empty()) {
            original = logical_prefix + "/" + original;
        }
        auto canonical = canonicalize(original);
        if (!canonical) {
            auto diagnostic = canonical.error();
            diagnostic.code = std::string(diagnostic_codes::archive_index);
            diagnostic.message = "invalid logical path in MEG: " + diagnostic.message;
            diagnostic.source_id = source_id;
            return core::Result<ParsedMeg>::failure(std::move(diagnostic));
        }
        if (!seen.insert(canonical.value()).second) {
            return core::Result<ParsedMeg>::failure(error(
                diagnostic_codes::archive_path_collision,
                "MEG contains duplicate case-insensitive logical paths",
                canonical.value(),
                source_id
            ));
        }
        parsed.entries.push_back(MegEntry{
            .canonical_path = std::move(canonical.value()),
            .original_path = std::move(original),
            .offset = offset,
            .size = size,
        });
    }
    return core::Result<ParsedMeg>::success(std::move(parsed));
}


} // namespace eawr::vfs
