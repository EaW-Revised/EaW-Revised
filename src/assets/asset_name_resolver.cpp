#include "eawr/assets/asset_name_resolver.hpp"
#include "eawr/assets/model_listing.hpp"

#include "eawr/core/degradation_ledger.hpp"
#include "eawr/vfs/vfs.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <utility>

namespace eawr::assets {
namespace {

struct Registry {
    std::mutex mutex;
    std::set<std::string> seen;
    std::vector<std::string> lines;
    std::size_t suppressed{0};
    bool cap_reported{false};
};

Registry& registry() {
    static Registry instance;
    return instance;
}

std::atomic<NameMissEmitter>& emitter_slot() {
    static std::atomic<NameMissEmitter> slot{nullptr};
    return slot;
}

void default_emit(const std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
}

// One line must stay one line whatever the authored text holds.
std::string printable(const std::string_view text) {
    constexpr std::size_t limit = 200;
    std::string result;
    result.reserve(text.size() < limit ? text.size() : limit);
    for (const char c : text) {
        if (result.size() >= limit) {
            result += "...";
            break;
        }
        result.push_back(static_cast<unsigned char>(c) < 0x20 || c == 0x7f ? '?' : c);
    }
    return result;
}

void emit(const std::string_view line) {
    const NameMissEmitter custom = emitter_slot().load();
    if (custom != nullptr) custom(line);
    else default_emit(line);
}

} // namespace

std::string_view to_string(const NameKind kind) noexcept {
    return kind == NameKind::texture ? "texture" : "model";
}

bool report_name_miss(const NameKind kind, const std::string_view authored, const std::string_view site,
                      const std::string_view source) {
    const std::string folded = normalized_asset_name(authored);
    if (folded.empty()) return false;
    // Observation only: the ledger never answers back and the result below does not depend on it.
    core::degradation::record(asset_name_miss_code, source, std::string(to_string(kind)) + " " + folded,
                              "not found (site " + printable(site) + ")");
    std::string line;
    {
        // The line is recorded and emitted under one lock, so the emitted order is the recorded order and a custom
        // emitter never runs concurrently with itself.
        Registry& state = registry();
        const std::lock_guard lock(state.mutex);
        if (state.seen.size() >= name_miss_cap) {
            ++state.suppressed;
            if (state.cap_reported) return false;
            state.cap_reported = true;
            line = std::string(asset_name_miss_code) + " further missing asset names are not reported (limit "
                + std::to_string(name_miss_cap) + ")";
        } else {
            if (!state.seen.insert(std::string(to_string(kind)) + ":" + folded).second) return false;
            line = std::string(asset_name_miss_code) + " " + std::string(to_string(kind)) + " '" + printable(authored)
                + "' not found (site " + printable(site) + ")";
        }
        state.lines.push_back(line);
        emit(line);
    }
    return true;
}

void set_name_miss_emitter(const NameMissEmitter emitter) noexcept { emitter_slot().store(emitter); }

std::vector<std::string> name_miss_lines() {
    Registry& state = registry();
    const std::lock_guard lock(state.mutex);
    return state.lines;
}

void reset_name_misses() {
    Registry& state = registry();
    const std::lock_guard lock(state.mutex);
    state.seen.clear();
    state.lines.clear();
    state.suppressed = 0;
    state.cap_reported = false;
}

std::string resolve_texture(const vfs::Vfs& filesystem, const std::string_view name, const std::string_view site,
                            const std::string_view root) {
    return resolve_texture_reporting(root, name, site,
        [&filesystem](const std::string_view path) { return static_cast<bool>(filesystem.stat(path)); });
}

std::string probe_texture(const vfs::Vfs& filesystem, const std::string_view name, const std::string_view root) {
    return resolve_texture_name(effective_root(root, name), name,
        [&filesystem](const std::string_view path) { return static_cast<bool>(filesystem.stat(path)); });
}

std::string resolve_model(const vfs::Vfs& filesystem, const std::string_view name, const std::string_view site,
                          const std::string_view root) {
    return resolve_model_reporting(root, name, site,
        [&filesystem](const std::string_view path) { return static_cast<bool>(filesystem.stat(path)); });
}

} // namespace eawr::assets

namespace eawr::assets {

core::Result<std::vector<vfs::AssetRecord>> list_models(const vfs::Vfs& filesystem) {
    return filesystem.enumerate("data/art/models", ".alo");
}

} // namespace eawr::assets
