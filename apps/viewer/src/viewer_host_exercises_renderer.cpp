#include "viewer_host_internal.hpp"

namespace eawr::presentation::godot_backend {
[[nodiscard]] sim::math::Mat3x4 viewer_host_detail::runtime_transform(
    const std::int64_t x, const std::int64_t z) {
    using Fixed = sim::math::Fixed;
    const Fixed zero = Fixed::from_raw(0);
    const Fixed one = Fixed::from_raw(Fixed::scale);
    const Fixed translate = Fixed::from_raw(x * Fixed::scale);
    sim::math::Mat3x4 result{};
    result.rows[0] = {one, zero, zero, translate};
    result.rows[1] = {zero, one, zero, zero};
    result.rows[2] = {zero, zero, one, Fixed::from_raw(z * Fixed::scale)};
    return result;
}

[[nodiscard]] assets::Model viewer_host_detail::runtime_model(const float half_extent) {
    assets::Submesh submesh;
    submesh.vertices = {
        {{-half_extent, 0.0F, -half_extent}, {0.0F, -1.0F, 0.0F}, {}, {}, {}, {}},
        {{half_extent, 0.0F, -half_extent}, {0.0F, -1.0F, 0.0F}, {}, {}, {}, {}},
        {{0.0F, 0.0F, half_extent}, {0.0F, -1.0F, 0.0F}, {}, {}, {}, {}},
    };
    submesh.indices = {0, 1, 2};
    assets::Mesh mesh;
    mesh.name = "runtime-pass-triangle";
    mesh.visible = true;
    mesh.submeshes.push_back(std::move(submesh));
    assets::Model model;
    model.meshes.push_back(std::move(mesh));
    return model;
}

[[nodiscard]] assets::Texture viewer_host_detail::runtime_texture() {
    assets::MipLevel mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.bytes = {std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    assets::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.format = assets::PixelFormat::rgba8;
    texture.mips.push_back(std::move(mip));
    return texture;
}

[[nodiscard]] MaterialDescription viewer_host_detail::runtime_material(
    const RenderPass pass, const std::string_view source) {
    return MaterialDescription{
        .schema_version = MaterialDescription::current_schema_version,
        .route = MaterialRoute::modern_spatial,
        .pass = pass,
        .program = std::string(source),
        .technique = {},
        .pass_name = {},
        .bindings = {},
    };
}

namespace {

// Upper bound for one renderer diagnostic message observed by the runtime
// exercise. The public contract messages are short fixed sentences; anything
// longer would mean caller-supplied text (such as shader source) leaked in.
constexpr std::size_t max_runtime_diagnostic_message = 256;

// Uploads a material whose selector or source is outside the contract and
// requires the live adapter to reject it exactly like validate_material: one
// bounded error with the expected code (EAWR-RENDER-0001 unless given), the
// history still capped, and neither the resource registry nor the live
// instance set touched.
[[nodiscard]] bool rejects_unknown_selector(
    GodotRenderer& renderer, const sim::AssetId asset_id, const MaterialDescription& material,
    const std::string_view expected_message,
    const std::string_view expected_code = diagnostic_codes::invalid_material) {
    const std::size_t diagnostics_before = renderer.diagnostics().size();
    const std::size_t instances_before = renderer.instance_count();
    if (!renderer.resources().empty()) return false;
    const auto rejected = renderer.upload(asset_id, runtime_model(), runtime_texture(), material);
    if (rejected) return false;
    const core::Diagnostic& error = rejected.error();
    const std::span<const core::Diagnostic> history = renderer.diagnostics();
    const bool recorded = diagnostics_before < detail::DiagnosticBuffer::capacity
        ? history.size() == diagnostics_before + 1
        : history.size() == detail::DiagnosticBuffer::capacity;
    return error.code == expected_code
        && error.severity == core::Severity::error && error.message == expected_message
        && error.message.size() <= max_runtime_diagnostic_message && recorded
        && history.back().code == error.code && history.back().message == error.message
        && renderer.resources().empty() && renderer.instance_count() == instances_before;
}


} // namespace

bool ViewerHost::start_renderer_runtime_exercise(const std::shared_ptr<GodotShaderCache>& shaders) {
    const assets::Model model = runtime_model();
    const assets::Texture texture = runtime_texture();
    const MaterialDescription invalid = runtime_material(RenderPass::opaque,
        "shader_type spatial; void fragment() { ALBEDO = unknown_runtime_identifier; }");
    const auto rejected = renderer_->upload(99, model, texture, invalid);
    runtime_failed_upload_clean_ = !rejected && renderer_->resources().empty();
    if (rejected || rejected.error().code != diagnostic_codes::shader_compile_failed
        || renderer_->diagnostics().empty()
        || renderer_->diagnostics().back().code != diagnostic_codes::shader_compile_failed
        || !runtime_failed_upload_clean_) {
        status_message_ = "runtime shader compilation rejection was not observed";
        return false;
    }

    if (shaders && shaders->stats().entries != 0) {
        status_message_ = "rejected source entered the shader cache";
        return false;
    }

    constexpr std::string_view opaque = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque;
void fragment() { ALBEDO = vec3(1.0, 0.0, 0.0); }
)GODOT";
    if (shaders) {
        // Valid source beyond the byte budget still uploads normally; it
        // must not grow setup retention, and its release must own the RID.
        const auto oversized = runtime_material(RenderPass::opaque,
            std::string(opaque) + "\n/*" + std::string(GodotShaderCache::source_byte_limit, 'x') + "*/");
        if (!renderer_->upload(88, model, texture, oversized)
            || shaders->stats().entries != 0 || !renderer_->release(88)
            || !renderer_->resources().empty()) {
            status_message_ = "shader byte budget fallback or release failed";
            return false;
        }
    }
    // A compilable source isolates the selector: only the out-of-contract
    // route or pass can cause these rejections. They are folded into the
    // existing registry-clean report field so the report format is unchanged.
    MaterialDescription unknown_route = runtime_material(RenderPass::opaque, opaque);
    unknown_route.route = static_cast<MaterialRoute>(0xFF);
    MaterialDescription unknown_pass = runtime_material(RenderPass::opaque, opaque);
    unknown_pass.pass = static_cast<RenderPass>(0xFF);
    runtime_failed_upload_clean_ = rejects_unknown_selector(*renderer_, 97, unknown_route,
                                       "material selected an unknown render route")
        && rejects_unknown_selector(*renderer_, 98, unknown_pass,
            "material selected an unknown render pass");
    if (!runtime_failed_upload_clean_) {
        status_message_ = "runtime unknown material route/pass rejection was not observed "
                          "or mutated the renderer registry";
        return false;
    }
    // Engine-output marker: the report field above predates these checks, so
    // package qualification requires this line to prove they ran.
    UtilityFunctions::print(
        "EAWR runtime: unknown material route and pass rejected; registry unchanged");

    // Each legacy rejection names the first selector outside the adapter
    // table. The disguised source is a canvas_item shader that only mentions
    // the spatial declaration in a comment: it would compile, so only the
    // leading-declaration rule can refuse it before a shader RID exists.
    MaterialDescription legacy = runtime_material(RenderPass::opaque, "MeshGloss.fx");
    legacy.route = MaterialRoute::legacy_effect;
    legacy.technique = "sph_t0";
    legacy.pass_name = "sph_t0_p0";
    MaterialDescription unknown_family = legacy;
    unknown_family.program = "UnknownEffect.fx";
    MaterialDescription unknown_technique = legacy;
    unknown_technique.technique = "sph_t9";
    MaterialDescription unknown_pass_name = legacy;
    unknown_pass_name.pass_name = "sph_t0_p9";
    MaterialDescription opaque_only = legacy;
    opaque_only.program = "BatchMeshGloss.fx";
    opaque_only.technique = "sph_t1";
    opaque_only.pass_name = "sph_t1_p0";
    opaque_only.pass = RenderPass::transparent;
    const MaterialDescription disguised = runtime_material(RenderPass::opaque,
        "shader_type canvas_item; // shader_type spatial;\n"
        "void fragment() { COLOR = vec4(1.0); }");
    runtime_failed_upload_clean_ = rejects_unknown_selector(*renderer_, 93, unknown_family,
                                       "unknown legacy material family 'UnknownEffect.fx'"
                                       " has no implemented Godot adapter")
        && rejects_unknown_selector(*renderer_, 94, unknown_technique,
            "legacy material family 'MeshGloss.fx' has no Godot adapter for technique"
            " 'sph_t9' (supported: 'sph_t0')")
        && rejects_unknown_selector(*renderer_, 95, unknown_pass_name,
            "legacy material family 'MeshGloss.fx' has no Godot adapter for pass"
            " 'sph_t0_p9' (supported: 'sph_t0_p0')")
        && rejects_unknown_selector(*renderer_, 96, opaque_only,
            "legacy material family 'BatchMeshGloss.fx' cannot draw in the transparent render pass")
        && rejects_unknown_selector(*renderer_, 92, disguised,
            "modern spatial shader must begin with 'shader_type spatial;'",
            diagnostic_codes::shader_compile_failed);
    if (!runtime_failed_upload_clean_) {
        status_message_ = "runtime legacy selector or non-spatial source rejection was not "
                          "observed or mutated the renderer registry";
        return false;
    }
    // The compile probe is anchored at the declaration, so a semicolon in a
    // leading comment no longer splits the source; the real compiler accepts it.
    const MaterialDescription comment_led = runtime_material(RenderPass::opaque,
        "// Leading comment; its semicolon precedes the declaration.\n"
        "shader_type spatial;\nrender_mode unshaded;\n"
        "void fragment() { ALBEDO = vec3(1.0); }\n");
    if (!renderer_->upload(91, model, texture, comment_led) || renderer_->resources().size() != 1
        || !renderer_->release(91) || !renderer_->resources().empty()) {
        status_message_ = "runtime comment-led spatial source did not compile and release cleanly";
        return false;
    }
    UtilityFunctions::print("EAWR runtime: unknown legacy family, technique, pass and render pass"
                            " and a non-spatial modern source rejected; comment-led spatial"
                            " source compiled; registry unchanged");
    constexpr std::string_view alpha_tested = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque;
void fragment() { ALBEDO = vec3(0.0, 1.0, 0.0); ALPHA = 1.0; ALPHA_SCISSOR_THRESHOLD = 0.5; }
)GODOT";
    constexpr std::string_view transparent = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never;
void fragment() { ALBEDO = vec3(0.0, 0.0, 1.0); ALPHA = 0.5; }
)GODOT";
    // hint_screen_texture is an engine-managed dependency on the frame drawn
    // by the opaque/alpha/transparent work before this post contribution.
    constexpr std::string_view post = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, blend_add, depth_draw_never;
uniform sampler2D previous_frame : hint_screen_texture;
void fragment() {
    float prior_red = texture(previous_frame, SCREEN_UV).r;
    ALBEDO = vec3(prior_red, 0.0, prior_red);
}
)GODOT";
    const std::array<MaterialDescription, 4> materials{
        runtime_material(RenderPass::opaque, opaque),
        runtime_material(RenderPass::alpha_tested, alpha_tested),
        runtime_material(RenderPass::transparent, transparent),
        runtime_material(RenderPass::post, post),
    };
    const std::array<assets::Model, 4> models{
        model, model, model, runtime_model(52.0F),
    };
    for (std::size_t index = 0; index < materials.size(); ++index) {
        const sim::AssetId asset_id = static_cast<sim::AssetId>(index + 1);
        const auto uploaded = renderer_->upload(asset_id, models[index], texture, materials[index]);
        if (!uploaded) {
            status_message_ = core::format_diagnostic(uploaded.error());
            return false;
        }
    }
    if (shaders) {
        const auto accepted = shaders->stats();
        auto wrong_binding = materials[0];
        wrong_binding.bindings.push_back({"undeclared_runtime_binding", 1.0F});
        const auto refused = renderer_->upload(89, model, texture, wrong_binding);
        if (refused || refused.error().code != diagnostic_codes::invalid_material
            || shaders->stats().entries != accepted.entries || renderer_->resources().size() != 4
            || !renderer_->upload(90, model, texture, materials[0])
            || shaders->stats().entries != accepted.entries || !renderer_->release(90)) {
            status_message_ = "cached shader skipped binding admission or release damaged a live shader";
            return false;
        }
        // Exact source keys must distinguish changes even when only a comment
        // differs. Overflow falls back, keeping retained shader work bounded.
        for (std::size_t index = 0; index < GodotShaderCache::entry_limit + 4; ++index) {
            const auto unique = runtime_material(RenderPass::opaque,
                std::string(opaque) + "\n// cache variant " + std::to_string(index));
            if (!renderer_->upload(91, model, texture, unique) || !renderer_->release(91)) {
                status_message_ = "shader entry budget fallback failed";
                return false;
            }
        }
        if (shaders->stats().entries != GodotShaderCache::entry_limit
            || shaders->stats().source_bytes > GodotShaderCache::source_byte_limit) {
            status_message_ = "shader retention exceeded its budget";
            return false;
        }
        UtilityFunctions::print("EAWR runtime: shader cache binding admission, byte and entry budgets passed");
    }
    // A second upload is a real shared reference to the same live RID bundle.
    if (!renderer_->upload(1, model, texture, materials[0])) {
        status_message_ = "runtime shared resource retain failed";
        return false;
    }
    const auto resources = renderer_->resources();
    if (resources.size() != 4 || resources.front().asset_id != 1 || resources.front().references != 2) {
        status_message_ = "runtime resource registry did not retain shared asset";
        return false;
    }

    // Five horizontal lanes make the observed result attributable: opaque
    // red, alpha-tested green over red, transparent blue, blue-over-red, and
    // post-over-red. The overlapping contributions sit slightly closer to
    // the camera so depth equality cannot decide the expected sample.
    const std::vector<sim::RenderInstance> full{
        {101, 1, runtime_transform(-440)},
        {102, 1, runtime_transform(-220)}, {103, 2, runtime_transform(-220, 2)},
        {104, 3, runtime_transform(0)},
        {105, 1, runtime_transform(220)}, {106, 3, runtime_transform(220, 2)},
        {107, 1, runtime_transform(440)}, {108, 4, runtime_transform(440, 2)},
    };
    snapshots_.push_back(std::make_shared<const sim::RenderSnapshot>(1, full));
    std::vector<sim::RenderInstance> missing = full;
    missing.front().asset_id = 404;
    runtime_lifecycle_snapshots_.push_back(
        std::make_shared<const sim::RenderSnapshot>(2, std::move(missing)));
    runtime_lifecycle_snapshots_.push_back(
        std::make_shared<const sim::RenderSnapshot>(3, std::vector<sim::RenderInstance>{}));
    std::vector<sim::RenderInstance> replaced = full;
    replaced.front().asset_id = 2;
    runtime_lifecycle_snapshots_.push_back(
        std::make_shared<const sim::RenderSnapshot>(4, std::move(replaced)));
    runtime_lifecycle_snapshots_.push_back(snapshots_.front());
    const std::string runtime_identity =
        "eawr-renderer-overlap-v2:opaque,alpha-tested,transparent,post:lifecycle-v1";
    scene_hash_ = hash_bytes(std::as_bytes(std::span(runtime_identity)));
    return true;
}

bool ViewerHost::verify_runtime_capture(const CaptureResult& capture) {
    PackedByteArray encoded;
    encoded.resize(static_cast<int64_t>(capture.png_bytes.size()));
    if (!capture.png_bytes.empty()) {
        std::memcpy(encoded.ptrw(), capture.png_bytes.data(), capture.png_bytes.size());
    }
    Ref<Image> image;
    image.instantiate();
    if (image->load_png_from_buffer(encoded) != OK || image->is_empty()) return false;
    std::array<std::array<std::size_t, 5>, 5> lane_colours{};
    constexpr std::array<float, 5> lane_centres{0.245F, 0.372F, 0.500F, 0.628F, 0.755F};
    for (int32_t y = 0; y < image->get_height(); y += 4) {
        for (int32_t x = 0; x < image->get_width(); x += 4) {
            const Color pixel = image->get_pixel(x, y);
            const float normalized_x = static_cast<float>(x)
                / static_cast<float>(image->get_width());
            std::size_t lane{};
            for (std::size_t candidate = 1; candidate < lane_centres.size(); ++candidate) {
                if (std::abs(normalized_x - lane_centres[candidate])
                    < std::abs(normalized_x - lane_centres[lane])) lane = candidate;
            }
            if (pixel.r > 0.55F && pixel.g < 0.25F && pixel.b < 0.25F) ++lane_colours[lane][0];
            if (pixel.g > 0.55F && pixel.r < 0.25F && pixel.b < 0.25F) ++lane_colours[lane][1];
            if (pixel.b > 0.40F && pixel.r < 0.25F && pixel.g < 0.25F) ++lane_colours[lane][2];
            if (pixel.r > 0.25F && pixel.b > 0.25F && pixel.g < 0.25F
                && std::abs(pixel.r - pixel.b) < 0.30F) ++lane_colours[lane][3];
            if (pixel.r > 0.55F && pixel.b > 0.55F && pixel.g < 0.30F) ++lane_colours[lane][4];
        }
    }
    const bool distinct = lane_colours[0][0] >= 4 && lane_colours[1][1] >= 4
        && lane_colours[2][2] >= 4;
    runtime_overlap_verified_ = lane_colours[3][3] >= 4;
    runtime_post_dependency_verified_ = lane_colours[4][4] >= 4
        && lane_colours[4][0] >= 4;
    return distinct && runtime_overlap_verified_ && runtime_post_dependency_verified_;
}

bool ViewerHost::verify_draw_capture(const CaptureResult& capture) {
    PackedByteArray encoded;
    encoded.resize(static_cast<int64_t>(capture.png_bytes.size()));
    if (!capture.png_bytes.empty()) {
        std::memcpy(encoded.ptrw(), capture.png_bytes.data(), capture.png_bytes.size());
    }
    Ref<Image> image;
    image.instantiate();
    if (image->load_png_from_buffer(encoded) != OK || image->is_empty()) return false;
    const Color background = image->get_pixel(0, 0);
    std::size_t distinguishable{};
    for (int32_t y = 0; y < image->get_height(); y += 2) {
        for (int32_t x = 0; x < image->get_width(); x += 2) {
            const Color pixel = image->get_pixel(x, y);
            const float difference = std::abs(pixel.r - background.r)
                + std::abs(pixel.g - background.g) + std::abs(pixel.b - background.b);
            if (difference > 0.04F) ++distinguishable;
        }
    }
    return distinguishable >= 16;
}

bool ViewerHost::verify_preview_capture(const CaptureResult& capture) {
    ModelPreview& preview = *model_preview_;
    preview.capture_verified = false;
    PackedByteArray encoded;
    encoded.resize(static_cast<int64_t>(capture.png_bytes.size()));
    if (!capture.png_bytes.empty()) {
        std::memcpy(encoded.ptrw(), capture.png_bytes.data(), capture.png_bytes.size());
    }
    Ref<Image> image;
    image.instantiate();
    if (image->load_png_from_buffer(encoded) != OK || image->is_empty()) return false;
    const int32_t width = image->get_width();
    const int32_t height = image->get_height();
    preview.capture_width = width;
    preview.capture_height = height;
    const Color background = image->get_pixel(0, 0);
    std::int64_t drawn{};
    std::int64_t min_x = width;
    std::int64_t min_y = height;
    std::int64_t max_x = -1;
    std::int64_t max_y = -1;
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const Color pixel = image->get_pixel(x, y);
            const float difference = std::abs(pixel.r - background.r)
                + std::abs(pixel.g - background.g) + std::abs(pixel.b - background.b);
            if (difference <= 0.04F) continue;
            ++drawn;
            min_x = std::min<std::int64_t>(min_x, x);
            min_y = std::min<std::int64_t>(min_y, y);
            max_x = std::max<std::int64_t>(max_x, x);
            max_y = std::max<std::int64_t>(max_y, y);
        }
    }
    preview.drawn_pixels = drawn;
    if (drawn == 0) return false;
    preview.drawn_min_x = min_x;
    preview.drawn_min_y = min_y;
    preview.drawn_max_x = max_x;
    preview.drawn_max_y = max_y;
    // The same distinguishable-draw floor as the frozen draw check, and the
    // whole Hull strictly inside the frame: a drawn edge row or column means
    // the bounds fit was clipped by the real viewport.
    preview.capture_verified = drawn >= 16 && min_x > 0 && min_y > 0
        && max_x < width - 1 && max_y < height - 1;
    return preview.capture_verified;
}
} // namespace eawr::presentation::godot_backend
