#include "eawr/presentation/renderer.hpp"
#include "diagnostic_buffer.hpp"
#include "instance_reconciliation.hpp"
#include "missing_asset_waits.hpp"
#include "pass_submission.hpp"
#include "submission_plan.hpp"
#include "particle_texture.hpp"
#include "particle_prewarm.hpp"
#include "particle_upload.hpp"
#include "resource_lease_ledger.hpp"
#include "shader_adapter.hpp"
#include "upload_identity.hpp"
#include "upload_winding.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "renderer_test_support.hpp"

namespace eawr_renderer_test {

int failures{};

void check(const bool condition, const std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

eawr::sim::math::Fixed fixed(const std::int64_t integer) {
    return eawr::sim::math::Fixed::from_raw(integer * eawr::sim::math::Fixed::scale);
}

eawr::core::Diagnostic diagnostic(std::string message) {
    return {
        .code = "EAWR-PRES-0002",
        .severity = eawr::core::Severity::error,
        .message = std::move(message),
        .logical_path = {},
        .line = {},
        .column = {},
        .source_id = std::string("presentation.godot"),
    };
}

std::vector<std::byte> byte_vector(const std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const unsigned value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

void particle_upload_contracts() {
    namespace particles = eawr::presentation::particles;
    namespace detail = eawr::presentation::godot_backend::detail;
    const particles::ParticleVertex vertex{{3.0F, 5.0F, 7.0F}, {0.5F, 0.1F, 1.2F, -0.2F}, 0.25F, 0.75F};
    std::array<std::uint8_t, 12> position{};
    // Exercise separated, nonzero offsets as returned by the engine format queries.
    std::array<std::uint8_t, 20> attributes{};
    attributes.fill(0xCD);
    detail::pack_particle_vertex(vertex, position.data(), attributes.data() + 2, attributes.data() + 9);
    std::array<float, 3> xyz{};
    std::array<float, 2> uv{};
    std::memcpy(xyz.data(), position.data(), sizeof(xyz));
    std::memcpy(uv.data(), attributes.data() + 9, sizeof(uv));
    check(xyz == std::array<float, 3>{3.0F, 7.0F, -5.0F}, "particle region update uses the model axis conversion");
    check(uv == std::array<float, 2>{0.25F, 0.75F}, "particle region update preserves float UV");
    check(attributes[2] == 127 && attributes[3] == 25 && attributes[4] == 255 && attributes[5] == 0,
        "particle colour bytes truncate and clamp like the public array upload");
    check(attributes[0] == 0xCD && attributes[6] == 0xCD && attributes[17] == 0xCD,
        "particle packing respects attribute offsets");

    particles::VertexStream stream;
    stream.vertices.resize(4);
    stream.indices = {0, 1, 2, 0, 2, 3};
    stream.quads = 1;
    detail::ParticleSurfaceShape shape;
    std::size_t replacements = 0;
    const auto upload = [&] {
        if (!shape.matches(stream)) { ++replacements; shape.assign(stream); }
    };
    for (std::size_t frame = 0; frame < 1000; ++frame) {
        stream.vertices[0].position.x = static_cast<float>(frame);
        upload();
    }
    check(replacements == 1, "1000 moving same-size streams allocate one particle surface");
    stream.indices = {0, 2, 1, 0, 3, 2};
    upload();
    check(replacements == 1, "changed same-size topology updates the index region without replacement");
    stream.vertices.resize(8);
    stream.indices.resize(12);
    upload();
    check(replacements == 2, "particle surface grows when demand exceeds retained capacity");
    stream.indices.resize(6);
    upload();
    check(replacements == 2, "shrinking the index stream retains particle storage");
    for (std::size_t frame = 0; frame < 1000; ++frame) {
        stream.vertices.resize(frame % 2 == 0 ? 4 : 8);
        stream.indices.resize(frame % 2 == 0 ? 6 : 12);
        upload();
    }
    check(replacements == 2, "1000 oscillating particle counts allocate no additional surfaces");
    check(shape.vertices <= 8 + 8 / 2 && shape.indices <= 12 + 12 / 2,
        "retained storage has at most 50 percent spare capacity at its peak demand");
}

void particle_prewarm_contracts(){
    using namespace eawr::presentation::godot_backend::detail;
    ParticleWarmBudget budget;
    const ParticleWarmPolicy policy{"colour","normal","shader",1};
    const std::array charges{ParticleWarmBudget::TextureCharge{"colour",1024},ParticleWarmBudget::TextureCharge{"normal",2048}};
    check(budget.admits(policy,charges),"authored warm policy fits initial budget");
    budget.commit(policy,charges);
    check(budget.texture_bytes()==3072&&budget.emitters()==1,"warm charges include normal texture once");
    budget.commit(policy,charges);
    check(budget.texture_bytes()==3072&&budget.emitters()==1,"repeated warm plan cannot charge twice");
    for(std::int32_t priority=2;priority<=32;++priority){
        const ParticleWarmPolicy next{"colour","normal","shader",priority};
        check(budget.admits(next,charges),"shared textures fit the policy-count budget");
        budget.commit(next,charges);
    }
    check(!budget.admits({"colour","normal","shader",33},charges),"warm policy cap stops additional dormant emitters");
    budget.clear();
    const std::array repeated{ParticleWarmBudget::TextureCharge{"same",ParticleWarmBudget::max_texture_bytes},ParticleWarmBudget::TextureCharge{"same",ParticleWarmBudget::max_texture_bytes}};
    check(budget.admits(policy,repeated),"same texture in two bindings is charged once");
    budget.commit(policy,repeated);
    check(budget.texture_bytes()==ParticleWarmBudget::max_texture_bytes,"full-budget shared texture has one charge");
    const std::array extra{ParticleWarmBudget::TextureCharge{"new",1}};
    check(!budget.admits({"new","","shader",1},extra),"one extra byte cannot exceed warm VRAM cap");
    const std::array huge{ParticleWarmBudget::TextureCharge{"huge",std::numeric_limits<std::uint64_t>::max()}};
    budget.clear();
    check(!budget.admits(policy,huge),"oversized mod texture cannot overflow warm admission");
    check(budget.texture_bytes()==0&&budget.emitters()==0,"warm release clears all retained-policy charges");
}

} // namespace eawr_renderer_test

int main() {
    using namespace eawr_renderer_test;
    particle_texture_contracts();
    particle_prewarm_contracts();
    particle_upload_contracts();
    derived_fog_variant_contracts();
    upload_identity_contracts();
    missing_asset_wait_contracts();
    pass_order_contracts();
    surface_material_contracts();
    snapshot_adapter_contracts();
    submission_plan_contracts();
    submission_presence_contracts();
    instance_resource_contracts();
    gpu_memory_ledger_contracts();
    texture_budget_contracts();

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
