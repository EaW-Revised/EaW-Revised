#pragma once

#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::script::authoritative {

// ML-31/ML-38: the Library and Miscellaneous script roots a scoring instance searches. The scoring
// builders use this one list both to pin their manifest and to configure the scheduler, so what
// `require` may resolve can never be narrower than what the scheduler searches.
[[nodiscard]] std::vector<std::string> scoring_script_directories();

// Pins every `.lua` the mounted view holds under each directory (lower-case logical prefix ending
// in '/'), in the effective layering, into `manifest`. The byte set is fixed up front, so the
// manifest digest stays deterministic; there is no fixed module list and no per-mod name.
[[nodiscard]] core::Result<void> pin_script_directories(
    const vfs::Vfs& files, std::span<const std::string> directories, ModuleManifest& manifest);

// The same enumeration for hosts that hand a module map (logical path -> bytes) to their loaders: every `.lua`
// under each directory, keyed by canonical path. A path that two directories both reach is opened once per
// directory and the first read is kept (`pin_script_directories` instead fails on the duplicate module). Whoever
// builds a manifest from such a map must pass the directories its scheduler is configured with, so what
// `require` may resolve is never narrower than what the scheduler searches (ML-31, ML-33, ML-38, L-03a).
[[nodiscard]] core::Result<std::map<std::string, std::string>> read_script_directories(
    const vfs::Vfs& files, std::span<const std::string> directories);

} // namespace eawr::script::authoritative
