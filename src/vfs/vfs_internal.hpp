#pragma once
#include "eawr/vfs/vfs.hpp"
#include <fstream>

namespace eawr::vfs {

struct MegEntry {
    std::string canonical_path;
    std::string original_path;
    std::uint64_t offset{0};
    std::uint64_t size{0};
};

struct ParsedMeg {
    ArchiveProbe probe;
    std::vector<MegEntry> entries;
};

core::Diagnostic error(std::string_view code, std::string message,
    std::optional<std::string> logical_path = std::nullopt,
    std::optional<std::string> source_id = std::nullopt);
bool read_exact(std::ifstream& stream, void* target, std::size_t size);
core::Result<ParsedMeg> parse_meg(
    const std::filesystem::path& archive_path,
    const std::string& source_id,
    const std::string& logical_prefix);

} // namespace eawr::vfs
