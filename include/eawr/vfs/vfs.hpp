#pragma once

#include "eawr/core/result.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include <cstddef>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>
#include <utility>

namespace eawr::vfs {

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

// Where a mounted asset's bytes sit on disk: the file (archive or loose) and the byte offset in it. Only for ordering
// reads; the bytes are read through open().
struct ReadLocation {
    std::string source;
    std::uint64_t offset{0};
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

inline constexpr std::string_view revised_layer_id = "builtin:revised";
inline constexpr std::string_view revised_layer_folder = "Revised";
inline constexpr std::array<std::string_view, 3> revised_presentation_xml{
    "data/xml/revisedselectionsfx.xml", "data/xml/selectioncontrols.xml",
    "data/xml/sfxeventfilesadditions.xml"};

// Inserts the built-in layer before expansion, below all user layers.
// Returns false only when enabled and the configured/package directory is absent.
[[nodiscard]] bool insert_revised_layer(std::vector<LayerRoot>& roots, bool enabled,
    const std::filesystem::path& player_folder, const std::filesystem::path& override_root = {});

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
    // The first min(size, max_bytes) bytes of the effective winner, read without the rest of the asset (#5197: a picker
    // that needs only a header does not read every file whole).
    [[nodiscard]] core::Result<std::vector<std::byte>> open_prefix(std::string_view logical_path,
                                                                   std::uint64_t max_bytes) const;
    // Read one exact candidate returned by this mounted VFS. This lets callers
    // qualify archive entry names independently of their logical mount prefix.
    // When archive_name is supplied, an archive candidate must bear that exact
    // canonical raw entry name, excluding its logical mount prefix.
    [[nodiscard]] core::Result<std::vector<std::byte>> open(const AssetRecord& candidate,
                                                          std::string_view archive_name = {}) const;
    // Streams one exact mounted candidate into a caller-owned, nonempty buffer.
    // Each callback span is valid only during the call and never exceeds buffer.size().
    // Empty assets succeed without callbacks; failures may follow partial delivery.
    [[nodiscard]] core::Result<void> read_chunks(const AssetRecord& candidate, std::span<std::byte> buffer,
        const std::function<void(std::span<const std::byte>)>& consume) const;
    // The disk location of one exact mounted candidate; nullopt when it is not in the mounted view.
    [[nodiscard]] std::optional<ReadLocation> read_location(const AssetRecord& candidate) const;
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
    // Mounted layer IDs in descending precedence, including layers with no assets.
    [[nodiscard]] std::span<const std::string> layer_ids() const noexcept;

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

} // namespace eawr::vfs
