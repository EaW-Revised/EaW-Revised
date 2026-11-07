#include "../../support/check.hpp"
#include "eawr/presentation/model_upload.hpp"
#include "eawr/assets/memory.hpp"
#include "mesh_hidden_geometry.hpp"
#include "model_digest_memo.hpp"
#include "model_recipe.hpp"
#include "resource_residency.hpp"
#include "upload_identity.hpp"

#include <array>
#include <future>
#include <iostream>
#include <map>
#include <new>

int main() {
    using namespace eawr;
    using namespace presentation;
    using namespace presentation::godot_backend::detail;
    testing::Checker checks;
    const auto check = [&](const bool value, const char* message) {
        checks.check(value, message, std::cerr);
    };
    auto decoded = std::make_shared<assets::Model>();
    // Reserved slack must count; strings and collision subset must not inflate
    // the additive total. Hidden geometry still belongs to the decoded owner.
    {
        assets::Model model;
        model.meshes.resize(1);
        model.meshes[0].collidable = true;
        model.meshes[0].visible = false;
        model.meshes[0].submeshes.resize(1);
        auto& surface = model.meshes[0].submeshes[0];
        surface.vertices.reserve(9);
        surface.vertices.resize(3);
        surface.indices.reserve(12);
        surface.parameters.reserve(2);
        surface.skin_bones.reserve(4);
        model.bones.reserve(5);
        const auto capacity = assets::payload_capacity(model);
        check(capacity.vertex_bytes == surface.vertices.capacity() * sizeof(assets::Vertex)
            && capacity.index_bytes == surface.indices.capacity() * sizeof(std::uint16_t),
            "geometry counts capacity including reserved slack and hidden collision meshes");
        check(capacity.collision_bytes == capacity.vertex_bytes + capacity.index_bytes
            && capacity.material_bytes == surface.parameters.capacity() * sizeof(assets::MaterialParameter)
            && capacity.bone_bytes == model.bones.capacity() * sizeof(assets::Bone)
                + surface.skin_bones.capacity() * sizeof(std::uint32_t),
            "material, bones and collision subset have exact element-byte accounting");
        check(capacity.total_bytes() == capacity.vertex_bytes + capacity.index_bytes
            + capacity.material_bytes + capacity.bone_bytes + capacity.structure_bytes,
            "collision subset is excluded from additive total");
        assets::Texture cpu_texture;
        cpu_texture.mips.resize(2);
        cpu_texture.mips[0].bytes.reserve(32);
        cpu_texture.mips[1].bytes.reserve(8);
        assets::PayloadProbe probe;
        probe.model(&model); probe.model(&model);
        probe.texture(&cpu_texture); probe.texture(&cpu_texture);
        check(probe.result.models == 1 && probe.result.textures == 1
            && probe.result.model_capacity == capacity
            && probe.result.texture_capacity.texture_payload_bytes
                == cpu_texture.mips[0].bytes.capacity() + cpu_texture.mips[1].bytes.capacity(),
            "explicit recipe probes deduplicate owners and include every retained mip capacity");
    }
    decoded->bones.resize(1);
    decoded->meshes.resize(2);
    for (std::size_t index = 0; index < decoded->meshes.size(); ++index) {
        auto& mesh = decoded->meshes[index];
        mesh.visible = index == 0;
        mesh.bone = 0;
        mesh.submeshes.resize(2);
        for (std::size_t surface = 0; surface < mesh.submeshes.size(); ++surface) {
            auto& geometry = mesh.submeshes[surface];
            geometry.vertices.resize(3);
            geometry.vertices[0].position.x = static_cast<float>(index * 10 + surface);
            geometry.vertices[1].position.y = 1.0F;
            geometry.vertices[2].position.z = 1.0F;
            geometry.indices = {0, 1, 2};
        }
    }
    const assets::Texture texture;
    const MaterialDescription material;
    // Compare the consumed upload bytes against the former extracted model,
    // including hidden meshes, forced visibility, and every surface index.
    for (std::size_t mesh = 0; mesh < 2; ++mesh) {
        for (std::size_t surface = 0; surface < 2; ++surface) {
            for (const bool force_visible : {false, true}) {
                ModelUpload recipe{decoded, ModelSurface{mesh, surface, force_visible}};
                assets::Model extracted;
                extracted.bones = decoded->bones;
                extracted.meshes.push_back(decoded->meshes[mesh]);
                extracted.meshes[0].submeshes = {decoded->meshes[mesh].submeshes[surface]};
                extracted.meshes[0].visible = decoded->meshes[mesh].visible || force_visible;
                check(upload_identity_bytes(recipe.view(), texture, material)
                        == upload_identity_bytes(extracted, texture, material),
                    "shared surface consumes exactly the former extracted mesh bytes");
                check(hidden_skin_geometry_collapses(recipe.view()) == hidden_skin_geometry_collapses(extracted),
                    "shared surface preserves hidden geometry classification");
            }
        }
    }
    check(upload_identity_bytes(ModelUpload{decoded, {}}.view(), texture, material)
            == upload_identity_bytes(*decoded, texture, material),
        "whole-model view preserves upload bytes");
    check(!ModelUpload{}.valid(), "missing owner is invalid");
    check(!ModelUpload{decoded, ModelSurface{2, 0, false}}.valid(), "invalid mesh is rejected");
    check(!ModelUpload{decoded, ModelSurface{0, 2, false}}.valid(), "invalid submesh is rejected");

    std::map<std::uint64_t, ModelUpload> recipes;
    recipes.emplace(10, ModelUpload{decoded, ModelSurface{1, 1, true}});
    recipes.emplace(11, ModelUpload{decoded, ModelSurface{1, 1, true}});
    check(decoded.use_count() == 3, "two upload recipes share one cache model owner");
    const auto* vertices = decoded->meshes[1].submeshes[1].vertices.data();
    const auto before = upload_identity_bytes(recipes.at(10).view(), texture, material);
    std::weak_ptr<const assets::Model> lifetime = decoded;
    decoded.reset();
    ResourceResidency residency;
    residency.add(10, true);
    residency.add(11, true);
    residency.pressure();
    const std::array<std::uint64_t, 1> first_demand{11};
    const auto evictions = residency.plan(first_demand);
    check(evictions.evict == std::vector<std::uint64_t>{10}, "pressure evicts only the unused upload");
    residency.resident(10, false);
    check(!lifetime.expired(), "eviction and cache destruction keep recipe geometry alive");
    const std::array<std::uint64_t, 2> renewed_demand{10, 11};
    const auto admissions = residency.plan(renewed_demand);
    check(admissions.admit == std::vector<std::uint64_t>{10}, "renewed demand re-admits the evicted upload");
    // The worker owns its handle while the presentation side releases the
    // other upload. Views contain spans into immutable, shared geometry.
    auto worker = std::async(std::launch::async, [recipe = recipes.at(10), &texture, &material] {
        return upload_identity_bytes(recipe.view(), texture, material);
    });
    recipes.erase(11);
    const auto view = recipes.at(10).view();
    check(view.meshes[0].submeshes[0].vertices.data() == vertices,
        "re-admission reads the original vertex allocation without a deep copy");
    check(upload_identity_bytes(view, texture, material) == before && worker.get() == before,
        "re-admission and worker read preserve every consumed mesh byte");
    residency.resident(10, true);
    recipes.clear();
    check(lifetime.expired(), "releasing the final upload releases the cached model");

    // Reloadable recipe: release after admission, reload validates the digest.
    {
        const auto make = [] {
            auto model = std::make_shared<assets::Model>();
            model->source.logical_path = "data/art/models/test.alo";
            model->bones.resize(1);
            model->meshes.resize(1);
            model->meshes[0].submeshes.resize(2);
            for (auto& submesh : model->meshes[0].submeshes) {
                submesh.vertices.resize(3);
                submesh.vertices[1].position.y = 1.0F;
                submesh.indices = {0, 1, 2};
            }
            return model;
        };
        int loads = 0;
        bool corrupt = false;
        const ModelLoader loader = [&](const assets::Source&) -> std::optional<assets::Model> {
            ++loads;
            auto model = make();
            if (corrupt) model->meshes[0].submeshes[0].vertices[2].position.x = 5.0F;
            return std::move(*model);
        };
        ModelRecipeStats stats;
        ModelRecipe first{ModelUpload{make(), ModelSurface{0, 0, false}}, loader, true};
        ModelRecipe second{ModelUpload{make(), ModelSurface{0, 1, false}}, loader, true};
        ModelRecipe::Batch none;
        const auto initial = first.read(none);
        const auto before_release = upload_identity_bytes(initial->view(), assets::Texture{}, MaterialDescription{});
        check(first.uploaded(stats) && second.uploaded(stats) && first.released() && second.released()
                && stats.ever_released == 2 && stats.ever_fallback == 0 && loads == 0,
            "geometry is released after admission without a second decode");
        ModelRecipe::Batch batch;
        const auto a = first.read(batch);
        const auto b = second.read(batch);
        check(a && b && loads == 1 && a->model == b->model,
            "sibling surfaces share one transient model for the batch");
        check(upload_identity_bytes(a->view(), assets::Texture{}, MaterialDescription{}) == before_release,
            "evict -> restore feeds identical upload inputs");
        batch.clear();
        corrupt = true;
        ModelRecipe::Batch changed;
        check(!first.read(changed) && changed.empty(), "changed content fails the reload instead of uploading a different mesh");
        ModelRecipe generated{ModelUpload{std::make_shared<const assets::Model>(), {}}, loader, true};
        check(!generated.uploaded(stats) && !generated.released() && stats.ever_fallback == 1,
            "generated model without a source keeps its snapshot and is counted");
        ModelRecipe derived{ModelUpload{make(), {}}, loader, false};
        check(!derived.uploaded(stats) && !derived.released() && stats.ever_fallback == 2,
            "a derived model that keeps a source path never releases (counted fallback)");
        ModelRecipe no_loader{ModelUpload{make(), {}}, {}, true};
        check(!no_loader.uploaded(stats) && stats.ever_fallback == 3, "missing loader is a counted fallback");
    }

    // The upload digest memo (#5197): one digest per shared model object, exactly model_identity, never a stale answer.
    {
        const auto make_model = [](const float height) {
            auto model = std::make_shared<assets::Model>();
            model->source.logical_path = "data/art/models/memo.alo";
            model->bones.resize(1);
            model->meshes.resize(1);
            model->meshes[0].submeshes.resize(2);
            for (auto& submesh : model->meshes[0].submeshes) {
                submesh.vertices.resize(3);
                submesh.vertices[1].position.y = height;
                submesh.indices = {0, 1, 2};
            }
            return model;
        };
        int computed = 0;
        const auto counted = [&computed](const assets::Model& model) {
            ++computed;
            return model_identity(model);
        };
        ModelDigestMemo memo;
        const std::shared_ptr<const assets::Model> shared = make_model(1.0F);
        const auto first = memo.identity(shared, counted);
        check(first == model_identity(*shared), "the memo answers exactly the model_identity digest");
        check(memo.identity(shared, counted) == first && memo.identity(shared) == first && computed == 1 && memo.size() == 1,
            "every further surface of the same model is a hit, not another hash");
        const std::shared_ptr<const assets::Model> other = make_model(2.0F);
        check(memo.identity(other, counted) != first && computed == 2 && memo.size() == 2,
            "a different model has its own digest and is hashed once");

        // A freed model's address reused by another model (same storage, a fresh control block) must recompute.
        alignas(assets::Model) unsigned char storage[sizeof(assets::Model)];
        const auto keep_storage = [](const assets::Model*) {};
        auto* original = new (storage) assets::Model(*make_model(1.0F));
        std::shared_ptr<const assets::Model> live(original, keep_storage);
        ModelDigestMemo reuse;
        computed = 0;
        const auto stale = reuse.identity(live, counted);
        const assets::Model* address = original;
        live.reset();
        original->~Model();
        auto* replacement = new (storage) assets::Model(*make_model(2.0F));
        const std::shared_ptr<const assets::Model> reborn(replacement, keep_storage);
        const auto fresh = reuse.identity(reborn, counted);
        check(replacement == address && computed == 2 && fresh == model_identity(*replacement) && fresh != stale,
            "a freed model's reused address gets a fresh hash, never the old model's digest");
        check(reuse.identity(reborn, counted) == fresh && computed == 2, "and the new model is memoised from then on");
        replacement->~Model();

        // Recipes admitted with and without the memo end with the same identity, and both restore the same upload.
        const ModelLoader loader = [&](const assets::Source&) -> std::optional<assets::Model> { return *make_model(1.0F); };
        ModelRecipeStats memo_stats;
        ModelRecipeStats plain_stats;
        ModelDigestMemo recipe_memo;
        const std::shared_ptr<const assets::Model> model_a = make_model(1.0F);
        ModelRecipe via_memo_0{ModelUpload{model_a, ModelSurface{0, 0, false}}, loader, true};
        ModelRecipe via_memo_1{ModelUpload{model_a, ModelSurface{0, 1, false}}, loader, true};
        ModelRecipe plain{ModelUpload{make_model(1.0F), ModelSurface{0, 0, false}}, loader, true};
        const auto from_memo = [&](const std::shared_ptr<const assets::Model>& model) { return recipe_memo.identity(model); };
        check(via_memo_0.uploaded(memo_stats, from_memo) && via_memo_1.uploaded(memo_stats, from_memo) && plain.uploaded(plain_stats)
                && via_memo_0.identity() == plain.identity() && via_memo_1.identity() == plain.identity() && recipe_memo.size() == 1,
            "a recipe admitted through the memo has the identity of one admitted without it; sibling surfaces share one entry");
        ModelRecipe::Batch memo_batch;
        ModelRecipe::Batch plain_batch;
        const auto restored = via_memo_0.read(memo_batch);
        const auto expected = plain.read(plain_batch);
        check(restored && expected && upload_identity_bytes(restored->view(), assets::Texture{}, MaterialDescription{})
                == upload_identity_bytes(expected->view(), assets::Texture{}, MaterialDescription{}),
            "evict -> restore after a memoised admission feeds identical upload inputs");
    }
    return checks.finish();
}
