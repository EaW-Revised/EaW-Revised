#pragma once

#include "eawr/presentation/camera/camera.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::camera {

// The caller owns VFS selection and hashing. Paths must be logical, relative
// paths; the loader never records a host filesystem path.
struct XmlSource final {
    std::span<const std::byte> bytes;
    std::string_view logical_path;
    std::string_view sha256;
};

enum class FieldStatus { supplied, absent };

struct FieldProvenance final {
    std::string file;
    std::string source_sha256;
    std::string definition;
    std::string tag;
    FieldStatus status;
};

// One scalar field replaced by a map override. `base` is the XML field's own
// provenance row (supplied or absent), `base_value` the value it resolved to
// before the override, and the remaining fields name the override source.
struct AppliedOverride final {
    std::string tag;
    FieldProvenance base;
    float base_value{};
    float value{};
    std::string file;
    std::string source_sha256;
    std::string source_id;
    std::string authority;
};

struct LoadedConstants final {
    Constants constants;
    // Stable order: camera required scalars, camera optional scalars,
    // Use_Splines and both splines, then global scroll scalars. These rows
    // always describe the XML layer, even for a field a map override replaced.
    std::vector<FieldProvenance> provenance;
    // Empty from load_constants; filled only by apply_map_overrides, in the
    // override document's order.
    std::vector<AppliedOverride> overrides;
};

// Tags and matching camera names fold ASCII case. A supplied empty tag is not
// absent. Scalars use parse_scalar's surrounding space/tab/CR/LF trimming and
// finite whole-token decimal policy. Use_Splines trims the same whitespace and
// accepts only yes/true/1 or no/false/0, case-insensitively. Duplicate known
// tags or duplicate matching camera definitions fail, even if values agree.
// Spline text is parsed only when Use_Splines is true, as in the existing host.
[[nodiscard]] core::Result<LoadedConstants> load_constants(
    XmlSource tactical_cameras, XmlSource game_constants, Mode mode);

// Map constant overrides (P1-09 #30 work item 1). Local project policy, not a
// claim about retail per-map camera data: no original map override format is
// established (#26), so the only accepted authority is "project-authored".
//
// Precedence, lowest to highest:
//   1. effective-VFS XML (`load_constants`: selected definition + global scroll)
//   2. these map overrides, each replacing exactly one named scalar field
//   3. a fixed capture camera, which is not a constants layer at all: it pins
//      the whole frame and ignores the constants (see BoundedTacticalController)
inline constexpr std::string_view invalid_override = "EAWR-CAMERA-0005";
inline constexpr std::string_view project_authored_override = "project-authored";

struct ConstantOverride final {
    // Exact canonical tag spelling (no case folding): one of the scalar tags
    // load_constants reads. Use_Splines, the splines and the camera-lock tags
    // are refused.
    std::string tag;
    // Parsed with parse_scalar's finite whole-token decimal policy.
    std::string value;
};

struct OverrideSource final {
    // Relative logical name of the override document; never a host path.
    std::string_view logical_path;
    // Lowercase 64-digit SHA-256 of the override document.
    std::string_view sha256;
    std::string_view source_id;
    std::string_view authority;
};

// All-or-nothing. Rejects (EAWR-CAMERA-0005) an unsupported authority, a bad
// source identity, an empty override list, an unknown, refused or duplicate
// tag, a malformed value, a negative Tactical_Min/Max_Scroll_Speed override,
// and an overridden scroll-speed pair whose layered result is inverted. The
// two scroll-speed rules are project override policy; the XML layer keeps only
// `validate`'s domain. Any other result that `validate` refuses returns that
// validator's own diagnostic. On success the returned constants carry every
// override and `overrides` records the replaced value with both provenances;
// `base` is never modified.
//
// This checks constants only. Whether a controller can be built from them
// also depends on the map's bounds, initial zoom, yaw and viewport, which this
// function does not see (for example Yaw_Min above the initial yaw, or a
// resolved field of view outside (0, 180) degrees); the viewer's resolve_map_constants
// dry-runs BoundedTacticalController::create for that.
[[nodiscard]] core::Result<LoadedConstants> apply_map_overrides(
    const LoadedConstants& base, OverrideSource source,
    std::span<const ConstantOverride> overrides);

} // namespace eawr::presentation::camera
