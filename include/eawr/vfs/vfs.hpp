#pragma once

#include "eawr/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>
#include <utility>

namespace eawr::vfs {

enum class AssetOrigin : std::uint8_t {
    loose,
    archive,
};

struct ArchiveSpec {
    std::filesystem::path path;
    std::string source_id;
    std::string logical_prefix;
};

// Mounts are supplied highest-precedence first (MODPATH leaf, its dependencies,
// expansion, then base). active_archives are supplied lowest-precedence first.
struct MountSpec {
    std::string layer_id;
    std::filesystem::path data_root;
    std::string loose_logical_prefix{"data"};
    std::vector<ArchiveSpec> active_archives;
};

struct AssetRecord {
    std::string canonical_path;
    std::string original_path;
    std::string layer_id;
    AssetOrigin origin{AssetOrigin::loose};
    std::uint64_t size{0};
    std::string source_id;

    friend bool operator==(const AssetRecord&, const AssetRecord&) = default;
};

struct ArchiveProbe {
    std::string source_id;
    std::uint64_t archive_size{0};
    std::uint64_t entry_count{0};
    std::uint64_t filename_count{0};
    std::string format{"MEG-v1"};
};

struct ManifestResolution {
    MountSpec mount;
    std::string manifest_source_id;
    std::vector<std::string> declared_archives;
    std::vector<std::string> missing_archives;
};

namespace diagnostic_codes {
inline constexpr std::string_view invalid_path = "EAWR-VFS-0001";
inline constexpr std::string_view not_found = "EAWR-VFS-0002";
inline constexpr std::string_view native_io = "EAWR-VFS-0003";
inline constexpr std::string_view loose_case_collision = "EAWR-VFS-0004";
inline constexpr std::string_view truncated_archive = "EAWR-VFS-0005";
inline constexpr std::string_view archive_bounds = "EAWR-VFS-0006";
inline constexpr std::string_view archive_index = "EAWR-VFS-0007";
inline constexpr std::string_view archive_limit = "EAWR-VFS-0008";
inline constexpr std::string_view archive_path_collision = "EAWR-VFS-0009";
inline constexpr std::string_view manifest_invalid = "EAWR-VFS-0010";
inline constexpr std::string_view mount_invalid = "EAWR-VFS-0011";
inline constexpr std::string_view manifest_absent = "EAWR-VFS-0012";
} // namespace diagnostic_codes

[[nodiscard]] core::Result<std::string> canonicalize(std::string_view logical_path);

// Resolves only manifest-declared archives plus the engine's exact conventional
// SFX and patch slots. It never mounts an arbitrary discovered *.meg file.
// Base/expansion require a manifest; other layers without one mount loose files
// only (ML-09), log manifest_absent, and leave manifest_source_id empty.
[[nodiscard]] core::Result<ManifestResolution> resolve_manifest_mount(
    std::string layer_id,
    const std::filesystem::path& data_root
);

using LayerRoot = std::pair<std::string, std::filesystem::path>;

// A mod may name its containing folder or the data root itself.
[[nodiscard]] std::filesystem::path mod_data_root(const std::filesystem::path& root);

// Split the quoted leaf;parent;... CLI value without losing native path encoding.
[[nodiscard]] std::vector<LayerRoot> mod_chain_roots(const std::filesystem::path& chain);

// Resolve inherited archive declarations at their supplying layer, preserving
// leaf > parent > expansion > base and loose > archive precedence.
[[nodiscard]] core::Result<std::vector<ManifestResolution>> resolve_manifest_chain(
    std::span<const LayerRoot> ordered_roots);

[[nodiscard]] core::Result<ArchiveProbe> probe_meg_archive(
    const std::filesystem::path& archive_path,
    std::string source_id
);

class Vfs final {
public:
    Vfs();
    ~Vfs();
    Vfs(Vfs&&) noexcept;
    Vfs& operator=(Vfs&&) noexcept;
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    [[nodiscard]] static core::Result<Vfs> mount(std::span<const MountSpec> ordered_layers);

    [[nodiscard]] core::Result<std::vector<std::byte>> open(std::string_view logical_path) const;
    // Read one exact candidate returned by this mounted VFS. This lets callers
    // qualify archive entry names independently of their logical mount prefix.
    // When archive_name is supplied, an archive candidate must bear that exact
    // canonical raw entry name, excluding its logical mount prefix.
    [[nodiscard]] core::Result<std::vector<std::byte>> open(const AssetRecord& candidate,
                                                          std::string_view archive_name = {}) const;
    [[nodiscard]] core::Result<AssetRecord> stat(std::string_view logical_path) const;
    [[nodiscard]] core::Result<std::vector<AssetRecord>> enumerate(
        std::string_view prefix = {},
        std::string_view extension = {}
    ) const;
    [[nodiscard]] core::Result<std::vector<AssetRecord>> enumerate_raw(
        std::string_view prefix = {},
        std::string_view extension = {}
    ) const;
    [[nodiscard]] core::Result<std::vector<AssetRecord>> candidates(std::string_view logical_path) const;

    // Immutable decoded resources belong to this mounted content set. The VFS
    // owns their lifetime without depending on the libraries that decode them.
    template<class T> [[nodiscard]] std::shared_ptr<T> attachment() const {
        return std::static_pointer_cast<T>(attachment(typeid(T), []() -> std::shared_ptr<void> {
            return std::make_shared<T>();
        }));
    }

private:
    [[nodiscard]] std::shared_ptr<void> attachment(std::type_index type,
        std::shared_ptr<void> (*create)()) const;
    struct Impl;
    explicit Vfs(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] constexpr std::string_view to_string(const AssetOrigin origin) noexcept {
    return origin == AssetOrigin::loose ? "loose" : "archive";
}

} // namespace eawr::vfs
