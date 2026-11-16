#pragma once

#include "eawr/assets/asset_names.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// The one place an authored texture or model name becomes a logical path (docs/behaviour/asset-name-resolution.md,
// rules ANR-01 and ANR-02). A call site hands over the authored name and its own site label; it gets the logical path
// or an empty string, and a miss is reported once per (kind, name) as one `EAWR-ASSET-NAME-0001` line.
// tools/ci/asset_name_guard.py fails when new code builds such a path itself.
namespace eawr::assets {

enum class NameKind { texture, model };

[[nodiscard]] std::string_view to_string(NameKind kind) noexcept;

inline constexpr std::string_view asset_name_miss_code = "EAWR-ASSET-NAME-0001";
inline constexpr std::string_view texture_root = "data/art/textures/";
inline constexpr std::string_view model_root = "data/art/models/";

// Reports a miss once per (kind, case/slash-folded name) for the process. Returns true when this call emitted the
// line. Thread safe. A blank name is not a miss. After `name_miss_cap` distinct names one "suppressed" line follows
// and later names are counted only.
// Every call (not only the first per name) also goes to the degradation ledger (core/degradation_ledger.hpp) as code
// `EAWR-ASSET-NAME-0001`, object "<kind> <folded name>", so the ledger counts all names even past the cap. `source` is
// the mod id or layer the name belongs to; empty means "unknown", which the ledger reports as a count of its own.
inline constexpr std::size_t name_miss_cap = 4096;
bool report_name_miss(NameKind kind, std::string_view authored, std::string_view site, std::string_view source = {});

// Where the lines go. The default writes each line to the standard error stream; the viewer may install its own log.
// An emitter is called one line at a time, in recorded order, with the reporter's lock held: it must be quick and
// must not call back into this header. nullptr restores the default.
using NameMissEmitter = void (*)(std::string_view line);
void set_name_miss_emitter(NameMissEmitter emitter) noexcept;

// Every line emitted so far, in order (capped), for load notes and tests.
[[nodiscard]] std::vector<std::string> name_miss_lines();
// Forgets the reported names and lines. For tests.
void reset_name_misses();

// A VFS-backed lookup: `root` is the search root (default: the kind's), dropped when the authored name already starts
// with `data/`. Use these from sites that hold a Vfs.
[[nodiscard]] std::string resolve_texture(const vfs::Vfs& filesystem, std::string_view name, std::string_view site,
                                          std::string_view root = texture_root);
[[nodiscard]] std::string resolve_model(const vfs::Vfs& filesystem, std::string_view name, std::string_view site,
                                        std::string_view root = model_root);

// The same lookup without the miss report, for a probe whose miss is expected and is not a missing authored name
// (the end of a numbered frame sequence). Do not use it for an authored name a person would want reported.
[[nodiscard]] std::string probe_texture(const vfs::Vfs& filesystem, std::string_view name,
                                        std::string_view root = texture_root);

// The same rules over an existence functor (a scene cache, a test set) with the same once-per-name miss report.
// `exists` takes a logical path. Same `data/` prefix handling.
[[nodiscard]] inline std::string_view effective_root(const std::string_view root, const std::string_view name) {
    return normalized_asset_name(name).starts_with("data/") ? std::string_view{} : root;
}

template <typename Exists>
[[nodiscard]] std::string resolve_texture_reporting(const std::string_view root, const std::string_view name,
                                                    const std::string_view site, Exists&& exists) {
    std::string path = resolve_texture_name(effective_root(root, name), name, exists);
    if (path.empty()) report_name_miss(NameKind::texture, name, site);
    return path;
}

template <typename Exists>
[[nodiscard]] std::string resolve_model_reporting(const std::string_view root, const std::string_view name,
                                                  const std::string_view site, Exists&& exists) {
    std::string path = resolve_model_name(effective_root(root, name), name, exists);
    if (path.empty()) report_name_miss(NameKind::model, name, site);
    return path;
}

} // namespace eawr::assets
