#include "eawr/presentation/particles/map_effect_plan.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace eawr;
using namespace eawr::presentation::particles;
using sim::math::Fixed;
using sim::math::Mat3x4;

void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

Fixed integer(int value) { return Fixed::from_raw(static_cast<std::int64_t>(value) * Fixed::scale); }
Mat3x4 translated(int x, int y, int z) {
    auto matrix = sim::math::identity_matrix();
    matrix.rows[0][3] = integer(x);
    matrix.rows[1][3] = integer(y);
    matrix.rows[2][3] = integer(z);
    return matrix;
}

struct Fixture {
    assets::Model model;
    scene::Placement placement;
    std::vector<std::optional<Mat3x4>> frames;
    std::vector<EffectEvidence> effects;

    Fixture(std::initializer_list<std::string> names) {
        model.source.logical_path = "data/art/models/host.alo";
        model.source.source_id = "archive:unit-test";
        model.bones.push_back(assets::Bone{});
        frames.push_back(translated(2, 3, 4));
        placement.map_logical_path = "data/art/maps/test.ted";
        placement.scene_ordinal = 17;
        placement.record_ordinal = 23;
        placement.model_path = model.source.logical_path;
        placement.model_provenance = {"Model_Name", "HostObject", "data/xml/objects.xml", 42};
        scene::Transform transform;
        transform.matrix = translated(10, 20, 30);
        placement.transform = transform;
        for (const auto& name : names) {
            model.proxies.push_back({name, 0, true, false});
            placement.effects.push_back({name, "data/art/models/" + name + ".alo", 0, false});
            effects.push_back({EffectKind::particle, 3});
        }
    }

    std::vector<std::uint8_t> hardpoint_hidden;
    std::vector<std::uint8_t> code_shown;

    MapEffectPlan run(std::size_t budget = 100, std::uint64_t seed = 99,
                      std::optional<std::uint32_t> alt = 0,
                      std::optional<std::uint32_t> lod = 0,
                      VisibilityEvidence visibility = VisibilityEvidence::bind_pose) {
        const MapEffectPlacementInput input{&model, &placement, frames, effects, visibility, alt, lod,
                                            hardpoint_hidden, code_shown};
        return plan_map_effects(std::span(&input, 1), seed, budget);
    }
};

void variants() {
    Fixture f{"Smoke_ALT0_LOD0", "Smoke_ALT1_LOD0", "Smoke_ALT0_LOD1", "Smoke"};
    auto p = f.run();
    expect(p.records.size() == 4 && p.allocated_capacity == 6, "source order and mutually exclusive variants");
    expect(p.records[0].status == MapEffectStatus::admitted && p.records[1].cause == MapEffectCause::alt_mismatch
        && p.records[2].cause == MapEffectCause::lod_mismatch && p.records[3].status == MapEffectStatus::admitted,
        "explicit ALT/LOD selection");
    expect(f.run(100, 99, std::nullopt).records[0].cause == MapEffectCause::missing_alt_selection,
        "ALT cannot be inferred from a resolved filename");
    expect(f.run(100, 99, 0, std::nullopt).records[0].cause == MapEffectCause::missing_lod_selection,
        "LOD cannot be inferred");
    f.model.proxies[0].name = "Smoke_ALTx";
    expect(f.run().records[0].cause == MapEffectCause::reference_mismatch, "reference mismatch explicit");
    f.placement.effects[0].proxy_name = "Smoke_ALTx";
    expect(f.run().records[0].cause == MapEffectCause::malformed_variant_tag, "malformed ALT explicit");
}

void visibility_and_resolution() {
    Fixture f{"Smoke", "Smoke", "Smoke", "Smoke"};
    f.model.proxies[0].visible = false;
    f.model.proxies[1].bone = 1;
    f.placement.effects[1].bone = 1;
    f.model.bones.push_back(assets::Bone{});
    f.model.bones[1].visible = false;
    f.placement.effects[2].resolved.clear();
    f.effects[3].kind = EffectKind::non_particle;
    auto p = f.run();
    expect(p.records[0].cause == MapEffectCause::hidden_proxy && p.records[1].cause == MapEffectCause::hidden_bone,
        "initial visibility decisions");
    expect(p.records[2].status == MapEffectStatus::unresolved && p.records[2].cause == MapEffectCause::unresolved_reference,
        "unresolved effect is retained");
    expect(p.records[3].cause == MapEffectCause::non_particle_reference, "nonparticle path is unsupported");
    expect(f.run(100, 99, 0, 0, VisibilityEvidence::unknown).records[0].cause == MapEffectCause::unknown_visibility,
        "unknown dynamic visibility is unsupported");
    f.effects[3].kind = EffectKind::unknown;
    expect(f.run().records[3].cause == MapEffectCause::unknown_effect_kind, "unknown classification unresolved");
}

// #136: a damage emitter of an intact hardpoint is hidden, neither admitted
// nor charged to the budget; an authored-hidden proxy keeps its own cause.
void hardpoint_states() {
    Fixture f{"Damage", "Damage", "Engine"};
    f.hardpoint_hidden = {1, 1};
    f.model.proxies[1].visible = false;
    auto p = f.run();
    expect(p.records.size() == 3 && p.records[0].status == MapEffectStatus::hidden
        && p.records[0].cause == MapEffectCause::hardpoint_state, "intact hardpoint hides its emitter");
    expect(p.records[1].cause == MapEffectCause::hidden_proxy, "authored hidden proxy keeps its cause");
    expect(p.records[2].status == MapEffectStatus::admitted && p.allocated_capacity == 3,
        "a proxy past the mask is admitted; hidden emitters take no capacity");
    f.hardpoint_hidden = {0, 0, 0};
    expect(f.run().records[0].status == MapEffectStatus::admitted, "a destroyed hardpoint's emitter is admitted");
}

// IS-09: a proxy whose type the code shows (an ion stun's) runs although authored hidden; a
// hidden bone and a hardpoint's state still hide it.
void code_shown_proxies() {
    Fixture f{"pi_Elec", "pi_Elec", "pi_Elec"};
    for (auto& proxy : f.model.proxies) proxy.visible = false;
    expect(f.run().records[0].cause == MapEffectCause::hidden_proxy, "authored hidden without the code flag");
    f.code_shown = {1, 1};
    f.hardpoint_hidden = {0, 1};
    auto p = f.run();
    expect(p.records[0].status == MapEffectStatus::admitted, "the code shows an authored-hidden proxy");
    expect(p.records[1].cause == MapEffectCause::hardpoint_state, "a hardpoint's state still hides it");
    expect(p.records[2].cause == MapEffectCause::hidden_proxy, "a proxy past the mask keeps its authored flag");
}

void identity_frames_budget_seed() {
    Fixture f{"Smoke", "Smoke", "Smoke"};
    f.placement.effects[1].alternate_suffix_removed = true;
    auto p = f.run(6);
    expect(p.records.size() == 3 && p.records[0].proxy_ordinal == 0 && p.records[1].proxy_ordinal == 1
        && p.records[2].proxy_ordinal == 2, "duplicate names retain original ordinals");
    expect(p.records[0].map_logical_path == "data/art/maps/test.ted" && p.records[0].record_ordinal == 23
        && p.records[0].model_logical_path == "data/art/models/host.alo"
        && p.records[0].model_source.source_id == "archive:unit-test"
        && p.records[0].model_provenance.logical_path == "data/xml/objects.xml"
        && p.records[1].alternate_suffix_removed, "source paths and provenance retained");
    expect(p.records[0].emitter_frame && p.records[0].emitter_frame->rows[0][3] == integer(12)
        && p.records[0].emitter_frame->rows[1][3] == integer(23)
        && p.records[0].emitter_frame->rows[2][3] == integer(34), "fixed placement plus proxy frame");
    expect(p.allocated_capacity == 6 && p.records[2].cause == MapEffectCause::capacity_exhausted
        && p.records[2].capacity == 0, "aggregate budget bounded in source order");
    auto repeat = f.run(6);
    expect(repeat.records[0].seed == p.records[0].seed && repeat.records[1].seed == p.records[1].seed
        && repeat.records[0].seed != p.records[1].seed && repeat.records[0].seed != f.run(6, 100).records[0].seed,
        "per-effect seeds are deterministic and ordinal-specific");
    f.placement.scene_ordinal = std::numeric_limits<std::uint64_t>::max();
    auto extreme = f.run(6, std::numeric_limits<std::uint64_t>::max());
    expect(extreme.records[0].seed != 0
        && extreme.records[0].seed == f.run(6, std::numeric_limits<std::uint64_t>::max()).records[0].seed,
        "maximal seed and ordinal are deterministic without signed overflow");
    f.effects[0].requested_capacity = 0;
    expect(f.run().records[0].cause == MapEffectCause::zero_capacity, "zero capacity never implies a default");
    f.effects[0].requested_capacity = 3;
    f.frames.clear();
    expect(f.run().records[0].cause == MapEffectCause::missing_frame, "missing bone frame diagnosed");
    f.frames.push_back(translated(2, 3, 4));
    f.model.proxies[0].bone = 2;
    f.placement.effects[0].bone = 2;
    expect(f.run().records[0].cause == MapEffectCause::missing_bone, "missing proxy bone diagnosed");
    f.model.proxies[0].bone = 0;
    f.placement.effects[0].bone = 0;
    f.placement.transform->matrix.rows[0][3] = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    expect(f.run().records[0].cause == MapEffectCause::frame_overflow, "fixed composition overflow diagnosed");
}
void attached_model_frame() {
    Fixture f{"Pulse"};
    // A rotated, scaled attachment bone in the owner's model space, rather than the hull origin.
    auto& attachment = f.placement.transform->matrix;
    attachment = translated(10, 20, 30);
    attachment.rows[0][0] = integer(0);
    attachment.rows[0][1] = integer(-2);
    attachment.rows[1][0] = integer(2);
    attachment.rows[1][1] = integer(0);
    attachment.rows[2][2] = integer(2);
    const auto p = f.run();
    expect(p.records[0].status == MapEffectStatus::admitted && p.records[0].emitter_frame,
        "attached-model pulse is admitted at its proxy bone");
    const auto& local = *p.records[0].emitter_frame;
    expect(local.rows[0][3] == integer(4) && local.rows[1][3] == integer(24)
        && local.rows[2][3] == integer(38), "attachment rotation and scale reach the proxy origin");
    const auto world = sim::math::compose(translated(100, -200, 300), local);
    expect(world && world.value().rows[0][3] == integer(104)
        && world.value().rows[1][3] == integer(-176) && world.value().rows[2][3] == integer(338),
        "live ship placement composes with attachment and proxy frames");
    f.model.proxies[0].visible = false;
    expect(f.run().records[0].cause == MapEffectCause::hidden_proxy,
        "attached model retains its authored proxy visibility");
    f.code_shown = {1};
    expect(f.run().records[0].status == MapEffectStatus::admitted,
        "recursive emitter-type visibility can show an attached proxy");
}
} // namespace

int main() {
    variants();
    visibility_and_resolution();
    hardpoint_states();
    code_shown_proxies();
    identity_frames_budget_seed();
    attached_model_frame();
    std::cout << "map effect plan contracts passed\n";
}
