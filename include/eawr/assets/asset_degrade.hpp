#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/load_note.hpp"

#include <optional>
#include <string_view>

// A model or animation the file system names but the loaders refuse (an empty file, a file with no root group, a clip that
// does not bind to its model) must never abort a campaign or battle start (behaviour note ASD-01). The caller keeps the
// object, draws it without that asset, and publishes the one load note this header builds. The parsers keep refusing the
// file; only the callers' reaction changes. The note is a class C note (retail's handling of a damaged file is recorded
// in docs/behaviour/asset-degrade.md), so the degradation ledger counts it under the mod that supplied the file.
namespace eawr::assets {

inline constexpr std::string_view asset_degraded_code = "EAWR-LOAD-1239";

enum class DegradedAsset { animation, model };

// What was tried: loading the file, or binding a loaded clip to its model.
enum class DegradedStage { load, bind };

// The note for one refused asset. `path` is the logical path the caller asked for; the layer and the file name come from
// the file system's record of it (when the file system has none, the note names `path` under the stock layer).
[[nodiscard]] core::LoadNote asset_degraded_note(const vfs::Vfs& filesystem, std::string_view path, DegradedAsset kind,
                                                 DegradedStage stage, const core::Diagnostic& why);

struct TolerantAnimation final {
    std::optional<Animation> clip;      // empty when the file could not be used
    std::optional<core::LoadNote> note; // set exactly when `clip` is empty
};

// `load_animation`, with a refusal turned into the note instead of an error. A missing file is a refusal too; callers
// that treat "no clip file" as normal test `filesystem.stat(path)` first, as before.
[[nodiscard]] TolerantAnimation load_animation_or_note(const vfs::Vfs& filesystem, std::string_view path);

} // namespace eawr::assets
