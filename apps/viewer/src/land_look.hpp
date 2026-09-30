#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/core/result.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/terrain/terrain.hpp"
#include "eawr/sim/snapshot.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The land map's look (P1-06 #27): blended terrain layers, a camera-centred
// skydome with its sun, approximate water, and the default tactical framing.
// Everything here is a clean-room modern_spatial adapter: it reads the TED and
// ALO data through the engine-free terrain module and draws through the
// GodotRenderer, never through a translated TERRAIN, SKYDOME or WATER effect.
namespace eawr::presentation::godot_backend::land_look {

// Decoded texture for a declared name, or nothing when it does not resolve.
using TextureLookup = std::function<std::optional<assets::Texture>(std::string_view declared)>;

// ---- terrain -----------------------------------------------------------------

inline constexpr std::string_view terrain_adapter_id = "eawr-terrain-blend-v1";

// The terrain surface shader (unshaded) or its SH-lit, shadow-receiving
// variant. Both declare `void vertex() {` and a single `    ALBEDO = ...;`
// statement, which is what the fog variant builder edits.
[[nodiscard]] std::string terrain_shader(bool lit);

struct TerrainBlendReport final {
    std::size_t layers{};
    std::uint32_t layer_edge{};
    std::size_t unresolved_layers{};
};

// Registers the shared layer array, the per-sample layer index and the
// per-layer mapping table on the renderer, and returns the bindings every
// terrain surface material carries.
[[nodiscard]] core::Result<std::vector<MaterialBinding>> register_terrain_blend(
    GodotRenderer& renderer, const terrain::BlendMap& blend,
    const std::vector<assets::TerrainMaterial>& materials, const TextureLookup& lookup,
    TerrainBlendReport& report);

// ---- sky -----------------------------------------------------------------------

struct SkyInputs final {
    // The dome is drawn around the eye at this fraction of the active
    // camera's far distance, so terrain nearer than that draws in front.
    float dome_fraction{0.9F};
    // Environment light 0, toward the light, source basis. Without it the
    // sun billboard is not drawn.
    std::optional<assets::Vec3f> toward_sun;
};

struct SkyComposition final {
    std::size_t dome_surfaces{};
    std::size_t sun_surfaces{};
    std::size_t skipped_surfaces{};
    std::vector<std::string> skipped;
};

// Uploads each Skydome.fx surface as a camera-centred dome with its own base
// and cloud textures, and each MeshAdditive surface on a mode-7 sun bone as a
// camera-facing additive billboard placed toward the sun. Other surfaces are
// skipped and named.
[[nodiscard]] core::Result<SkyComposition> compose_sky(
    GodotRenderer& renderer, const assets::Model& model, const TextureLookup& lookup,
    const SkyInputs& inputs, sim::AssetId& next_asset, sim::EntityId& next_entity,
    std::vector<sim::RenderInstance>& instances);

// R-LIT-01 of docs/behaviour/p1-effective-environment.md: light 0 heading
// (0x08) and elevation (0x0b), radians, toward the light.
[[nodiscard]] std::optional<assets::Vec3f> toward_sun(const assets::EnvironmentDescriptor& environment);

// ---- water ----------------------------------------------------------------------

struct WaterComposition final {
    std::string status{"absent"};
    std::string cause;
    std::optional<terrain::WaterPlane> plane;
    bool plane_drawn{};
    std::size_t rivers_drawn{};
    std::size_t water_decoration_tracks_drawn{};
    std::size_t terrain_tracks_drawn{};
    std::uint32_t rivers_skipped{};
};

// Apply the same map-fog sample and RGB attenuation to terrain and water shaders.
[[nodiscard]] std::string with_map_fog(std::string shader);

// Approximate water: the map-wide plane and authored terrain tracks, submitted
// in track then water-decoration order using mini 0x10 for classification.
[[nodiscard]] core::Result<WaterComposition> compose_water(
    GodotRenderer& renderer, const assets::Map& map, const terrain::Mesh& mesh,
    const TextureLookup& lookup, sim::AssetId& next_asset, sim::EntityId& next_entity,
    std::vector<sim::RenderInstance>& instances, float flow_time, bool flow_animate, bool fog);

// ---- camera ---------------------------------------------------------------------

struct TacticalRequest final {
    std::optional<float> zoom;
    std::optional<float> yaw_degrees;
    // Source X/Y of the camera target.
    std::optional<std::array<float, 2>> target;
};

struct TacticalDefault final {
    FixedCamera camera;
    float zoom{};
    float yaw_degrees{};
    float distance{};
    float pitch_degrees{};
    std::array<float, 3> target_source{};
    std::string target_origin;
    std::string tactical_xml_sha256;
};

// The Land_Mode tactical camera from the effective-VFS XML constants, at the
// default distance and yaw, aimed at the first Player_0 spawn marker (the
// skirmish start), else the terrain centre, at the terrain height there.
[[nodiscard]] core::Result<TacticalDefault> tactical_default(
    const vfs::Vfs& filesystem, const assets::Map& map, const terrain::Mesh& mesh,
    std::uint32_t width, std::uint32_t height, const TacticalRequest& request);

} // namespace eawr::presentation::godot_backend::land_look
