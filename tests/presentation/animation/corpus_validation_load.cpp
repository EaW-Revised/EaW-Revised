#include "corpus_validation_support.hpp"

namespace eawr_validation {
[[nodiscard]] std::string hash(const std::span<const std::byte> bytes) {
    return eawr::sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

[[nodiscard]] std::filesystem::path data_root(const std::filesystem::path& root) {
    return root.filename().string() == "Data" ? root : root / "Data";
}

[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) return std::nullopt;
    return bytes;
}

[[nodiscard]] std::optional<std::string> bytes_sha256(const std::optional<std::string>& bytes) {
    if (!bytes) return std::nullopt;
    return hash(std::as_bytes(std::span<const char>(bytes->data(), bytes->size())));
}

[[nodiscard]] std::optional<std::string> file_sha256(const std::filesystem::path& path) {
    return bytes_sha256(read_file(path));
}

// The audit's frozen inputs, as authenticated before the run.
[[nodiscard]] corpus::associations::AssetIdentity asset_identity(const eawr::vfs::AssetRecord& record, std::string sha256) {
    return {record.canonical_path, std::move(sha256), record.layer_id,
        std::string(eawr::vfs::to_string(record.origin)), record.source_id, record.original_path, record.size};
}

[[nodiscard]] corpus::associations::AssetIdentity missing_identity(const std::string& path) {
    return {path, {}, {}, {}, {}, {}, 0};
}

} // namespace eawr_validation
