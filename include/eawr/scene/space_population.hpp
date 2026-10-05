#pragma once

#include "eawr/data/xml.hpp"
#include "eawr/scene/idle_tags.hpp"
#include "eawr/scene/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Space-map populate policy (P1-11 #32, space half). Engine-free: it reads a
// built static scene and what the XML catalog says about each placed object,
// and decides which placements a populated space view draws. The rule is
// deliberately plain: every placement with a renderable model is drawn,
// except catalog-flagged markers (never visible in game) and nebula or
// background objects, which the space environment composition owns.
namespace eawr::scene {

// MD-06: a mesh-free projectile model may draw entirely through resolved particle proxies.
[[nodiscard]] bool drawable_projectile_effects(const Placement& placement) noexcept;

// One entry of an object's HardPoints list, as the hardpoint's own XML object
// declares it. Its art is selected by its state (hardpoint_art below):
// Model_To_Attach is drawn at the bind frame of the owner's Attachment_Bone,
// the owner's Damage_Decal mesh and the owner's particle proxies at or below
// the Damage_Particles bone are authored visible but belong to a destroyed
// hardpoint (docs/asset-formats.md "Hardpoint state art").
struct HardpointAttachment final {
    std::string hardpoint;         // hardpoint object id
    bool resolved{};               // the id names a catalog object
    std::string model;             // Model_To_Attach, empty when none
    std::string bone;              // Attachment_Bone, empty when none
    std::string damage_decal;      // Damage_Decal mesh name, empty when none
    std::string damage_particles;  // Damage_Particles bone name, empty when none
    std::string engine_particles;  // Engine_Particles bone name, empty when none
    bool hide_engine_particles_on_death{};  // Engine_Death_Hide_Engine_Particles
};

// A hardpoint's presentation state. A spawned object has every hardpoint
// intact; #72 decides when one is damaged or destroyed.
enum class HardpointState : std::uint8_t { intact, damaged, destroyed };
[[nodiscard]] std::string_view to_string(HardpointState state) noexcept;
[[nodiscard]] std::optional<HardpointState> parse_hardpoint_state(std::string_view text) noexcept;

// What a hardpoint in `state` shows of the art its XML entry names.
//   intact:    Model_To_Attach
//   damaged:   Model_To_Attach (identical to intact)
//   destroyed: the Damage_Decal mesh and the Damage_Particles emitters
struct HardpointArt final {
    bool attached_model{};
    bool damage_decal{};
    bool damage_particles{};
};
[[nodiscard]] HardpointArt hardpoint_art(HardpointState state) noexcept;

// Which hardpoint owns each mesh and proxy of the owner's model, by the
// names the hardpoints declare (ASCII case-insensitive): a mesh named as a
// Damage_Decal, a proxy on the Damage_Particles bone or a bone below it.
// When two hardpoints name the same art the first in HardPoints order owns it.
struct HardpointOwnerArt final {
    static constexpr std::size_t none = static_cast<std::size_t>(-1);
    std::vector<std::size_t> mesh_hardpoint;   // per owner mesh, `none` when no hardpoint owns it
    std::vector<std::size_t> proxy_hardpoint;  // per owner proxy
    std::vector<std::size_t> engine_mesh_hardpoint;  // engine sub-objects hidden at destruction
    std::vector<std::size_t> engine_proxy_hardpoint;
};
[[nodiscard]] HardpointOwnerArt hardpoint_owner_art(const assets::Model& owner,
                                                    std::span<const HardpointAttachment> hardpoints);

// Per owner proxy: 1 when the state of the hardpoint that owns it hides it.
// `states` is aligned with `hardpoints`; a missing entry is intact.
[[nodiscard]] std::vector<std::uint8_t> hidden_hardpoint_proxies(
    const assets::Model& owner, std::span<const HardpointAttachment> hardpoints,
    std::span<const HardpointState> states);

// Catalog facts about one placed object type.
struct SpaceObjectTags final {
    std::string type_name;  // XML element of the catalog winner, e.g. SpaceProp
    bool marker{};          // Is_Marker = yes, or the Marker element
    bool nebula{};          // Is_Nebula = yes
    bool background{};      // In_Background = yes
    IdleTags idle{};        // idle_tags(object, MapKind::space) (#145)
    std::vector<HardpointAttachment> hardpoints{};
};

using ObjectResolver = std::function<std::optional<data::EffectiveObject>(std::string_view object_id, data::Category category)>;

// Reads the tags above from a resolved object. XML booleans are accepted as
// yes/true/1 in any case, with surrounding whitespace. HardPoints is a comma
// or whitespace separated id list; each id is resolved through `resolve`
// (when given) in the hardpoint registry for its attachment fields.
[[nodiscard]] SpaceObjectTags space_object_tags(const data::EffectiveObject& object,
                                                const ObjectResolver& resolve = {});

enum class SpaceRole : std::uint8_t {
    drawn,         // at least one visible surface has a supported material
    environment,   // nebula or background object: composed by the space environment
    marker,        // editor/skirmish marker: not drawn in game
    not_drawable,  // nothing drawable; `missing` says why
};
[[nodiscard]] std::string_view to_string(SpaceRole role) noexcept;

// MeshShadowVolume.fx surfaces are stencil-shadow geometry, never drawn in
// colour; the renderer's shadow map stands in for them.
[[nodiscard]] bool is_shadow_volume_shader(std::string_view shader) noexcept;

struct SpacePlacementDecision final {
    std::uint64_t scene_ordinal{};
    std::string object_id;
    std::string type_name;  // empty when the object has no catalog winner
    SpaceRole role{SpaceRole::not_drawable};
    // Indices into Placement::surfaces that are drawn (role drawn only).
    std::vector<std::size_t> drawn_surfaces;
    std::size_t shadow_volume_surfaces{};
    // Supported surfaces of a hardpoint's Damage_Decal mesh, hidden while the
    // hardpoint is intact (role drawn only).
    std::size_t damage_decal_surfaces{};
    // Those surfaces with the index (into `hardpoints`) of the hardpoint
    // whose destruction shows them.
    struct DecalSurface final {
        std::size_t surface{};
        std::size_t hardpoint{};
    };
    std::vector<DecalSurface> damage_decals;
    // The object's hardpoints (role drawn only), for the attached models.
    std::vector<HardpointAttachment> hardpoints;
    // What is not drawn and why, as "<cause> <detail>": for a drawn placement
    // the surfaces, textures and effects left out; for not_drawable the
    // causes that block it. Stencil-shadow surfaces are counted, not listed.
    std::vector<std::string> missing;
};

// One decision per scene placement, in scene order. `tags_of` returns the
// catalog facts of an object id, or nullopt when it has none.
[[nodiscard]] std::vector<SpacePlacementDecision> classify_space_placements(
    const Scene& scene,
    const std::function<std::optional<SpaceObjectTags>(std::string_view object_id)>& tags_of);

} // namespace eawr::scene
