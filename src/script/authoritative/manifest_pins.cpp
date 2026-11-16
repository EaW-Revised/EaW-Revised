#include "eawr/script/authoritative/manifest_pins.hpp"

#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <utility>

namespace eawr::script::authoritative {

std::vector<std::string> scoring_script_directories() {
    return {"data/scripts/library/", "data/scripts/miscellaneous/"};
}

namespace {

// One pass over the mounted `.lua` files of `directories`; `take` receives each canonical path with its bytes.
template <class Take>
core::Result<void> visit_script_directories(const vfs::Vfs& files, const std::span<const std::string> directories, Take&& take) {
    using Void = core::Result<void>;
    for (const std::string& directory : directories) {
        auto assets = files.enumerate(directory, ".lua");
        if (!assets) return Void::failure(assets.error());
        for (const auto& asset : assets.value()) {
            const auto bytes = files.open(asset.canonical_path);
            if (!bytes) {
                // A pinned file must be readable: skipping one could make two peers' manifests differ.
                auto diagnostic = bytes.error();
                diagnostic.message = "script " + asset.canonical_path + " cannot be read: " + diagnostic.message;
                return Void::failure(std::move(diagnostic));
            }
            std::string text(bytes.value().size(), '\0');
            std::transform(bytes.value().begin(), bytes.value().end(), text.begin(),
                [](const std::byte value) { return static_cast<char>(value); });
            if (auto taken = take(asset.canonical_path, std::move(text)); !taken) return taken;
        }
    }
    return Void::success();
}

} // namespace

core::Result<void> pin_script_directories(
    const vfs::Vfs& files, const std::span<const std::string> directories, ModuleManifest& manifest) {
    return visit_script_directories(files, directories,
        [&](const std::string& path, std::string text) { return manifest.add(path, std::move(text)); });
}

core::Result<std::map<std::string, std::string>> read_script_directories(
    const vfs::Vfs& files, const std::span<const std::string> directories) {
    using Modules = core::Result<std::map<std::string, std::string>>;
    std::map<std::string, std::string> modules;
    auto read = visit_script_directories(files, directories, [&](const std::string& path, std::string text) {
        modules.try_emplace(path, std::move(text));
        return core::Result<void>::success();
    });
    if (!read) return Modules::failure(read.error());
    return Modules::success(std::move(modules));
}

} // namespace eawr::script::authoritative
