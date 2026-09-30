#pragma once

#include "space_fog.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/space/space.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/snapshot.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// The synthetic units of an opt-in space fog run (P1-07 #28). The static scene
// builder reads the space map (Space_Model_Name chain); space_fog::classify
// admits only caller-declared XML element types; every admitted surface is
// uploaded through its legacy selector and declared a fog consumer. Anything
// an admitted placement cannot compose whole fails the run: there is no stand-
// in texture, no partial unit and no inferred classification. The sky is not
// involved and is never a fog consumer.
class SpaceFogUnits final {
public:
    // Renderer asset and entity ids start here, clear of the sky surfaces and
    // the foreground controls.
    static constexpr sim::AssetId first_asset = 2001;
    static constexpr sim::EntityId first_entity = 2001;

    struct Unit final {
        std::uint64_t scene_ordinal{};
        std::string object_id;
        std::string type_name;
        std::vector<sim::AssetId> assets;      // one per uploaded surface
        std::vector<sim::EntityId> entities;   // one instance per surface
        // Posed and placed render-basis triangles of every surface, for masks.
        std::vector<space::Triangle> triangles;
        // Source-basis XY footprint of the same vertices.
        space_fog::SourceBounds bounds;
    };

    // False when an admitted placement cannot be composed, nothing is
    // admitted, or an upload, declaration or pose fails; failure() says why,
    // unsupported() names every failing surface, every uploaded asset has been
    // released again and units() is empty.
    [[nodiscard]] bool compose(GodotRenderer& renderer, const assets::Map& map, const std::string& map_sha256,
                               const vfs::Vfs& filesystem, const data::Catalog& catalog,
                               std::span<const std::string> admitted_types);
    void release(GodotRenderer& renderer);

    [[nodiscard]] const std::optional<scene::Scene>& scene() const noexcept { return scene_; }
    [[nodiscard]] const std::vector<space_fog::PlacementDecision>& decisions() const noexcept { return decisions_; }
    [[nodiscard]] const std::vector<Unit>& units() const noexcept { return units_; }
    [[nodiscard]] const std::vector<sim::RenderInstance>& instances() const noexcept { return instances_; }
    [[nodiscard]] const std::vector<std::string>& unsupported() const noexcept { return unsupported_; }
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] std::size_t live_assets() const noexcept { return uploaded_.size(); }

private:
    std::optional<scene::Scene> scene_;
    std::vector<space_fog::PlacementDecision> decisions_;
    std::vector<Unit> units_;
    std::vector<sim::RenderInstance> instances_;
    std::vector<sim::AssetId> uploaded_;
    std::vector<std::string> unsupported_;
    std::string failure_;
};

} // namespace eawr::presentation::godot_backend
