#include "renderer_test_support.hpp"
#include "upload_identity_pool.hpp"

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

} // namespace eawr_renderer_test
