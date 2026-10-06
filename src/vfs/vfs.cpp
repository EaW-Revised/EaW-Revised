#include "eawr/core/load_profile.hpp"
#include "vfs_internal.hpp"
#include "eawr/vfs/vfs.hpp"

#include "eawr/core/diagnostic.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>

namespace eawr::vfs {
namespace {

constexpr std::uint64_t max_asset_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

bool valid_utf8(const std::string_view bytes) {
    for (std::size_t index = 0; index < bytes.size();) {
        const auto lead = static_cast<unsigned char>(bytes[index]);
        std::size_t count = 0;
        std::uint32_t value = 0;
        if (lead <= 0x7fU) {
            count = 1;
            value = lead;
        } else if (lead >= 0xc2U && lead <= 0xdfU) {
            count = 2;
            value = lead & 0x1fU;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            count = 3;
            value = lead & 0x0fU;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            count = 4;
            value = lead & 0x07U;
        } else {
            return false;
        }
        if (index + count > bytes.size()) return false;
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto continuation = static_cast<unsigned char>(bytes[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (continuation & 0x3fU);
        }
        if ((count == 3 && value < 0x800U) || (count == 4 && value < 0x10000U) ||
            value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) {
            return false;
        }
        index += count;
    }
    return true;
}

bool valid_utf16(const std::string_view bytes, const bool little_endian) {
    if (bytes.size() % 2U != 0U) return false;
    auto unit = [&](const std::size_t offset) {
        const auto first = static_cast<unsigned char>(bytes[offset]);
        const auto second = static_cast<unsigned char>(bytes[offset + 1]);
        return static_cast<std::uint16_t>(little_endian ? first | (second << 8U) : (first << 8U) | second);
    };
    for (std::size_t offset = 0; offset < bytes.size(); offset += 2) {
        const auto value = unit(offset);
        if (value >= 0xd800U && value <= 0xdbffU) {
            if (offset + 3 >= bytes.size()) return false;
            const auto low = unit(offset + 2);
            if (low < 0xdc00U || low > 0xdfffU) return false;
            offset += 2;
        } else if (value >= 0xdc00U && value <= 0xdfffU) {
            return false;
        }
    }
    return true;
}

bool valid_utf32(const std::string_view bytes, const bool little_endian) {
    if (bytes.size() % 4U != 0U) return false;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 4) {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const auto source = little_endian ? offset + 3U - index : offset + index;
            value = (value << 8U) | static_cast<unsigned char>(bytes[source]);
        }
        if (value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) return false;
    }
    return true;
}

bool valid_xml_encoding(const std::string_view bytes, const pugi::xml_encoding encoding) {
    switch (encoding) {
    case pugi::encoding_utf8: return valid_utf8(bytes);
    case pugi::encoding_utf16_le: return valid_utf16(bytes, true);
    case pugi::encoding_utf16_be: return valid_utf16(bytes, false);
    case pugi::encoding_utf32_le: return valid_utf32(bytes, true);
    case pugi::encoding_utf32_be: return valid_utf32(bytes, false);
    default: return false;
    }
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string ascii_lower(std::string value) {
    for (char& ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) {
            ch = static_cast<char>(byte + ('a' - 'A'));
        }
    }
    return value;
}

bool ascii_iequals(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(left[i]);
        unsigned char b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

core::Result<std::filesystem::path> find_case_insensitive(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    const std::string& source_id
) {
    auto current = root;
    for (const auto& component : relative) {
        const auto wanted = path_utf8(component);
        if (wanted.empty() || wanted == ".") continue;
        if (wanted == "..") {
            return core::Result<std::filesystem::path>::failure(error(
                diagnostic_codes::invalid_path, "relative native path contains traversal", wanted, source_id));
        }
        std::error_code ec;
        std::vector<std::filesystem::path> matches;
        for (std::filesystem::directory_iterator it(current, ec), end; !ec && it != end; it.increment(ec)) {
            if (ascii_iequals(path_utf8(it->path().filename()), wanted)) matches.push_back(it->path());
        }
        if (ec) {
            return core::Result<std::filesystem::path>::failure(native_io_error(
                current, "cannot read directory", std::nullopt, source_id));
        }
        if (matches.empty()) {
            return core::Result<std::filesystem::path>::failure(error(
                diagnostic_codes::not_found, "case-insensitive native path was not found", wanted, source_id));
        }
        if (matches.size() != 1) {
            return core::Result<std::filesystem::path>::failure(error(
                diagnostic_codes::loose_case_collision, "native path component has a case-insensitive collision", wanted, source_id));
        }
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(matches.front(), status_error);
        if (status_error) return core::Result<std::filesystem::path>::failure(native_io_error(
            matches.front(), "cannot inspect game path", std::nullopt, source_id));
        if (std::filesystem::is_symlink(status)) {
            return core::Result<std::filesystem::path>::failure(error(
                diagnostic_codes::invalid_path,
                "mounted paths may not traverse a symbolic link",
                wanted,
                source_id
            ));
        }
        current = std::move(matches.front());
    }
    return core::Result<std::filesystem::path>::success(std::move(current));
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string normalize_declared_archive(std::string text) {
    text = trim(std::move(text));
    std::replace(text.begin(), text.end(), '\\', '/');
    while (text.starts_with("./")) text.erase(0, 2);
    if (ascii_lower(text).starts_with("data/")) text.erase(0, 5);
    return text;
}

bool has_extension(const std::string& path, std::string_view extension) {
    if (extension.empty()) return true;
    std::string normalized(extension);
    if (!normalized.empty() && normalized.front() != '.') normalized.insert(normalized.begin(), '.');
    normalized = ascii_lower(std::move(normalized));
    if (path.size() < normalized.size()) return false;
    return path.compare(path.size() - normalized.size(), normalized.size(), normalized) == 0;
}

bool has_prefix(const std::string& path, const std::string& prefix) {
    if (prefix.empty()) return true;
    if (!path.starts_with(prefix)) return false;
    return path.size() == prefix.size() || prefix.back() == '/' || path[prefix.size()] == '/';
}

} // namespace

core::Result<std::string> canonicalize(const std::string_view logical_path) {
    if (logical_path.empty()) {
        return core::Result<std::string>::failure(error(diagnostic_codes::invalid_path, "logical path is empty"));
    }
    std::string value(logical_path);
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.starts_with('/') || value.starts_with("//") ||
        (value.size() >= 2 && ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z')) && value[1] == ':')) {
        return core::Result<std::string>::failure(error(
            diagnostic_codes::invalid_path, "absolute and drive-qualified logical paths are forbidden", value));
    }
    std::vector<std::string> components;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find('/', begin);
        const auto component = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!component.empty() && component != ".") {
            if (component == "..") {
                if (components.empty()) {
                    return core::Result<std::string>::failure(error(
                        diagnostic_codes::invalid_path, "logical path traverses above the mounted root", value));
                }
                components.pop_back();
            } else {
                if (component.find('\0') != std::string::npos) {
                    return core::Result<std::string>::failure(error(
                        diagnostic_codes::invalid_path, "logical path contains an embedded NUL", value));
                }
                components.push_back(ascii_lower(component));
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (components.empty()) {
        return core::Result<std::string>::failure(error(diagnostic_codes::invalid_path, "logical path has no components", value));
    }
    std::string result;
    for (const auto& component : components) {
        if (!result.empty()) result.push_back('/');
        result += component;
    }
    return core::Result<std::string>::success(std::move(result));
}

core::Result<ManifestResolution> resolve_manifest_mount(
    std::string layer_id,
    const std::filesystem::path& data_root
) {
    std::error_code root_error;
    const bool directory = std::filesystem::is_directory(data_root, root_error);
    if (root_error) return core::Result<ManifestResolution>::failure(native_io_error(
        data_root, "cannot inspect game directory", std::nullopt, layer_id));
    if (layer_id.empty() || !directory) {
        return core::Result<ManifestResolution>::failure(error(
            diagnostic_codes::mount_invalid, "mount layer id must be non-empty and data root must be a directory", std::nullopt, layer_id));
    }
    auto manifest_path = find_case_insensitive(data_root, "megafiles.xml", layer_id + ":manifest");
    if (!manifest_path) {
        if (manifest_path.error().code != diagnostic_codes::not_found)
            return core::Result<ManifestResolution>::failure(manifest_path.error());
        auto diagnostic = manifest_path.error();
        diagnostic.code = std::string(diagnostic_codes::manifest_invalid);
        diagnostic.message = "MegaFiles.xml is required under '" + path_utf8(data_root) +
            "' to establish active archive ordering. Verify or repair the FoC installation.";
        return core::Result<ManifestResolution>::failure(std::move(diagnostic));
    }
    std::ifstream manifest(manifest_path.value(), std::ios::binary);
    if (!manifest) return core::Result<ManifestResolution>::failure(native_io_error(
        manifest_path.value(), "cannot read manifest", std::nullopt, layer_id + ":manifest"));
    std::ostringstream buffer;
    buffer << manifest.rdbuf();
    if (!manifest || buffer.str().size() > 4ULL * 1024ULL * 1024ULL) {
        return core::Result<ManifestResolution>::failure(error(
            diagnostic_codes::manifest_invalid, "cannot read a bounded MegaFiles.xml", std::nullopt, layer_id + ":manifest"));
    }
    const std::string xml = buffer.str();
    ManifestResolution result;
    result.mount.layer_id = std::move(layer_id);
    result.mount.data_root = data_root;
    result.mount.loose_logical_prefix = "data";
    result.manifest_source_id = result.mount.layer_id + ":Data/MegaFiles.xml";
    std::set<std::string> already;

    auto add_archive = [&](const std::string& declared, const std::string& prefix, const bool record_missing) -> core::Result<void> {
        const auto relative = normalize_declared_archive(declared);
        if (relative.empty()) return core::Result<void>::success();
        auto canonical_relative = canonicalize(relative);
        if (!canonical_relative) {
            auto diagnostic = canonical_relative.error();
            diagnostic.code = std::string(diagnostic_codes::manifest_invalid);
            diagnostic.source_id = result.manifest_source_id;
            return core::Result<void>::failure(std::move(diagnostic));
        }
        if (!already.insert(canonical_relative.value()).second) return core::Result<void>::success();
        auto actual = find_case_insensitive(data_root, std::filesystem::path(relative), result.manifest_source_id);
        if (!actual) {
            if (actual.error().code != diagnostic_codes::not_found) {
                return core::Result<void>::failure(actual.error());
            }
            if (record_missing) result.missing_archives.push_back("Data/" + relative);
            return core::Result<void>::success();
        }
        const std::string source = result.mount.layer_id + ":Data/" + relative;
        result.mount.active_archives.push_back(ArchiveSpec{actual.value(), source, prefix});
        return core::Result<void>::success();
    };

    pugi::xml_document document;
    constexpr unsigned int parse_options =
        pugi::parse_default | pugi::parse_comments | pugi::parse_pi |
        pugi::parse_declaration | pugi::parse_doctype;
    const auto parsed = document.load_buffer(xml.data(), xml.size(), parse_options, pugi::encoding_auto);
    if (!parsed) {
        return core::Result<ManifestResolution>::failure(error(
            diagnostic_codes::manifest_invalid,
            "invalid MegaFiles.xml: " + std::string(parsed.description()) +
                " at byte offset " + std::to_string(parsed.offset),
            std::nullopt,
            result.manifest_source_id));
    }
    if (!valid_xml_encoding(xml, parsed.encoding)) {
        return core::Result<ManifestResolution>::failure(error(
            diagnostic_codes::manifest_invalid,
            "MegaFiles.xml contains an invalid or unsupported character encoding",
            std::nullopt,
            result.manifest_source_id));
    }
    const auto declaration = document.first_child();
    if (declaration.type() == pugi::node_declaration) {
        const std::string_view label = declaration.attribute("encoding").value();
        const bool matches = label.empty() ||
            (ascii_iequals(label, "UTF-8") && parsed.encoding == pugi::encoding_utf8) ||
            (ascii_iequals(label, "UTF-16") &&
             (parsed.encoding == pugi::encoding_utf16_le || parsed.encoding == pugi::encoding_utf16_be)) ||
            (ascii_iequals(label, "UTF-32") &&
             (parsed.encoding == pugi::encoding_utf32_le || parsed.encoding == pugi::encoding_utf32_be));
        if (!matches) {
            return core::Result<ManifestResolution>::failure(error(
                diagnostic_codes::manifest_invalid,
                "MegaFiles.xml declares an unsupported or mismatched character encoding",
                std::nullopt,
                result.manifest_source_id));
        }
    }
    std::size_t document_element_count = 0;
    for (const auto child : document.children()) {
        if (child.type() == pugi::node_doctype) {
            return core::Result<ManifestResolution>::failure(error(
                diagnostic_codes::manifest_invalid,
                "MegaFiles.xml document type declarations are not supported",
                std::nullopt,
                result.manifest_source_id));
        }
        if (child.type() == pugi::node_element) {
            ++document_element_count;
        }
    }
    if (document_element_count != 1) {
        return core::Result<ManifestResolution>::failure(error(
            diagnostic_codes::manifest_invalid,
            "MegaFiles.xml must contain exactly one document element",
            std::nullopt,
            result.manifest_source_id));
    }
    const auto root = document.document_element();
    if (!root || std::string_view(root.name()) != "Mega_Files") {
        return core::Result<ManifestResolution>::failure(error(
            diagnostic_codes::manifest_invalid,
            "MegaFiles.xml root element must be Mega_Files",
            std::nullopt,
            result.manifest_source_id));
    }

    for (const auto file : root.children("File")) {
        std::string declared_text;
        for (const auto child : file.children()) {
            if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
                declared_text += child.value();
            } else if (child.type() != pugi::node_comment && child.type() != pugi::node_pi) {
                return core::Result<ManifestResolution>::failure(error(
                    diagnostic_codes::manifest_invalid,
                    "MegaFiles.xml File entries must contain text only",
                    std::nullopt,
                    result.manifest_source_id));
            }
        }
        const auto declared = normalize_declared_archive(declared_text);
        if (declared.empty()) {
            return core::Result<ManifestResolution>::failure(error(
                diagnostic_codes::manifest_invalid,
                "MegaFiles.xml File entries must not be empty",
                std::nullopt,
                result.manifest_source_id));
        }
        result.declared_archives.push_back("Data/" + declared);
        auto added = add_archive(declared, {}, true);
        if (!added) return core::Result<ManifestResolution>::failure(added.error());
    }
    if (result.declared_archives.empty()) {
        return core::Result<ManifestResolution>::failure(error(
            diagnostic_codes::manifest_invalid, "MegaFiles.xml contains no File entries", std::nullopt, result.manifest_source_id));
    }

    constexpr std::array<std::string_view, 3> sfx{
        "Audio/SFX/SFX2D_English.meg",
        "Audio/SFX/SFX2D_Non_Localized.meg",
        "Audio/SFX/SFX3D_Non_Localized.meg",
    };
    for (const auto path : sfx) {
        auto added = add_archive(std::string(path), "data/audio/sfx", false);
        if (!added) return core::Result<ManifestResolution>::failure(added.error());
    }
    constexpr std::array<std::string_view, 3> patches{"Patch.meg", "Patch2.meg", "64Patch.meg"};
    for (const auto path : patches) {
        auto added = add_archive(std::string(path), {}, false);
        if (!added) return core::Result<ManifestResolution>::failure(added.error());
    }
    return core::Result<ManifestResolution>::success(std::move(result));
}

std::vector<LayerRoot> mod_chain_roots(const std::filesystem::path& chain) {
    std::vector<LayerRoot> roots;
    // A directory whose own name contains ';' (legal on Windows) is one root, not a chain.
    if (!chain.empty() && std::filesystem::is_directory(chain)) {
        auto root = chain;
        if (std::filesystem::is_directory(root / "Data")) root /= "Data";
        roots.emplace_back("mod", root);
        return roots;
    }
    const auto text = chain.native();
    std::size_t begin = 0;
    while (!text.empty() && begin <= text.size()) {
        const auto end = text.find(static_cast<std::filesystem::path::value_type>(';'), begin);
        auto root = std::filesystem::path(text.substr(begin, end == text.npos ? text.npos : end - begin));
        if (!root.empty() && std::filesystem::is_directory(root / "Data")) root /= "Data";
        roots.emplace_back(roots.empty() ? "mod" : "mod-parent-" + std::to_string(roots.size()), root);
        if (end == text.npos) break;
        begin = end + 1;
    }
    return roots;
}

core::Result<std::vector<ManifestResolution>> resolve_manifest_chain(
    const std::span<const LayerRoot> ordered_roots) {
    std::vector<ManifestResolution> results;
    for (const auto& [id, root] : ordered_roots) {
        auto resolved = resolve_manifest_mount(id, root);
        if (!resolved) return core::Result<std::vector<ManifestResolution>>::failure(resolved.error());
        results.push_back(std::move(resolved.value()));
    }
    std::vector<std::size_t> inherited_counts(results.size());
    for (std::size_t leaf = 0; leaf < results.size(); ++leaf) {
        // Retail layers keep their established independent manifest semantics.
        if (results[leaf].mount.layer_id == "expansion" || results[leaf].mount.layer_id == "base") continue;
        auto& missing = results[leaf].missing_archives;
        for (auto entry = missing.begin(); entry != missing.end();) {
            const auto relative = normalize_declared_archive(*entry);
            bool supplied = false;
            for (std::size_t parent = leaf + 1; parent < results.size(); ++parent) {
                auto& mount = results[parent].mount;
                auto actual = find_case_insensitive(mount.data_root, std::filesystem::path(relative), mount.layer_id);
                if (!actual) {
                    if (actual.error().code != diagnostic_codes::not_found &&
                        actual.error().code != diagnostic_codes::native_io)
                        return core::Result<std::vector<ManifestResolution>>::failure(actual.error());
                    continue;
                }
                const auto found = std::find_if(mount.active_archives.begin(), mount.active_archives.end(),
                    [&](const ArchiveSpec& archive) { return archive.path == actual.value(); });
                if (found == mount.active_archives.end()) {
                    // Inherited declarations precede this layer's own manifest and patches.
                    mount.active_archives.insert(mount.active_archives.begin() +
                        static_cast<std::ptrdiff_t>(inherited_counts[parent]++),
                        ArchiveSpec{actual.value(), mount.layer_id + ":Data/" + relative, {}});
                }
                supplied = true;
                break;
            }
            if (supplied) entry = missing.erase(entry);
            else ++entry;
        }
    }
    return core::Result<std::vector<ManifestResolution>>::success(std::move(results));
}

core::Result<ArchiveProbe> probe_meg_archive(
    const std::filesystem::path& archive_path,
    std::string source_id
) {
    auto parsed = parse_meg(archive_path, source_id, {});
    if (!parsed) return core::Result<ArchiveProbe>::failure(parsed.error());
    return core::Result<ArchiveProbe>::success(std::move(parsed.value().probe));
}

struct Vfs::Impl {
    struct StoredAsset {
        AssetRecord record;
        std::filesystem::path native_path;
        std::uint64_t offset{0};
    };
    std::vector<StoredAsset> assets;
    std::unordered_map<std::string, std::vector<std::size_t>> by_path;
};

Vfs::Vfs() = default;
Vfs::~Vfs() = default;
Vfs::Vfs(Vfs&&) noexcept = default;
Vfs& Vfs::operator=(Vfs&&) noexcept = default;
Vfs::Vfs(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

core::Result<Vfs> Vfs::mount(const std::span<const MountSpec> ordered_layers) {
    core::load_profile::Scope load_scope(core::load_profile::Phase::vfs);
    if (ordered_layers.empty()) {
        return core::Result<Vfs>::failure(error(diagnostic_codes::mount_invalid, "at least one layer is required"));
    }
    auto impl = std::make_unique<Impl>();
    std::set<std::string> layer_ids;
    for (const auto& layer : ordered_layers) {
        std::error_code root_error;
        const bool directory = std::filesystem::is_directory(layer.data_root, root_error);
        if (root_error) return core::Result<Vfs>::failure(native_io_error(
            layer.data_root, "cannot inspect game directory", std::nullopt, layer.layer_id));
        if (layer.layer_id.empty() || !layer_ids.insert(ascii_lower(layer.layer_id)).second ||
            !directory) {
            return core::Result<Vfs>::failure(error(
                diagnostic_codes::mount_invalid,
                "layer ids must be unique and non-empty and each data root must be a directory",
                std::nullopt,
                layer.layer_id
            ));
        }

        std::unordered_map<std::string, std::string> loose_seen;
        std::error_code walk_error;
        for (std::filesystem::recursive_directory_iterator it(
                 layer.data_root, std::filesystem::directory_options::none, walk_error), end;
             !walk_error && it != end; it.increment(walk_error)) {
            std::error_code link_error;
            const auto status = it->symlink_status(link_error);
            if (link_error) return core::Result<Vfs>::failure(native_io_error(
                it->path(), "cannot inspect game path", std::nullopt, layer.layer_id));
            if (std::filesystem::is_symlink(status)) {
                return core::Result<Vfs>::failure(error(
                    diagnostic_codes::invalid_path,
                    "mounted loose trees may not contain symbolic links",
                    path_utf8(it->path().lexically_relative(layer.data_root)),
                    layer.layer_id
                ));
            }
            std::error_code type_error;
            const bool regular = it->is_regular_file(type_error);
            if (type_error) return core::Result<Vfs>::failure(native_io_error(
                it->path(), "cannot inspect game file", std::nullopt, layer.layer_id));
            if (!regular) continue;
            if (ascii_iequals(path_utf8(it->path().extension()), ".meg")) continue;
            const auto relative = it->path().lexically_relative(layer.data_root);
            std::string original = path_utf8(relative);
            if (!layer.loose_logical_prefix.empty()) original = layer.loose_logical_prefix + "/" + original;
            auto canonical = canonicalize(original);
            if (!canonical) return core::Result<Vfs>::failure(canonical.error());
            const auto previous = loose_seen.find(canonical.value());
            if (previous != loose_seen.end()) {
                return core::Result<Vfs>::failure(error(
                    diagnostic_codes::loose_case_collision,
                    "same-layer loose files collide case-insensitively: " + previous->second + " and " + original,
                    canonical.value(),
                    layer.layer_id
                ));
            }
            loose_seen.emplace(canonical.value(), original);
            const auto size = std::filesystem::file_size(it->path(), type_error);
            if (type_error) {
                return core::Result<Vfs>::failure(native_io_error(
                    it->path(), "cannot determine loose file size", canonical.value(), layer.layer_id));
            }
            Impl::StoredAsset stored{
                .record = AssetRecord{
                    .canonical_path = canonical.value(),
                    .original_path = original,
                    .layer_id = layer.layer_id,
                    .origin = AssetOrigin::loose,
                    .size = size,
                    .source_id = layer.layer_id + ":loose:" + original,
                },
                .native_path = it->path(),
                .offset = 0,
            };
            impl->by_path[stored.record.canonical_path].push_back(impl->assets.size());
            impl->assets.push_back(std::move(stored));
        }
        if (walk_error) {
            return core::Result<Vfs>::failure(native_io_error(
                layer.data_root, "cannot read loose layer", std::nullopt, layer.layer_id));
        }

        for (auto archive = layer.active_archives.rbegin(); archive != layer.active_archives.rend(); ++archive) {
            if (archive->source_id.empty()) {
                return core::Result<Vfs>::failure(error(
                    diagnostic_codes::mount_invalid,
                    "each active archive requires a stable source id",
                    std::nullopt,
                    layer.layer_id
                ));
            }
            auto parsed = parse_meg(archive->path, archive->source_id, archive->logical_prefix);
            if (!parsed) return core::Result<Vfs>::failure(parsed.error());
            for (const auto& entry : parsed.value().entries) {
                Impl::StoredAsset stored{
                    .record = AssetRecord{
                        .canonical_path = entry.canonical_path,
                        .original_path = entry.original_path,
                        .layer_id = layer.layer_id,
                        .origin = AssetOrigin::archive,
                        .size = entry.size,
                        .source_id = archive->source_id,
                    },
                    .native_path = archive->path,
                    .offset = entry.offset,
                };
                impl->by_path[stored.record.canonical_path].push_back(impl->assets.size());
                impl->assets.push_back(std::move(stored));
            }
        }
    }
    return core::Result<Vfs>::success(Vfs(std::move(impl)));
}

core::Result<std::vector<AssetRecord>> Vfs::candidates(const std::string_view logical_path) const {
    auto canonical = canonicalize(logical_path);
    if (!canonical) return core::Result<std::vector<AssetRecord>>::failure(canonical.error());
    if (!impl_) return core::Result<std::vector<AssetRecord>>::failure(error(
        diagnostic_codes::mount_invalid, "VFS is not mounted", canonical.value()));
    const auto found = impl_->by_path.find(canonical.value());
    if (found == impl_->by_path.end()) {
        return core::Result<std::vector<AssetRecord>>::failure(error(
            diagnostic_codes::not_found, "asset is not present in the mounted view", canonical.value()));
    }
    std::vector<AssetRecord> result;
    result.reserve(found->second.size());
    for (const auto index : found->second) result.push_back(impl_->assets[index].record);
    return core::Result<std::vector<AssetRecord>>::success(std::move(result));
}

core::Result<AssetRecord> Vfs::stat(const std::string_view logical_path) const {
    auto found = candidates(logical_path);
    if (!found) return core::Result<AssetRecord>::failure(found.error());
    return core::Result<AssetRecord>::success(std::move(found.value().front()));
}

core::Result<std::vector<std::byte>> Vfs::open(const std::string_view logical_path) const {
    auto canonical = canonicalize(logical_path);
    if (!canonical) return core::Result<std::vector<std::byte>>::failure(canonical.error());
    if (!impl_) return core::Result<std::vector<std::byte>>::failure(error(
        diagnostic_codes::mount_invalid, "VFS is not mounted", canonical.value()));
    const auto found = impl_->by_path.find(canonical.value());
    if (found == impl_->by_path.end()) {
        return core::Result<std::vector<std::byte>>::failure(error(
            diagnostic_codes::not_found, "asset is not present in the mounted view", canonical.value()));
    }
    const auto& asset = impl_->assets[found->second.front()];
    if (asset.record.size > max_asset_bytes ||
        asset.record.size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return core::Result<std::vector<std::byte>>::failure(error(
            diagnostic_codes::archive_limit, "asset exceeds the bounded read limit", canonical.value(), asset.record.source_id));
    }
    std::ifstream input(asset.native_path, std::ios::binary);
    if (!input) return core::Result<std::vector<std::byte>>::failure(native_io_error(
        asset.native_path, "cannot read asset source", canonical.value(), asset.record.source_id));
    input.seekg(static_cast<std::streamoff>(asset.offset), std::ios::beg);
    if (!input) return core::Result<std::vector<std::byte>>::failure(error(
        diagnostic_codes::native_io, "cannot seek asset source", canonical.value(), asset.record.source_id));
    std::vector<std::byte> bytes(static_cast<std::size_t>(asset.record.size));
    if (!read_exact(input, bytes.data(), bytes.size())) {
        return core::Result<std::vector<std::byte>>::failure(error(
            diagnostic_codes::native_io, "short read from asset source", canonical.value(), asset.record.source_id));
    }
    return core::Result<std::vector<std::byte>>::success(std::move(bytes));
}

core::Result<std::vector<AssetRecord>> Vfs::enumerate_raw(
    const std::string_view prefix,
    const std::string_view extension
) const {
    if (!impl_) return core::Result<std::vector<AssetRecord>>::failure(error(
        diagnostic_codes::mount_invalid, "VFS is not mounted"));
    std::string canonical_prefix;
    if (!prefix.empty()) {
        auto normalized = canonicalize(prefix);
        if (!normalized) return core::Result<std::vector<AssetRecord>>::failure(normalized.error());
        canonical_prefix = std::move(normalized.value());
    }
    std::vector<AssetRecord> result;
    for (const auto& asset : impl_->assets) {
        if (has_prefix(asset.record.canonical_path, canonical_prefix) &&
            has_extension(asset.record.canonical_path, extension)) result.push_back(asset.record);
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.canonical_path != right.canonical_path) return left.canonical_path < right.canonical_path;
        if (left.layer_id != right.layer_id) return left.layer_id < right.layer_id;
        if (left.origin != right.origin) return left.origin < right.origin;
        return left.source_id < right.source_id;
    });
    return core::Result<std::vector<AssetRecord>>::success(std::move(result));
}

core::Result<std::vector<AssetRecord>> Vfs::enumerate(
    const std::string_view prefix,
    const std::string_view extension
) const {
    auto raw = enumerate_raw(prefix, extension);
    if (!raw) return raw;
    std::string canonical_prefix;
    if (!prefix.empty()) {
        auto normalized = canonicalize(prefix);
        if (!normalized) return core::Result<std::vector<AssetRecord>>::failure(normalized.error());
        canonical_prefix = std::move(normalized.value());
    }
    std::vector<AssetRecord> result;
    std::set<std::string> emitted;
    for (const auto& asset : impl_->assets) {
        if (!has_prefix(asset.record.canonical_path, canonical_prefix) ||
            !has_extension(asset.record.canonical_path, extension)) continue;
        if (emitted.insert(asset.record.canonical_path).second) result.push_back(asset.record);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.canonical_path < right.canonical_path;
    });
    return core::Result<std::vector<AssetRecord>>::success(std::move(result));
}

} // namespace eawr::vfs
