#include "eawr/mods/fingerprint.hpp"
#include "eawr/mods/profile.hpp"

#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace eawr::mods {

namespace fs = steam::fs;

std::string loadcheck_chain_fingerprint(const Profile& profile, const fs::path& game, std::string_view engine) {
    std::string material(engine);
    const auto append = [&](const fs::path& root) {
        const auto tree = tree_fingerprint(root);
        if (tree.capped || tree.incomplete || tree.tree.empty()) return false;
        material += '\n' + steam::path_to_utf8(root) + '\n' + tree.tree;
        return true;
    };
    for (const auto& layer : profile.layers) {
        material += '\n' + layer.key;
        if (!append(steam::path_from_utf8(layer.root))) return {};
    }
    if (!append(game / "corruption/Data") || !append(game / "GameData/Data")) return {};
    return core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(material.data()), material.size()));
}

TreeFingerprint tree_fingerprint(const fs::path& root, std::uint64_t entry_cap) {
    TreeFingerprint result;
    std::error_code error;
    if (!fs::is_directory(root, error)) return result;

    // (relative path, "\t<size>\t<mtime>\n"), sorted by path before hashing.
    std::vector<std::pair<std::string, std::string>> files;
    std::vector<std::pair<fs::path, std::string>> pending{{root, std::string()}};
    std::uint64_t visited = 0;
    while (!pending.empty() && !result.capped) {
        auto [directory, prefix] = std::move(pending.back());
        pending.pop_back();
        std::error_code list_error;
        fs::directory_iterator iterator(directory, fs::directory_options::skip_permission_denied, list_error);
        for (fs::directory_iterator end; !list_error && iterator != end; iterator.increment(list_error)) {
            if (visited++ >= entry_cap) {
                result.capped = true;
                break;
            }
            const fs::directory_entry& entry = *iterator;
            const std::string relative = prefix + steam::path_to_utf8(entry.path().filename());
            std::error_code status_error;
            // symlink_status: links and junctions are neither walked into nor hashed.
            const fs::file_type type = entry.symlink_status(status_error).type();
            if (status_error) {
                result.incomplete = true;
                continue;
            }
            if (type == fs::file_type::directory) {
                pending.emplace_back(entry.path(), relative + "/");
            } else if (type == fs::file_type::regular) {
                std::error_code size_error;
                std::error_code time_error;
                const std::uintmax_t size = entry.file_size(size_error);
                const fs::file_time_type time = entry.last_write_time(time_error);
                if (size_error || time_error) {
                    result.incomplete = true;
                    continue;
                }
                files.emplace_back(relative, "\t" + std::to_string(size) + "\t" + std::to_string(time.time_since_epoch().count()) + "\n");
            }
        }
        if (list_error) result.incomplete = true;
    }

    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    std::string text;
    for (const auto& [path, tail] : files) {
        text += path;
        text += tail;
    }
    result.files = files.size();
    result.tree = core::sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    return result;
}

} // namespace eawr::mods
