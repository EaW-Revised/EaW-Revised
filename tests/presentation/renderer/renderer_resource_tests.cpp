#include "renderer_test_support.hpp"
#include "upload_identity_pool.hpp"
#include "gpu_memory_ledger.hpp"
#include "texture_budget.hpp"
#include "resource_residency.hpp"
#include "mesh_rest_pose.hpp"

namespace eawr_renderer_test {

void particle_texture_contracts() {
    using namespace eawr;
    using presentation::godot_backend::detail::prepare_particle_texture;
    assets::Texture top;
    top.width = 5;
    top.height = 3;
    top.format = assets::PixelFormat::rgba8;
    for (const auto& [width, height] : {std::pair{5U, 3U}, std::pair{2U, 1U}, std::pair{1U, 1U}}) {
        assets::MipLevel mip;
        mip.width = width;
        mip.height = height;
        mip.row_pitch = width * 4;
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const auto pixel = byte_vector({10 + x, 20 + y, 30 + x + y, 80 + y * 20});
                mip.bytes.insert(mip.bytes.end(), pixel.begin(), pixel.end());
            }
        }
        top.mips.push_back(std::move(mip));
    }
    assets::Texture bottom = top;
    bottom.source_origin = assets::ImageOrigin::bottom_left;
    for (auto& mip : bottom.mips) {
        for (std::size_t y = 0; y < mip.height / 2; ++y) {
            const std::size_t upper = y * mip.row_pitch;
            const std::size_t lower = (mip.height - 1 - y) * mip.row_pitch;
            for (std::size_t col = 0; col < mip.row_pitch; ++col)
                std::swap(mip.bytes[upper + col], mip.bytes[lower + col]);
        }
    }
    const auto original_bottom = bottom.mips;
    const auto top_upload = prepare_particle_texture(top);
    const auto bottom_upload = prepare_particle_texture(bottom);
    check(top_upload && bottom_upload && top_upload.bytes == bottom_upload.bytes,
        "odd-sized equivalent top/bottom RGBA mips must upload identically");
    bool unchanged = bottom.mips.size() == original_bottom.size();
    for (std::size_t i = 0; unchanged && i < bottom.mips.size(); ++i)
        unchanged = bottom.mips[i].bytes == original_bottom[i].bytes;
    check(unchanged, "particle texture preparation must not mutate decoded source mips");
    check(top_upload && top_upload.bytes.size() == 72,
        "odd 5x3, 2x1, 1x1 RGBA mip chain must have its exact upload length");

    assets::Texture bgra;
    bgra.width = 1;
    bgra.height = 2;
    bgra.format = assets::PixelFormat::bgra8;
    bgra.source_origin = assets::ImageOrigin::bottom_left;
    bgra.has_alpha = true;
    bgra.mips = {{1, 2, 4, byte_vector({3, 2, 1, 40, 6, 5, 4, 70})}};
    const auto bgra_upload = prepare_particle_texture(bgra);
    check(bgra_upload && bgra_upload.bytes == byte_vector({4, 5, 6, 70, 1, 2, 3, 40})
            && bgra.mips[0].bytes == byte_vector({3, 2, 1, 40, 6, 5, 4, 70}),
        "BGRA reversal must keep channel order and alpha without source mutation");
    bgra.format = assets::PixelFormat::bgr8;
    bgra.mips = {{1, 2, 3, byte_vector({3, 2, 1, 6, 5, 4})}};
    const auto bgr_upload = prepare_particle_texture(bgra);
    check(bgr_upload && bgr_upload.bytes == byte_vector({4, 5, 6, 1, 2, 3}),
        "BGR reversal must retain red-first upload order");
    bgra.format = assets::PixelFormat::l8;
    bgra.mips = {{1, 2, 1, byte_vector({15, 230})}};
    const auto l8_upload = prepare_particle_texture(bgra);
    check(l8_upload && l8_upload.bytes == byte_vector({230, 15}),
        "single-channel luminance must reverse by row");
    bgra.format = assets::PixelFormat::a8;
    const auto a8_upload = prepare_particle_texture(bgra);
    check(!a8_upload && a8_upload.failure.find("alpha-only") != std::string::npos,
        "alpha-only particle texture upload must be rejected");

    assets::Texture invalid = top;
    invalid.width = 0;
    check(!prepare_particle_texture(invalid), "zero texture width must be rejected");
    invalid = top;
    invalid.height = 0;
    check(!prepare_particle_texture(invalid), "zero texture height must be rejected");
    invalid = top;
    invalid.mips.clear();
    check(!prepare_particle_texture(invalid), "missing texture mips must be rejected");
    invalid = top;
    invalid.width = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) + 1U;
    check(!prepare_particle_texture(invalid), "dimensions exceeding Godot's signed width must be rejected");
    invalid = top;
    invalid.mips[1].width = 3;
    check(!prepare_particle_texture(invalid), "wrong mip dimensions must be rejected");
    invalid = top;
    invalid.mips[0].row_pitch += 4;
    check(!prepare_particle_texture(invalid), "padded or wrong row pitch must be rejected");
    invalid = top;
    invalid.mips[0].bytes.pop_back();
    check(!prepare_particle_texture(invalid), "short mip payload must be rejected");
    invalid = top;
    invalid.mips[0].bytes.push_back(std::byte{0});
    check(!prepare_particle_texture(invalid), "overlong mip payload must be rejected");
    invalid = top;
    invalid.mips.push_back(invalid.mips.back());
    check(!prepare_particle_texture(invalid), "mips after 1x1 must be rejected");

    assets::Texture bc;
    bc.width = 5;
    bc.height = 3;
    bc.format = assets::PixelFormat::bc1;
    bc.mips = {{5, 3, 16, std::vector<std::byte>(16, std::byte{42})}};
    const auto bc_upload = prepare_particle_texture(bc);
    check(bc_upload && bc_upload.bytes == bc.mips[0].bytes,
        "top-left block-compressed texture must remain byte-identical");
    assets::Texture invalid_bc = bc;
    invalid_bc.mips[0].row_pitch = 8;
    check(!prepare_particle_texture(invalid_bc), "compressed block row pitch must be validated");
    bc.source_origin = assets::ImageOrigin::bottom_left;
    const auto rejected_bc = prepare_particle_texture(bc);
    check(!rejected_bc && rejected_bc.failure.find("bottom-left block-compressed") != std::string::npos,
        "bottom-left block-compressed texture must fail with an explicit reason");
}

// A repeated upload under a live asset ID is a shared reference only when it
// carries the same render content; the identity covers exactly what upload
// consumes, so provenance alone never splits one asset into two.
void upload_identity_contracts() {
    using namespace eawr;
    using presentation::godot_backend::detail::upload_identity;
    assets::Model model;
    model.bones.resize(2);
    assets::Mesh mesh;
    mesh.bone = 1;
    assets::Submesh submesh;
    submesh.vertices.resize(3);
    for (std::size_t index = 0; index < submesh.vertices.size(); ++index) {
        submesh.vertices[index].position = {static_cast<float>(index), 1.0F, 2.0F};
        submesh.vertices[index].normal = {0.0F, 0.0F, 1.0F};
    }
    submesh.indices = {0, 1, 2};
    mesh.submeshes.push_back(submesh);
    model.meshes.push_back(mesh);
    assets::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.mips.push_back({1, 1, 4, byte_vector({10, 20, 30, 255})});
    presentation::MaterialDescription material;
    material.route = presentation::MaterialRoute::modern_spatial;
    material.program = "shader_type spatial; void fragment() { ALBEDO = vec3(1.0); }";
    material.bindings.push_back({"eawr_scale", 0.5F});
    const auto base = upload_identity(model, texture, material);
    check(upload_identity(model, texture, material) == base, "equal uploads must share one identity");

    assets::Model relocated = model;
    relocated.source.logical_path = "data/art/models/other_layer.alo";
    relocated.notices.push_back({1, 2, 3, "provenance only"});
    relocated.meshes[0].name = "renamed";
    assets::Texture retagged = texture;
    retagged.source.logical_path = "data/art/textures/other_layer.dds";
    check(upload_identity(relocated, retagged, material) == base,
        "provenance and names must not change the upload identity");

    const auto differs = [&](const assets::Model& m, const assets::Texture& t,
                             const presentation::MaterialDescription& d, const std::string_view what) {
        check(upload_identity(m, t, d) != base, what);
    };
    assets::Model moved = model;
    moved.meshes[0].submeshes[0].vertices[2].position.z = 2.5F;
    differs(moved, texture, material, "a moved vertex must be a different asset");
    assets::Model uv = model;
    uv.meshes[0].submeshes[0].vertices[1].texcoord[0].x = 0.25F;
    differs(uv, texture, material, "a changed UV must be a different asset");
    assets::Model tinted = model;
    tinted.meshes[0].submeshes[0].vertices[0].color = {1.0F, 0.0F, 0.0F, 1.0F};
    differs(tinted, texture, material, "a changed vertex colour (uploaded as COLOR) must be a different asset");
    assets::Model rewound = model;
    rewound.meshes[0].submeshes[0].indices = {0, 2, 1};
    differs(rewound, texture, material, "reordered indices must be a different asset");
    assets::Model rigid = model;
    rigid.meshes[0].bone = 0;
    differs(rigid, texture, material, "a different rigid bone must be a different asset");
    assets::Model boneless = model;
    boneless.bones.pop_back();
    differs(boneless, texture, material, "a different bone count must be a different asset");
    assets::Model hidden = model;
    hidden.meshes[0].visible = false;
    differs(hidden, texture, material, "hiding a mesh must be a different asset");
    assets::Texture recolored = texture;
    recolored.mips[0].bytes[0] = std::byte{11};
    differs(model, recolored, material, "changed texel bytes must be a different asset");
    assets::Texture reformatted = texture;
    reformatted.format = assets::PixelFormat::bgra8;
    differs(model, reformatted, material, "a changed pixel format must be a different asset");
    presentation::MaterialDescription recompiled = material;
    recompiled.program += " ";
    differs(model, texture, recompiled, "changed shader source must be a different asset");
    presentation::MaterialDescription repassed = material;
    repassed.pass = presentation::RenderPass::transparent;
    differs(model, texture, repassed, "a changed render pass must be a different asset");
    presentation::MaterialDescription rebound = material;
    rebound.bindings[0].value = 0.75F;
    differs(model, texture, rebound, "a changed binding value must be a different asset");
    presentation::MaterialDescription retyped = material;
    retyped.bindings[0].value = std::int32_t{0};
    differs(model, texture, retyped, "a changed binding type must be a different asset");
    // Length prefixes keep adjacent strings from aliasing.
    presentation::MaterialDescription left = material;
    left.technique = "ab";
    left.pass_name = "c";
    presentation::MaterialDescription right = material;
    right.technique = "a";
    right.pass_name = "bc";
    check(upload_identity(model, texture, left) != upload_identity(model, texture, right),
        "adjacent material selectors must not alias");

    // Per-binding textures (P1 #27 Planet.fx): none keeps the old identity;
    // each bound texture's name and texels are part of it.
    using presentation::godot_backend::detail::NamedTexture;
    check(upload_identity(model, texture, material, {}) == base,
        "an upload without per-binding textures keeps its identity");
    const std::array<NamedTexture, 1> cloud{{{"CloudTexture", &texture}}};
    const auto clouded = upload_identity(model, texture, material, cloud);
    check(clouded != base, "a per-binding texture must be a different asset");
    check(upload_identity(model, texture, material, cloud) == clouded, "equal per-binding textures share one identity");
    const std::array<NamedTexture, 1> renamed{{{"NormalTexture", &texture}}};
    check(upload_identity(model, texture, material, renamed) != clouded,
        "a per-binding texture's binding name is part of the identity");
    const std::array<NamedTexture, 1> recoloured_cloud{{{"CloudTexture", &recolored}}};
    check(upload_identity(model, texture, material, recoloured_cloud) != clouded,
        "a per-binding texture's texels are part of the identity");

    using presentation::godot_backend::detail::UploadIdentity;
    using presentation::godot_backend::detail::UploadIdentityPool;
    using presentation::godot_backend::detail::upload_identity_bytes;
    UploadIdentityPool pool(2, 4096, 8);
    // The queued job owns the canonical bytes even if decoded assets change
    // or disappear before its digest is read.
    auto owned_model = model;
    auto owned_texture = texture;
    const auto owned = pool.submit(upload_identity_bytes(owned_model, owned_texture, material, cloud));
    owned_model.meshes.clear();
    owned_texture.mips.clear();
    check(owned.get() == clouded, "pending identity owns model and all texture bytes");

    std::vector<std::shared_future<UploadIdentity>> pending;
    std::vector<UploadIdentity> expected;
    for (const std::size_t length : {0U, 1U, 55U, 56U, 63U, 64U, 65U, 127U, 1024U, 4096U}) {
        std::vector<std::uint8_t> bytes(length);
        for (std::size_t index = 0; index < length; ++index)
            bytes[index] = static_cast<std::uint8_t>((index * 37U + length) & 255U);
        expected.push_back(core::sha256(bytes));
        pending.push_back(pool.submit(std::move(bytes)));
    }
    for (std::size_t index = 0; index < pending.size(); ++index)
        check(pending[index].get() == expected[index], "pooled SHA matches scalar at padding boundaries");
    auto state = pool.stats();
    check(state.input_capacity == 0 && state.pending == 0,
        "completed futures retain no admitted input capacity");
    check(state.peak_input_capacity <= 4096 && state.peak_pending <= 8,
        "queued and running inputs respect capacity and job limits");

    // Spare allocation, rather than byte length, is the queue's memory cost.
    UploadIdentityPool capacity_pool(2, 4096, 8);
    pending.clear();
    for (std::size_t index = 0; index < 32; ++index) {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(3072);
        bytes.push_back(static_cast<std::uint8_t>(index));
        pending.push_back(capacity_pool.submit(std::move(bytes)));
    }
    for (const auto& future : pending) static_cast<void>(future.get());
    state = capacity_pool.stats();
    check(state.peak_input_capacity >= 3072 && state.peak_input_capacity <= 4096,
        "admission accounts spare vector capacity");
    check(state.pending == 0 && state.input_capacity == 0,
        "retained futures release spare input allocations");

    UploadIdentityPool oversized_pool(2, 4096, 8);
    const std::vector<std::uint8_t> large(128U * 1024U, 17);
    const auto large_expected = core::sha256(large);
    const auto large_first = oversized_pool.submit(large);
    const auto large_second = oversized_pool.submit(large);
    check(large_first.get() == large_expected && large_second.get() == large_expected,
        "oversized inputs still produce exact identities");
    state = oversized_pool.stats();
    check(state.peak_pending == 1 && state.peak_input_capacity == large.capacity(),
        "an oversized input is admitted alone");

    using PendingResult = std::pair<std::shared_future<UploadIdentity>, UploadIdentity>;
    std::array<std::vector<PendingResult>, 4> concurrent;
    std::vector<std::thread> producers;
    for (std::size_t lane = 0; lane < concurrent.size(); ++lane) {
        producers.emplace_back([&, lane] {
            for (std::size_t index = 0; index < 16; ++index) {
                std::vector<std::uint8_t> bytes(256, static_cast<std::uint8_t>(lane * 16 + index));
                const auto digest = core::sha256(bytes);
                concurrent[lane].emplace_back(pool.submit(std::move(bytes)), digest);
            }
        });
    }
    for (std::thread& producer : producers) producer.join();
    for (const auto& lane : concurrent) {
        for (const auto& [future, digest] : lane)
            check(future.get() == digest, "concurrent admissions keep each input's digest");
    }
    state = pool.stats();
    check(state.pending == 0 && state.input_capacity == 0
            && state.peak_pending <= 8 && state.peak_input_capacity <= 4096,
        "concurrent producers preserve admission accounting");

    pending.clear();
    {
        UploadIdentityPool draining(1);
        for (std::size_t index = 0; index < 8; ++index) pending.push_back(draining.submit(large));
        static_cast<void>(draining.submit(large)); // A failed upload may discard its future.
    }
    for (const auto& future : pending)
        check(future.get() == large_expected, "shutdown drains owned work and leaves valid digest futures");
}

void instance_resource_contracts() {
    using namespace eawr;

    using presentation::godot_backend::detail::InstanceTransition;
    using presentation::godot_backend::detail::reconcile_instance;
    const std::optional<sim::AssetId> visible_asset{91};
    check(reconcile_instance(visible_asset, std::nullopt) == InstanceTransition::remove,
        "a missing requested asset must remove the entity's stale visible instance");
    check(reconcile_instance(std::nullopt, visible_asset) == InstanceTransition::create,
        "the entity must recover by creating an instance after its asset becomes available");
    check(reconcile_instance(visible_asset, visible_asset) == InstanceTransition::retain,
        "a still-valid entity instance must retain its RID");

    // This ledger drives the production RID registry. Exercise the lifecycle
    // that a live viewer sees when multiple scene entities share resources,
    // switch scenes repeatedly, recover an unavailable replacement and then
    // shut down with no retained renderer assets.
    presentation::godot_backend::detail::ResourceLeaseLedger leases;
    check(leases.upload(91), "first successful upload must create a resource lease");
    check(leases.retain(91) && leases.retain(91) && leases.references(91) == 3,
        "shared scene references must hold one stable asset bundle");
    for (std::size_t scene_switch = 0; scene_switch < 32; ++scene_switch) {
        check(reconcile_instance(visible_asset, std::nullopt) == InstanceTransition::remove,
            "unavailable replacement must remove stale scene instances during churn");
        check(reconcile_instance(std::nullopt, visible_asset) == InstanceTransition::create,
            "replacement recovery must create a fresh instance during churn");
    }
    check(leases.release(91) == presentation::godot_backend::detail::ResourceLeaseLedger::Release::retained
            && leases.release(91)
                == presentation::godot_backend::detail::ResourceLeaseLedger::Release::retained
            && leases.release(91) == presentation::godot_backend::detail::ResourceLeaseLedger::Release::free,
        "shared resource bundle must free only after its final release");
    check(!leases.contains(91) && leases.resources().empty(),
        "failed replacement or shutdown must leave no stale leased resource instances");
    check(!leases.retain(404) && leases.release(404)
            == presentation::godot_backend::detail::ResourceLeaseLedger::Release::missing,
        "failed uploads must not create a lease that a later release can free");

    presentation::godot_backend::detail::DiagnosticBuffer diagnostics;
    diagnostics.push(diagnostic("snapshot references unloaded renderer asset 91"));
    for (std::size_t repeat = 0; repeat < 200; ++repeat) {
        diagnostics.push(diagnostic("snapshot references unloaded renderer asset 91"));
    }
    check(diagnostics.entries().size() == 1,
        "consecutive identical missing-asset failures must be deduplicated");
    for (std::size_t index = 0;
         index < presentation::godot_backend::detail::DiagnosticBuffer::capacity + 8;
         ++index) {
        diagnostics.push(diagnostic("snapshot references unloaded renderer asset "
            + std::to_string(100 + index)));
    }
    const auto retained = diagnostics.entries();
    check(retained.size()
            == presentation::godot_backend::detail::DiagnosticBuffer::capacity,
        "renderer diagnostic retention must stay at its fixed capacity");
    check(retained.front().message == "snapshot references unloaded renderer asset 108",
        "bounded diagnostic retention must evict the oldest failure deterministically");
    check(retained.back().message == "snapshot references unloaded renderer asset 171",
        "bounded diagnostic retention must preserve the newest failure");

    // A source triangle with an outward +Z normal is CCW in its local XY plane.
    // Godot's production cull_back path keeps the opposite index order when
    // viewed from +Z. Exercise the same index selector used by upload_mesh.
    assets::Submesh synthetic;
    synthetic.vertices.resize(6);
    synthetic.vertices[0].position = {0.0F, 0.0F, 0.0F};
    synthetic.vertices[1].position = {1.0F, 0.0F, 0.0F};
    synthetic.vertices[2].position = {0.0F, 1.0F, 0.0F};
    synthetic.vertices[3].position = {2.0F, 0.0F, 0.0F};
    synthetic.vertices[4].position = {3.0F, 0.0F, 0.0F};
    synthetic.vertices[5].position = {2.0F, 1.0F, 0.0F};
    for (assets::Vertex& vertex : synthetic.vertices) vertex.normal = {0.0F, 0.0F, 1.0F};
    synthetic.indices = {0, 1, 2, 3, 4, 5};
    std::array<std::uint16_t, 6> uploaded_indices{};
    for (std::size_t index = 0; index < synthetic.indices.size(); ++index) {
        uploaded_indices[index] = presentation::godot_backend::detail::uploaded_triangle_index(
            synthetic.indices, index);
    }
    check(uploaded_indices == std::array<std::uint16_t, 6>{0, 2, 1, 3, 5, 4},
        "ALO outward CCW triangles must reach Godot as clockwise triangles without changing triangle order");
    for (std::size_t index = 0; index < uploaded_indices.size(); index += 3) {
        const assets::Vec3f& a = synthetic.vertices[uploaded_indices[index]].position;
        const assets::Vec3f& b = synthetic.vertices[uploaded_indices[index + 1]].position;
        const assets::Vec3f& c = synthetic.vertices[uploaded_indices[index + 2]].position;
        const float signed_area = (b.x - a.x) * (c.y - a.y)
            - (b.y - a.y) * (c.x - a.x);
        check(signed_area < 0.0F,
            "uploaded triangles must face clockwise while retaining their +Z outward vertex normals");
    }
    check(synthetic.indices == std::vector<std::uint16_t>{0, 1, 2, 3, 4, 5},
        "winding conversion must not mutate decoded ALO geometry");
    // A trailing partial triangle passes through instead of reading past the end.
    const std::vector<std::uint16_t> partial{0, 1, 2, 3, 4};
    check(presentation::godot_backend::detail::uploaded_triangle_index(partial, 3) == 3
            && presentation::godot_backend::detail::uploaded_triangle_index(partial, 4) == 4,
        "indices after the last complete triangle must stay in bounds and unchanged");
}

// #2856: the GPU ledger counts duplicate uploads of one content once as unique.
void gpu_memory_ledger_contracts() {
    using namespace eawr;
    namespace detail = presentation::godot_backend::detail;
    detail::GpuMemoryLedger ledger;
    assets::Texture texture;
    texture.format = assets::PixelFormat::bc1;
    texture.width = 4;
    texture.height = 4;
    texture.mips.push_back({.width = 4, .height = 4, .bytes = std::vector<std::byte>(8, std::byte{7})});
    assets::Texture other = texture;
    other.mips[0].bytes[0] = std::byte{8};
    const detail::GpuItem first = detail::texture_item(texture);
    const detail::GpuItem copy = detail::texture_item(texture);
    const detail::GpuItem distinct = detail::texture_item(other);
    check(first.key == copy.key && first.key != distinct.key && first.bytes == 8 && first.compressed,
        "texture content keys follow the uploaded bytes");
    check(first.check == copy.check && first.check != distinct.check && first.check != first.key,
        "the second texture digest is independent of the first");
    ledger.add(first);
    ledger.add(copy);
    ledger.add(distinct);
    auto totals = ledger.totals()[static_cast<std::size_t>(detail::GpuCategory::texture)];
    check(totals.live == 3 && totals.live_bytes == 24 && totals.unique == 2 && totals.unique_bytes == 16
            && totals.compressed_bytes == 24 && totals.uploads == 3,
        "duplicate texture uploads count as live but not unique");
    ledger.reuse(copy);
    check(ledger.totals()[static_cast<std::size_t>(detail::GpuCategory::texture)].reuses == 1,
        "a request served by a resident copy counts as a reuse");
    ledger.remove(first);
    ledger.remove(distinct);
    totals = ledger.totals()[static_cast<std::size_t>(detail::GpuCategory::texture)];
    check(totals.live == 1 && totals.unique == 1 && totals.unique_bytes == 8 && totals.uploads == 3,
        "a key stays unique while one upload still holds it");
    ledger.remove(copy);
    ledger.remove(copy);
    totals = ledger.totals()[static_cast<std::size_t>(detail::GpuCategory::texture)];
    check(totals.live == 0 && totals.live_bytes == 0 && totals.unique == 0 && totals.unique_bytes == 0,
        "released uploads leave nothing live and an extra release is ignored");
    assets::Texture rgb = texture;
    rgb.format = assets::PixelFormat::bgr8;
    rgb.mips[0].bytes.assign(48, std::byte{1});
    check(detail::texture_item(rgb).bytes == 64 && !detail::texture_item(rgb).compressed,
        "three-channel textures are padded to four channels on the GPU");
    const std::array<assets::Texture, 2> layers{texture, other};
    check(detail::texture_array_item(layers, 16).bytes == 2U * (16U * 16U * 4U * 4U / 3U),
        "texture array layers are RGBA8 at the array edge with a full mip chain");
}

// #3027 GM-04: the texture budget drops mips only when the device's memory is known and full.
void texture_budget_contracts() {
    using namespace eawr;
    namespace detail = presentation::godot_backend::detail;
    {
        detail::ResourceResidency residency;
        residency.pressure();
        residency.budget(1000, 0);
        residency.add(7, false);
        residency.add(8, false);
        const std::array<std::uint64_t, 2> demand{7, 8};
        const auto before = detail::ResourceResidency::facts();
        std::uint64_t attempts{};
        detail::TextureBudget changing_budget;
        changing_budget.set_override(1000);
        changing_budget.add(500);
        for (unsigned frame = 0; frame < 120; ++frame) {
            check(!residency.eager(1000 + frame, 2, 1000),
                "GM-05: changing device usage keeps pressure active without clearing refusals");
            changing_budget.measure(600 + frame % 2 * 100, frame);
            const auto state = changing_budget.state();
            check(state.other_bytes == 100 + frame % 2 * 100,
                "GM-05: fixture varies measured non-texture usage every frame");
            residency.budget(state.device_bytes, state.drop);
            for (const auto asset : residency.plan(demand).admit) {
                ++attempts;
                if (asset == 7) residency.admission_failed(asset); // forced allocator refusal
                else residency.resident(asset, true);
            }
        }
        check(attempts == 2 && detail::ResourceResidency::facts().admission_failures == before.admission_failures + 1,
            "GM-05: a forced refusal is counted once and never retried on unchanged frames");
        check(detail::ResourceResidency::facts().admissions == before.admissions + 1,
            "GM-05: a refused asset does not block other admissions");
        const std::array<std::uint64_t, 1> absent{8};
        static_cast<void>(residency.plan(absent));
        check(residency.plan(demand).admit == std::vector<std::uint64_t>{7},
            "GM-05: demand re-entry permits a new admission attempt");
        residency.admission_failed(7);
        residency.budget(2000, 0);
        check(residency.plan(demand).admit == std::vector<std::uint64_t>{7},
            "GM-05: device budget changes permit a new attempt");

        residency.admission_failed(7);
        residency.budget(2000, 1);
        check(residency.plan(demand).admit == std::vector<std::uint64_t>{7},
            "GM-05: changed mip detail permits a smaller admission");
        residency.admission_failed(7);
        residency.remove(7);
        residency.add(7, false);
        check(residency.plan(demand).admit == std::vector<std::uint64_t>{7},
            "GM-05: a released asset cannot leave a refusal on a replacement");
    }
    {
        assets::Model model;
        model.bones.resize(2);
        model.bones[0].parent = 1;
        model.bones[1].parent = -1;
        for (auto& bone : model.bones) bone.relative_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        model.meshes.resize(1);
        model.meshes[0].bone = 0;
        model.meshes[0].visible = true;
        model.meshes[0].submeshes.resize(1);
        const auto upload_rest = [&](const bool eager) {
            detail::ResourceResidency residency;
            const bool resident = residency.eager(0, 2, eager ? 100 : 1);
            check(resident == eager, "GM-05: parity fixture selects eager and dormant upload");
            std::optional<presentation::animation::Pose> rest;
            // The production mesh uploader calls this shared validation before its residency split.
            return detail::prepare_rigid_rest_pose(model, model.meshes[0].bone >= 0, rest);
        };
        const bool eager = upload_rest(true);
        const bool dormant = upload_rest(false);
        check(!eager && !dormant, "GM-05: child-before-parent rigid upload is refused in both residency modes");
        model.bones[0].parent = -1;
        model.bones[1].parent = 0;
        check(upload_rest(true) && upload_rest(false),
            "GM-05: parent-first rigid upload validates in both residency modes");
    }
    {
        detail::ResourceResidency residency;
        check(residency.eager(400, 600, 1000), "GM-05: exactly fitting content stays eager");
        residency.add(1, true);
        const std::array<std::uint64_t, 1> demand{2};
        check(residency.plan(demand).evict.empty() && residency.plan(demand).admit.empty(),
            "GM-05: fitting cards never evict unused content");
        check(!residency.eager(1000, 1, 1000), "GM-05: projected pressure defers before allocation");
        residency.add(2, false);
        const auto changes = residency.plan(demand);
        check(changes.evict == std::vector<std::uint64_t>{1} && changes.admit == std::vector<std::uint64_t>{2},
            "GM-05: unused content leaves before demanded reserve content enters");
        residency.resident(1, false);
        residency.resident(2, true);
        check(residency.plan(demand).evict.empty() && residency.plan(demand).admit.empty(),
            "GM-05: unchanged demand does not churn uploads");
        residency.remove(1);
        const std::array<std::uint64_t, 2> duplicate{2, 2};
        check(residency.plan(duplicate).admit.empty(), "GM-05: duplicate instances share residency");
        detail::ResourceResidency unknown;
        check(unknown.eager(UINT64_MAX, UINT64_MAX, 0), "GM-05: unknown device memory is unconstrained");
        check(!unknown.eager(UINT64_MAX, 1, UINT64_MAX), "GM-05: admission comparison cannot overflow");
        detail::ResourceResidency measured;
        measured.add(1, true);
        measured.add(2, true);
        measured.pressure();
        const std::array<std::uint64_t, 1> visible{1};
        check(measured.plan(visible).evict == std::vector<std::uint64_t>{2},
            "GM-05: measured pressure evicts unused content without another upload");
    }
    using Budget = detail::TextureBudget;
    assets::Texture texture;
    for (std::uint32_t edge = 1024; edge >= 1; edge /= 2) texture.mips.push_back({.width = edge, .height = edge, .row_pitch = 0, .bytes = {}});
    const Budget::MipEdges edges = Budget::mip_edges(texture);
    check(Budget::usable_drop(edges, 0) == 0 && Budget::usable_drop(edges, 2) == 2,
        "a drop removes the asked number of top mips");
    check(Budget::usable_drop(edges, 9) == 4, "a drop stops before the top edge falls under 64");
    const Budget::MipEdges single{{2048, 2048}};
    check(Budget::usable_drop(single, 3) == 0, "a texture without a stored chain is never dropped");
    const Budget::MipEdges small{{64, 64}, {32, 32}, {16, 16}};
    check(Budget::usable_drop(small, 2) == 0, "a small texture keeps its top mip");

    Budget budget;
    budget.add(1ULL << 40U);
    check(!budget.constrained() && !budget.over() && budget.drop() == 0, "an unknown device is never over budget");
    budget.set_device(1000, "test device");
    check(budget.probed() && budget.state().texture_budget == 900, "the process may fill nine tenths of the device");
    check(budget.over(), "the pool is over a known budget");
    budget.remove(1ULL << 40U, 0);
    budget.add(400);
    budget.measure(700, 10);
    check(budget.state().other_bytes == 300 && budget.state().texture_budget == 600 && !budget.over(),
        "memory outside the pool leaves the pool the rest, and a pool that fits drops nothing");
    budget.add(300);
    check(budget.over() && budget.raise() && budget.drop() == 1, "over budget, the drop rises one level");
    budget.downsized(700, 175, 10);
    check(!budget.over() && budget.state().resident_bytes == 175 && budget.state().downsized == 1,
        "a rebuilt texture frees the difference");
    budget.measure(1000, 11);
    check(budget.state().other_bytes == 300 && !budget.over(),
        "freed copies the device still holds are not memory outside the pool");
    budget.measure(475, 10 + Budget::release_frames);
    check(budget.state().other_bytes == 300 && budget.state().texture_budget == 600,
        "once the engine has released them the counter is taken as it is");
    budget.set_override(4000);
    budget.set_device(100, "late device");
    check(budget.state().device_bytes == 4000 && budget.state().source == "override"
            && budget.state().texture_budget == 3300,
        "an override wins over the device's figure");
    budget.set_minimum_drop(7);
    check(budget.drop() == Budget::max_drop && budget.state().minimum_drop == Budget::max_drop,
        "the detail setting clamps to the deepest drop and sets the floor");
    check(!budget.raise() && budget.state().over_budget == 1, "the deepest drop cannot rise further");
    budget.set_minimum_drop(1);
    check(budget.drop() == Budget::max_drop, "lowering the floor keeps a raised drop while textures are resident");
    budget.remove(175, 20);
    check(budget.drop() == 1, "an empty pool starts over from the detail setting");

    // Releases are stamped with the frame they happen in, not the last measure's.
    Budget timed;
    timed.set_device(1000, "test device");
    timed.add(400);
    timed.measure(500, 100);
    check(timed.state().other_bytes == 100 && timed.state().constrained, "the first measure sees the rest of the device");
    // No submit (no measure, no frame note) between the last measure and the release: the
    // release still carries its own frame.
    timed.remove(200, 129);
    timed.measure(500, 130);
    check(timed.state().other_bytes == 100,
        "a texture released one frame before a measure is still left out of the memory outside the pool");
    timed.measure(300, 129 + Budget::release_frames);
    check(timed.state().other_bytes == 100, "once the engine has released it the counter is taken as it is");
    timed.add(300);
    timed.measure(600, 200);
    timed.downsized(300, 100, 230);
    timed.remove(50, 228);
    timed.measure(600, 231);
    check(timed.state().other_bytes == 100, "an earlier release in the window keeps the latest release's frame");

    // A renderer's progress through the drop: a fake pool of eight textures, each with the drop
    // it was rebuilt at. Nothing here shrinks the pool's bytes, so the budget stays over.
    struct FakePool final {
        Budget& budget;
        std::array<std::uint32_t, 8> dropped{};
        std::size_t calls{};
        bool operator()(const std::size_t limit) {
            ++calls;
            std::size_t rebuilt = 0;
            for (std::uint32_t& drop : dropped) {
                if (drop >= budget.drop()) continue;
                if (rebuilt == limit) return false;
                ++rebuilt;
                drop = budget.drop();
            }
            return true;
        }
        [[nodiscard]] bool at(const std::uint32_t drop) const {
            return std::ranges::all_of(dropped, [&](const std::uint32_t level) { return level == drop; });
        }
    };
    Budget shared;
    shared.set_device(1000, "test device");
    shared.add(2000);
    FakePool pool{.budget = shared};
    detail::TextureDropProgress progress;
    const auto in_use = [] { return std::uint64_t{2000}; };
    for (std::uint32_t level = 0; level < Budget::max_drop; ++level) static_cast<void>(shared.raise());
    progress.at_frame(shared, 1, in_use, pool);
    check(pool.calls == 1 && static_cast<std::size_t>(std::ranges::count(pool.dropped, Budget::max_drop)) == Budget::rebuilds_per_frame
            && progress.behind(Budget::max_drop),
        "a drop raised elsewhere reaches the renderer's textures a few per frame");
    progress.at_upload(shared, pool);
    check(pool.at(Budget::max_drop) && !progress.behind(Budget::max_drop),
        "an upload at the deepest drop finishes an unfinished in-frame rebuild before marking it reached");
    const std::size_t calls = pool.calls;
    check(progress.at_frame(shared, 2, in_use, pool),
        "measured pressure at the deepest drop requests demand residency");
    check(pool.calls == calls, "a reached drop starts no rebuild in the next frame");

    Budget fitting;
    fitting.set_device(1000, "fitting device");
    fitting.add(100);
    detail::TextureDropProgress fit_progress;
    std::size_t reads = 0, rebuilds = 0;
    const auto read_fitting = [&] { ++reads; return std::uint64_t{100}; };
    const auto rebuild_fitting = [&](std::size_t) { ++rebuilds; return true; };
    check(!fit_progress.at_frame(fitting, 1, read_fitting, rebuild_fitting),
        "a fitting measurement leaves demand residency inactive");
    check(!fit_progress.at_frame(fitting, 2, read_fitting, rebuild_fitting) && reads == 1 && rebuilds == 0,
        "a fitting frame between measurements does no counter or drop work");

    Budget loading;
    loading.set_device(1000, "test device");
    loading.add(2000);
    FakePool loaded{.budget = loading};
    detail::TextureDropProgress fresh;
    fresh.at_upload(loading, loaded);
    check(loaded.at(Budget::max_drop) && !fresh.behind(Budget::max_drop) && loading.state().over_budget == 1,
        "an upload over budget rises to the deepest drop with every texture rebuilt");

    // The counter is read once per measure_frames frames for the whole process.
    Budget cadence;
    check(!cadence.due(1), "an unknown device is never measured");
    cadence.set_device(1000, "test device");
    check(cadence.due(1), "the first frame with a known device is due");
    cadence.measure(0, 1);
    check(cadence.due(2), "until the counter reads nonzero it is read every frame");
    cadence.measure(400, 2);
    check(!cadence.due(3) && !cadence.due(1 + Budget::measure_frames) && cadence.due(2 + Budget::measure_frames),
        "a live counter is read once per measure_frames frames");

    Budget off;
    off.disable();
    off.set_device(1000, "late device");
    off.add(1ULL << 40U);
    check(off.probed() && !off.constrained() && !off.over() && !off.due(10) && off.state().source == "off",
        "the off switch keeps every texture and never probes or measures");
    off.rebuild_failed();
    check(off.state().rebuild_failures == 1, "refused rebuilds are counted");

    detail::GpuMemoryLedger ledger;
    assets::Texture bc;
    bc.format = assets::PixelFormat::bc1;
    bc.width = 4;
    bc.height = 4;
    bc.mips.push_back({.width = 4, .height = 4, .bytes = std::vector<std::byte>(64, std::byte{3})});
    const detail::GpuItem item = detail::texture_item(bc);
    ledger.add(item);
    ledger.resize(item, 16);
    const auto totals = ledger.totals()[static_cast<std::size_t>(detail::GpuCategory::texture)];
    check(totals.live == 1 && totals.live_bytes == 16 && totals.unique_bytes == 16 && totals.compressed_bytes == 16
            && totals.uploads == 1,
        "a resized upload keeps its identity and holds the new bytes");
    detail::GpuItem resized = item;
    resized.bytes = 16;
    ledger.remove(resized);
    const auto after = ledger.totals()[static_cast<std::size_t>(detail::GpuCategory::texture)];
    check(after.live == 0 && after.live_bytes == 0 && after.unique_bytes == 0, "a resized upload releases cleanly");
}

} // namespace eawr_renderer_test
