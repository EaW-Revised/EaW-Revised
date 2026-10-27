#include "../../support/check.hpp"

#include "eawr/presentation/renderer.hpp"
#include "diagnostic_buffer.hpp"
#include "instance_reconciliation.hpp"
#include "missing_asset_waits.hpp"
#include "pass_submission.hpp"
#include "submission_plan.hpp"
#include "particle_texture.hpp"
#include "particle_prewarm.hpp"
#include "particle_spawn_trace.hpp"
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

void churn_contracts();

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void check(const bool condition, const std::string_view message) {
    local_checker.check(condition, message, std::cerr);
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

void particle_program_prewarm_contracts() {
    using namespace eawr::presentation::godot_backend::detail;
    ParticleProgramWarmBudget budget;
    for (std::size_t index = 0; index < ParticleProgramWarmBudget::max_programs; ++index) {
        const std::string program = "mod-program-" + std::to_string(index);
        check(budget.admits(program) && budget.admits(program), "program admission is repeatable");
        check(budget.size() == index && !budget.contains(program), "admission does not retain a program");
        budget.commit(program);
        check(budget.size() == index + 1, "successful program acquisition commits exactly once");
    }
    check(budget.full() && !budget.admits("one-more-program"), "the thirty-third distinct program is refused");
    check(budget.admits("mod-program-0"), "an existing program is admissible at the cap");
    budget.commit("mod-program-0");
    check(budget.size() == ParticleProgramWarmBudget::max_programs, "duplicate commit adds no program hold");
    budget.clear();
    check(budget.size() == 0 && !budget.full() && budget.admits("another-mod-program"),
        "release restores program capacity");
}

void particle_spawn_journal_contracts() {
    using namespace eawr::presentation::godot_backend::detail;
    std::string output;
    const auto sink = [&](const std::string_view text) { output.append(text); };
    {
        ParticleSpawnJournal disabled(false, sink);
        ParticleSpawnRecord record;
        disabled.append(std::move(record));
        check(!disabled.enabled() && disabled.records() == 0 && output.empty(),
            "disabled diagnostics retain and output nothing");
    }
    {
        ParticleSpawnJournal journal(true, sink);
        {
            const std::string effect = "mod-only-effect";
            const std::string texture = "mod-only-texture";
            ParticleSpawnTrace trace(&journal, effect, 77, texture, 3);
            trace.resource = 17;
            trace.shader_cold = true;
        }
        check(journal.records() == 1 && output.empty(), "spawn diagnostics buffer without output in create");
        journal.flush();
        check(journal.records() == 0 && output.find("mod-only-effect") != std::string::npos
            && output.find("\"records\":1") != std::string::npos,
            "teardown output owns its strings after the spawn caller is gone");
    }
    output.clear();
    {
        ParticleSpawnJournal journal(true, sink);
        ParticleSpawnRecord oversized;
        oversized.texture.assign(ParticleSpawnJournal::max_text_bytes + 1, 'x');
        journal.append(std::move(oversized));
        check(journal.records() == 0 && journal.dropped() == 1 && output.empty(),
            "diagnostic text bound rejects an oversized record without frame output");
        journal.flush();
        check(output.find("\"dropped\":1") != std::string::npos, "diagnostic truncation is explicit");
    }
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
    ParticleWarmBudget units(512, 96U * 1024U * 1024U);
    const std::array lens{ParticleWarmBudget::TextureCharge{"lens", 4U * 1024U * 1024U}};
    for (std::int32_t priority = 0; priority < 512; ++priority) {
        const ParticleWarmPolicy next{"lens", "", "program", priority};
        check(units.admits(next, lens), "unit policy allowance admits shared texture policies beyond default32");
        units.commit(next, lens);
    }
    check(units.texture_bytes() == 4U * 1024U * 1024U,
          "larger unit policy allowance still charges each shared texture only once");
    check(!units.admits({"lens", "", "program", 512}, lens), "configured policy cap remains bounded");
    units.clear();
    const std::array full{ParticleWarmBudget::TextureCharge{"full", 96U * 1024U * 1024U}};
    check(units.admits(policy, full), "configured texture allowance admits exact boundary");
    units.commit(policy, full);
    check(!units.admits({"new", "", "program", 0}, extra), "configured texture cap refuses one extra byte");
    ParticleWarmBudget unchanged;
    check(unchanged.policy_limit() == 32 && unchanged.texture_limit() == 8U * 1024U * 1024U,
          "other backends keep their original warm limits");
    ParticleWarmDemand demand;
    demand.observe(policy, charges, true);
    demand.observe(policy, charges, true);
    demand.observe({"missing", "normal", "shader", 1}, std::span(charges).subspan(1), false);
    check(demand.references() == 3 && demand.policies() == 2 && demand.textures() == 2
              && demand.texture_bytes() == 3072 && demand.unresolved() == 1 && !demand.complete(),
          "demand includes refused/missing policies, deduplicates shared bindings and labels incomplete bytes");
    demand.clear();
    check(demand.references() == 0 && demand.policies() == 0 && demand.texture_bytes() == 0 && demand.complete(),
          "demand ownership clears at teardown");
}

void reachable_particle_prewarm_contracts() {
    using namespace eawr::presentation::godot_backend::detail;
    struct Plan final {
        bool drawable;
        int identity;
        ParticleWarmPolicy policy;
        std::uint64_t texture_bytes;
    };
    const ParticleWarmPolicy shared{"shared", "", "program", 1};
    const std::array plans{
        Plan{false, 0, {}, 0},
        Plan{true, 1, shared, 1024},
        Plan{true, 1, shared, 1024},
        Plan{true, 2, {"oversized", "", "program", 1}, ParticleWarmBudget::max_texture_bytes + 1},
        Plan{true, 3, {"other", "", "program", 1}, 512},
        Plan{true, 1, shared, 1024}};
    ParticleWarmBudget budget;
    std::vector<int> visits;
    const auto coverage = prewarm_particle_plans(plans, [&](const Plan& plan) {
        visits.push_back(plan.identity);
        const std::array charges{ParticleWarmBudget::TextureCharge{plan.policy.texture, plan.texture_bytes}};
        if (!budget.admits(plan.policy, charges)) return false;
        budget.commit(plan.policy, charges);
        return true;
    });
    check(visits == std::vector<int>{1, 1, 2, 3, 1}, "warm visits drawable references in input order after a refusal");
    check(coverage.requested == 5 && coverage.admitted == 4 && coverage.refused == 1,
        "partial warm coverage reports refused references without dropping later plans");
    check(budget.emitters() == 2 && budget.texture_bytes() == 1536,
        "shared references retain one bounded backend hold and charge per creation identity");
    budget.clear();
    check(budget.emitters() == 0 && budget.texture_bytes() == 0, "load warm ownership releases at teardown");
    const auto empty = prewarm_particle_plans(std::array<Plan, 0>{}, [](const Plan&) { return false; });
    check(empty.requested == 0 && empty.admitted == 0 && empty.refused == 0, "empty closure has no warm work");
}

} // namespace eawr_renderer_test

int main() {
    using namespace eawr_renderer_test;
    churn_contracts();
    particle_texture_contracts();
    particle_prewarm_contracts();
    reachable_particle_prewarm_contracts();
    particle_program_prewarm_contracts();
    particle_spawn_journal_contracts();
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
