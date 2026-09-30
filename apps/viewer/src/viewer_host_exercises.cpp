#include "viewer_host_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {

[[nodiscard]] sim::math::Mat3x4 runtime_transform(
    const std::int64_t x, const std::int64_t z = 0) {
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

[[nodiscard]] assets::Model runtime_model(const float half_extent = 110.0F) {
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

[[nodiscard]] assets::Texture runtime_texture() {
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

[[nodiscard]] MaterialDescription runtime_material(
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

// ---------------------------------------------------------------------------
// Presentation-only mega-texture icon overlay support.
//
// The overlay samples an already-bound MegaTextureAtlas page rectangle into a
// screen-space canvas quad. It creates only canvas RIDs on the host viewport's
// World2D canvas; it never touches the 3D scenario, the material routes, or a
// renderer-owned resource registry.
// ---------------------------------------------------------------------------

constexpr std::int32_t atlas_overlay_margin = 16;
constexpr std::int32_t atlas_overlay_max_scale = 8;
// Evidence is sampled at source-texel centres under nearest filtering, so a
// drawn texel is an exact screen pixel. The transparent contract is strict;
// the opaque contract requires a bounded number of distinguishable texels
// because an icon may legitimately contain near-background colour.
constexpr std::int64_t atlas_overlay_min_opaque_evidence = 16;
constexpr float atlas_background_tolerance = 0.04F;

} // namespace

namespace viewer_host_detail {

[[nodiscard]] std::optional<vfs::Vfs> mount_content_layers(
    const std::filesystem::path& game_root,
    const std::filesystem::path& mod_root,
    const std::string_view profile,
    std::string& failure) {
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    if (profile != "" && profile != "eaw" && profile != "foc" && profile != "remake") {
        failure = "--eawr-profile must be eaw, foc or remake";
        return std::nullopt;
    }
    if (profile == "remake" && mod_root.empty()) {
        failure = "the Remake profile requires --eawr-mod-root";
        return std::nullopt;
    }
    if ((profile == "" && !mod_root.empty()) || profile == "remake") {
        for (const auto& layer : eawr::vfs::mod_chain_roots(mod_root)) roots.push_back(layer);
    }
    if (!game_root.empty()) {
        const std::filesystem::path expansion = game_root / "corruption" / "Data";
        const std::filesystem::path base = game_root / "GameData" / "Data";
        if (profile != "eaw" && std::filesystem::is_directory(expansion)) roots.emplace_back("expansion", expansion);
        if ((profile == "foc" || profile == "remake") && !std::filesystem::is_directory(expansion)) {
            failure = "the selected profile requires corruption/Data";
            return std::nullopt;
        }
        if (std::filesystem::is_directory(base)) roots.emplace_back("base", base);
    }
    if (roots.empty()) {
        failure = "atlas overlay requires --eawr-mod-root or a game root with Data layers";
        return std::nullopt;
    }
    std::vector<vfs::MountSpec> specs;
    auto chain = vfs::resolve_manifest_chain(roots);
    if (!chain) {
        failure = core::format_diagnostic(chain.error());
        return std::nullopt;
    }
    for (auto& manifest : chain.value()) specs.push_back(std::move(manifest.mount));
    auto filesystem = vfs::Vfs::mount(specs);
    if (!filesystem) {
        failure = core::format_diagnostic(filesystem.error());
        return std::nullopt;
    }
    return std::optional<vfs::Vfs>(std::move(filesystem.value()));
}

} // namespace viewer_host_detail

namespace {

// Decodes the bound page into a top-left oriented RGBA8 image. MTD rectangles
// are top-left, so a bottom-left TGA page is flipped exactly once here. That
// presentation step is not a per-entry flip; the format stores none.
[[nodiscard]] Ref<Image> atlas_page_image(const assets::Texture& page, std::string& failure) {
    if (page.mips.empty()) {
        failure = "atlas page decoded without a base mip level";
        return {};
    }
    const assets::MipLevel& mip = page.mips.front();
    Image::Format format = Image::FORMAT_MAX;
    std::uint32_t bytes_per_pixel{};
    switch (page.format) {
    case assets::PixelFormat::rgba8:
    case assets::PixelFormat::bgra8: format = Image::FORMAT_RGBA8; bytes_per_pixel = 4; break;
    case assets::PixelFormat::bgr8: format = Image::FORMAT_RGB8; bytes_per_pixel = 3; break;
    case assets::PixelFormat::l8: format = Image::FORMAT_L8; bytes_per_pixel = 1; break;
    case assets::PixelFormat::a8: format = Image::FORMAT_LA8; bytes_per_pixel = 1; break;
    case assets::PixelFormat::bc1: format = Image::FORMAT_DXT1; break;
    case assets::PixelFormat::bc2: format = Image::FORMAT_DXT3; break;
    case assets::PixelFormat::bc3: format = Image::FORMAT_DXT5; break;
    case assets::PixelFormat::bc4: format = Image::FORMAT_RGTC_R; break;
    case assets::PixelFormat::bc5: format = Image::FORMAT_RGTC_RG; break;
    case assets::PixelFormat::bc7: format = Image::FORMAT_BPTC_RGBA; break;
    }
    if (format == Image::FORMAT_MAX) {
        failure = "atlas page pixel format is unsupported by the overlay";
        return {};
    }
    if (bytes_per_pixel != 0
        && mip.row_pitch != static_cast<std::uint32_t>(page.width) * bytes_per_pixel) {
        failure = "atlas page base mip is not tightly packed";
        return {};
    }
    if (format == Image::FORMAT_LA8) {
        // The decoder reports one stored alpha byte per pixel; expand it to the
        // luminance/alpha pair Godot adopts without reinterpreting the payload.
        std::vector<std::byte> expanded(mip.bytes.size() * 2U, std::byte{0});
        for (std::size_t index = 0; index < mip.bytes.size(); ++index) {
            expanded[index * 2U + 1U] = mip.bytes[index];
        }
        PackedByteArray bytes;
        bytes.resize(static_cast<int64_t>(expanded.size()));
        std::memcpy(bytes.ptrw(), expanded.data(), expanded.size());
        Ref<Image> alpha_image = Image::create_from_data(static_cast<int32_t>(page.width),
            static_cast<int32_t>(page.height), false, Image::FORMAT_LA8, bytes);
        if (alpha_image.is_null() || alpha_image->is_empty()) {
            failure = "atlas page alpha-only payload could not be adopted";
            return {};
        }
        alpha_image->convert(Image::FORMAT_RGBA8);
        if (page.source_origin == assets::ImageOrigin::bottom_left) alpha_image->flip_y();
        return alpha_image;
    }
    PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(mip.bytes.size()));
    if (!mip.bytes.empty()) std::memcpy(bytes.ptrw(), mip.bytes.data(), mip.bytes.size());
    Ref<Image> image = Image::create_from_data(static_cast<int32_t>(page.width),
        static_cast<int32_t>(page.height), false, format, bytes);
    if (image.is_null() || image->is_empty()) {
        failure = "atlas page could not be adopted as a Godot image";
        return {};
    }
    if (image->is_compressed() && image->decompress() != OK) {
        failure = "atlas page block-compressed payload could not be decompressed";
        return {};
    }
    image->convert(Image::FORMAT_RGBA8);
    if (page.format == assets::PixelFormat::bgra8) {
        // The adopted bytes were BGRA; restore the documented channel order.
        for (int32_t y = 0; y < image->get_height(); ++y) {
            for (int32_t x = 0; x < image->get_width(); ++x) {
                const Color pixel = image->get_pixel(x, y);
                image->set_pixel(x, y, Color(pixel.b, pixel.g, pixel.r, pixel.a));
            }
        }
    }
    if (page.source_origin == assets::ImageOrigin::bottom_left) image->flip_y();
    return image;
}

// ---------------------------------------------------------------------------
// P1-09 tactical camera support.
//
// The camera model itself lives in eawr::presentation::camera and is pure. The
// viewer reads the two XML sources through the VFS, passes them to the pure
// constants loader, and applies the solved state to this run's capture camera.
// No constant is defaulted to a guessed original; a missing or unparseable
// required tag fails the run.
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<tactical_camera::Mode> parse_camera_mode(
    const std::string_view name) {
    if (ieq(name, "land")) return tactical_camera::Mode::land;
    if (ieq(name, "space")) return tactical_camera::Mode::space;
    if (ieq(name, "unlocked")) return tactical_camera::Mode::unlocked;
    return std::nullopt;
}

[[nodiscard]] std::string_view origin_name(const assets::ImageOrigin origin) {
    return origin == assets::ImageOrigin::top_left ? "top-left" : "bottom-left";
}

} // namespace

bool ViewerHost::start_renderer_runtime_exercise() {
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

    constexpr std::string_view opaque = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque;
void fragment() { ALBEDO = vec3(1.0, 0.0, 0.0); }
)GODOT";
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

bool ViewerHost::start_tactical_camera() {
    const auto mode = parse_camera_mode(options_->camera_mode);
    if (!mode) {
        status_message_ = "--eawr-camera-mode must be land, space or unlocked";
        return false;
    }
    if (options_->camera_zoom_requested && !std::isfinite(options_->camera_zoom)) {
        status_message_ = "--eawr-camera-zoom must be a finite number in [0, 1]";
        return false;
    }
    auto filesystem = mount_content_layers(
        options_->game_root, options_->mod_root, options_->profile, status_message_);
    if (!filesystem) return false;

    auto run = std::make_unique<TacticalCameraRun>();
    run->mode = *mode;
    run->camera_logical_path = "data/xml/tacticalcameras.xml";
    run->constants_logical_path = "data/xml/gameconstants.xml";

    auto camera_bytes = filesystem->open(run->camera_logical_path);
    if (!camera_bytes) {
        status_message_ = core::format_diagnostic(camera_bytes.error());
        return false;
    }
    auto constants_bytes = filesystem->open(run->constants_logical_path);
    if (!constants_bytes) {
        status_message_ = core::format_diagnostic(constants_bytes.error());
        return false;
    }
    run->camera_sha256 = hash_bytes(camera_bytes.value());
    run->constants_sha256 = hash_bytes(constants_bytes.value());

    auto loaded = tactical_camera::load_constants(
        {camera_bytes.value(), run->camera_logical_path, run->camera_sha256},
        {constants_bytes.value(), run->constants_logical_path, run->constants_sha256}, run->mode);
    if (!loaded) {
        status_message_ = core::format_diagnostic(loaded.error());
        return false;
    }
    run->constants = std::move(loaded.value().constants);
    if (auto valid = tactical_camera::validate(run->constants); !valid) {
        status_message_ = core::format_diagnostic(valid.error());
        return false;
    }

    // An unspecified zoom uses the mode's own Distance_Default, expressed back
    // in the normalised parameter, rather than an invented midpoint.
    float zoom{};
    if (options_->camera_zoom_requested) {
        auto clamped = tactical_camera::clamp_zoom(options_->camera_zoom);
        if (!clamped) {
            status_message_ = core::format_diagnostic(clamped.error());
            return false;
        }
        zoom = clamped.value();
    } else {
        const float span = run->constants.distance_max - run->constants.distance_min;
        zoom = std::clamp((run->constants.distance_default - run->constants.distance_min) / span,
            tactical_camera::min_zoom, tactical_camera::max_zoom);
    }
    auto solved = tactical_camera::solve(run->constants, zoom);
    if (!solved) {
        status_message_ = core::format_diagnostic(solved.error());
        return false;
    }
    run->state = solved.value();

    // Exercise the data-driven bindings once so the report records observed
    // mapping results rather than a restatement of the constants.
    auto stepped = tactical_camera::apply_zoom_step(run->constants, run->state.zoom, 1.0F);
    auto pan = tactical_camera::apply_pan(
        run->constants, run->state, {1.0F, 0.0F, false}, 1.0F);
    auto push = tactical_camera::apply_pan(
        run->constants, run->state, {1.0F, 0.0F, true}, 1.0F);
    auto left_edge = tactical_camera::edge_scroll(run->constants, 0.0F, 360.0F, 1280.0F, 720.0F);
    auto right_edge =
        tactical_camera::edge_scroll(run->constants, 1279.0F, 360.0F, 1280.0F, 720.0F);
    auto yaw = tactical_camera::apply_rotate(run->constants, run->state.yaw_degrees, 10.0F);
    auto pitch = tactical_camera::apply_pitch(run->constants, run->state.pitch_degrees, 10.0F);
    if (!stepped || !pan || !push || !left_edge || !right_edge || !yaw || !pitch) {
        status_message_ = "tactical camera input mapping rejected a nominal binding";
        return false;
    }
    run->zoom_step = stepped.value();
    run->pan_x_per_second = pan.value().x;
    run->push_pan_x_per_second = push.value().x;
    run->edge_scroll_left = left_edge.value().axis_x;
    run->edge_scroll_right = right_edge.value().axis_x;
    run->yaw_after_drag = yaw.value();
    run->pitch_after_drag = pitch.value();

    const std::array<float, 3> target{0.0F, 0.0F, 0.0F};
    auto eye = tactical_camera::eye_position(std::span<const float, 3>{target},
        run->state.distance, run->state.pitch_degrees, run->state.yaw_degrees);
    if (!eye) {
        status_message_ = core::format_diagnostic(eye.error());
        return false;
    }
    run->eye = eye.value();

    // Apply the solved model to this run's capture camera. The frozen P1-01
    // fixed-capture framing is a different code path and is not touched here.
    capture_camera_.eye = run->eye;
    capture_camera_.target = target;
    capture_camera_.up = {0.0F, 1.0F, 0.0F};
    // #515: the XML angle is FoC's (horizontal on 4:3); the renderer takes the vertical one.
    auto vertical = tactical_camera::vertical_fov_degrees(run->state.fov_degrees);
    if (!vertical) {
        status_message_ = core::format_diagnostic(vertical.error());
        return false;
    }
    capture_camera_.vertical_fov_degrees = vertical.value();
    capture_camera_.near_plane = run->constants.near_clip;
    capture_camera_.far_plane = run->constants.far_clip;

    // A single unlit triangle sized from the solved distance gives the capture
    // something to frame, so the graphical run proves the camera reached the
    // engine rather than only that the arithmetic ran.
    constexpr std::string_view marker = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque;
void fragment() { ALBEDO = vec3(0.0, 1.0, 0.4); }
)GODOT";
    const assets::Model model = runtime_model(run->state.distance * 0.25F);
    const auto uploaded = renderer_->upload(
        1, model, runtime_texture(), runtime_material(RenderPass::opaque, marker));
    if (!uploaded) {
        status_message_ = core::format_diagnostic(uploaded.error());
        return false;
    }
    snapshots_.push_back(std::make_shared<const sim::RenderSnapshot>(
        1, std::vector<sim::RenderInstance>{{1, 1, runtime_transform(0)}}));
    renderer_->set_camera(capture_camera_);
    const std::string identity = std::string("eawr-tactical-camera-v1:")
        + std::string(tactical_camera::to_string(run->mode));
    scene_hash_ = hash_bytes(std::as_bytes(std::span(identity)));
    tactical_camera_ = std::move(run);
    return true;
}

bool ViewerHost::start_atlas_overlay() {
    if (options_->icon_name.empty()) {
        status_message_ = "--eawr-atlas requires --eawr-icon to name the drawn entry";
        return false;
    }
    auto filesystem = mount_content_layers(
        options_->game_root, options_->mod_root, options_->profile, status_message_);
    if (!filesystem) return false;
    auto mtd_bytes = filesystem->open(options_->atlas_path);
    if (!mtd_bytes) {
        status_message_ = core::format_diagnostic(mtd_bytes.error());
        return false;
    }
    auto bound = assets::load_mega_texture_atlas(*filesystem, options_->atlas_path);
    if (!bound) {
        status_message_ = core::format_diagnostic(bound.error());
        return false;
    }
    const assets::MegaTextureAtlas& atlas = bound.value();
    const assets::MegaTextureEntry* const entry = atlas.directory.find(options_->icon_name);
    if (!entry) {
        status_message_ = "requested icon is not present in the mega texture directory";
        return false;
    }
    if (entry->rectangle.width == 0 || entry->rectangle.height == 0) {
        status_message_ = "requested icon has an empty atlas rectangle";
        return false;
    }
    auto page_bytes = filesystem->open(atlas.page.source.logical_path);
    if (!page_bytes) {
        status_message_ = core::format_diagnostic(page_bytes.error());
        return false;
    }

    auto overlay = std::make_unique<AtlasOverlay>();
    overlay->mtd_logical_path = atlas.directory.source.logical_path;
    overlay->mtd_sha256 = hash_bytes(mtd_bytes.value());
    overlay->page_logical_path = atlas.page.source.logical_path;
    overlay->page_sha256 = hash_bytes(page_bytes.value());
    overlay->icon_name = entry->name;
    overlay->rectangle = entry->rectangle;
    overlay->icon_has_alpha = entry->has_alpha;
    overlay->icon_flip_x = entry->flip_x;
    overlay->icon_flip_y = entry->flip_y;
    overlay->page_width = atlas.page.width;
    overlay->page_height = atlas.page.height;
    overlay->page_format = assets::to_string(atlas.page.format);
    overlay->page_origin = origin_name(atlas.page.source_origin);
    overlay->page_image = atlas_page_image(atlas.page, status_message_);
    if (overlay->page_image.is_null()) return false;

    RenderingServer* rendering = RenderingServer::get_singleton();
    Viewport* const viewport = get_viewport();
    if (!rendering || viewport == nullptr) {
        status_message_ = "Godot RenderingServer or viewport is unavailable for the overlay";
        return false;
    }
    const Ref<World2D> world = viewport->find_world_2d();
    if (world.is_null()) {
        status_message_ = "host viewport exposes no World2D canvas for the overlay";
        return false;
    }
    const Rect2 visible = viewport->get_visible_rect();
    const auto visible_width = static_cast<std::int32_t>(visible.size.width);
    const auto visible_height = static_cast<std::int32_t>(visible.size.height);
    if (visible_width <= 0 || visible_height <= 0) {
        status_message_ = "host viewport reported an empty visible rectangle";
        return false;
    }
    overlay->scale = atlas_overlay_max_scale;
    const auto rect_width = static_cast<std::int32_t>(overlay->rectangle.width);
    const auto rect_height = static_cast<std::int32_t>(overlay->rectangle.height);
    while (overlay->scale > 1
        && (rect_width * overlay->scale > visible_width / 2
            || rect_height * overlay->scale > visible_height / 2)) {
        --overlay->scale;
    }
    overlay->quad_width = std::min(rect_width * overlay->scale, visible_width);
    overlay->quad_height = std::min(rect_height * overlay->scale, visible_height);
    overlay->quad_x = std::min(atlas_overlay_margin, visible_width - overlay->quad_width);
    overlay->quad_y = std::min(atlas_overlay_margin, visible_height - overlay->quad_height);

    overlay->texture = rendering->texture_2d_create(overlay->page_image);
    if (!overlay->texture.is_valid()) {
        status_message_ = "atlas page could not be uploaded as a RenderingServer texture";
        return false;
    }
    overlay->canvas_item = rendering->canvas_item_create();
    if (!overlay->canvas_item.is_valid()) {
        status_message_ = "overlay canvas item could not be created";
        return false;
    }
    rendering->canvas_item_set_parent(overlay->canvas_item, world->get_canvas());
    // Nearest filtering keeps every drawn screen pixel an exact copy of the
    // source texel, so the alpha evidence below is a sampling fact rather than
    // a tolerance against interpolated edges.
    rendering->canvas_item_set_default_texture_filter(
        overlay->canvas_item, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_NEAREST);
    rendering->canvas_item_add_texture_rect_region(
        overlay->canvas_item,
        Rect2(static_cast<real_t>(overlay->quad_x), static_cast<real_t>(overlay->quad_y),
            static_cast<real_t>(overlay->quad_width), static_cast<real_t>(overlay->quad_height)),
        overlay->texture,
        Rect2(static_cast<real_t>(overlay->rectangle.x), static_cast<real_t>(overlay->rectangle.y),
            static_cast<real_t>(overlay->rectangle.width),
            static_cast<real_t>(overlay->rectangle.height)),
        Color(1.0F, 1.0F, 1.0F, 1.0F), false, false);
    atlas_overlay_ = std::move(overlay);
    return true;
}

bool ViewerHost::verify_atlas_capture(const CaptureResult& capture) {
    if (!atlas_overlay_) return false;
    PackedByteArray encoded;
    encoded.resize(static_cast<int64_t>(capture.png_bytes.size()));
    if (!capture.png_bytes.empty()) {
        std::memcpy(encoded.ptrw(), capture.png_bytes.data(), capture.png_bytes.size());
    }
    Ref<Image> image;
    image.instantiate();
    if (image->load_png_from_buffer(encoded) != OK || image->is_empty()) return false;
    Viewport* const viewport = get_viewport();
    if (viewport == nullptr) return false;
    const Rect2 visible = viewport->get_visible_rect();
    if (visible.size.width <= 0.0F || visible.size.height <= 0.0F) return false;
    // The capture is the host viewport texture; map canvas pixels onto it so a
    // scaled or high-DPI backing store still addresses the same quad.
    const float to_image_x = static_cast<float>(image->get_width()) / visible.size.width;
    const float to_image_y = static_cast<float>(image->get_height()) / visible.size.height;
    const Color background = image->get_pixel(image->get_width() - 1, image->get_height() - 1);

    // Each distinct opaque source colour must map to one stable drawn colour,
    // and distinct source colours must stay distinguishable once drawn. A
    // flipped, transposed or offset sampling breaks that mapping even though
    // it would still cover the same quad, so this is the orientation evidence
    // the asymmetric fixture asks for.
    struct DrawnColourSpread final {
        float min_red{1.0F}, max_red{0.0F};
        float min_green{1.0F}, max_green{0.0F};
        float min_blue{1.0F}, max_blue{0.0F};
    };
    std::map<std::array<std::uint8_t, 3>, DrawnColourSpread> drawn_by_source;

    AtlasOverlay& overlay = *atlas_overlay_;
    overlay.opaque_sampled = 0;
    overlay.opaque_non_background = 0;
    overlay.transparent_sampled = 0;
    overlay.transparent_background = 0;
    const auto rect_width = static_cast<std::int32_t>(overlay.rectangle.width);
    const auto rect_height = static_cast<std::int32_t>(overlay.rectangle.height);
    for (std::int32_t row = 0; row < rect_height; ++row) {
        for (std::int32_t column = 0; column < rect_width; ++column) {
            const Color source = overlay.page_image->get_pixel(
                static_cast<std::int32_t>(overlay.rectangle.x) + column,
                static_cast<std::int32_t>(overlay.rectangle.y) + row);
            const float centre_x = static_cast<float>(overlay.quad_x)
                + (static_cast<float>(column) + 0.5F)
                    * static_cast<float>(overlay.quad_width) / static_cast<float>(rect_width);
            const float centre_y = static_cast<float>(overlay.quad_y)
                + (static_cast<float>(row) + 0.5F)
                    * static_cast<float>(overlay.quad_height) / static_cast<float>(rect_height);
            const std::int32_t pixel_x = std::clamp(
                static_cast<std::int32_t>(centre_x * to_image_x), 0, image->get_width() - 1);
            const std::int32_t pixel_y = std::clamp(
                static_cast<std::int32_t>(centre_y * to_image_y), 0, image->get_height() - 1);
            const Color pixel = image->get_pixel(pixel_x, pixel_y);
            const float difference = std::abs(pixel.r - background.r)
                + std::abs(pixel.g - background.g) + std::abs(pixel.b - background.b);
            if (source.a <= 0.0F) {
                ++overlay.transparent_sampled;
                if (difference <= atlas_background_tolerance) ++overlay.transparent_background;
            } else if (source.a >= 1.0F) {
                ++overlay.opaque_sampled;
                if (difference > atlas_background_tolerance) ++overlay.opaque_non_background;
                const auto quantize = [](const float channel) {
                    return static_cast<std::uint8_t>(
                        std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F));
                };
                DrawnColourSpread& spread = drawn_by_source[{
                    quantize(source.r), quantize(source.g), quantize(source.b)}];
                spread.min_red = std::min(spread.min_red, pixel.r);
                spread.max_red = std::max(spread.max_red, pixel.r);
                spread.min_green = std::min(spread.min_green, pixel.g);
                spread.max_green = std::max(spread.max_green, pixel.g);
                spread.min_blue = std::min(spread.min_blue, pixel.b);
                spread.max_blue = std::max(spread.max_blue, pixel.b);
            }
        }
    }
    overlay.distinct_opaque_colours = static_cast<std::int64_t>(drawn_by_source.size());
    overlay.orientation_consistent = true;
    std::vector<std::array<float, 3>> drawn_centres;
    drawn_centres.reserve(drawn_by_source.size());
    for (const auto& [source_colour, spread] : drawn_by_source) {
        static_cast<void>(source_colour);
        if (spread.max_red - spread.min_red > atlas_background_tolerance
            || spread.max_green - spread.min_green > atlas_background_tolerance
            || spread.max_blue - spread.min_blue > atlas_background_tolerance) {
            overlay.orientation_consistent = false;
        }
        drawn_centres.push_back({(spread.min_red + spread.max_red) * 0.5F,
            (spread.min_green + spread.max_green) * 0.5F,
            (spread.min_blue + spread.max_blue) * 0.5F});
    }
    for (std::size_t left = 0; left + 1 < drawn_centres.size(); ++left) {
        for (std::size_t right = left + 1; right < drawn_centres.size(); ++right) {
            const float separation = std::abs(drawn_centres[left][0] - drawn_centres[right][0])
                + std::abs(drawn_centres[left][1] - drawn_centres[right][1])
                + std::abs(drawn_centres[left][2] - drawn_centres[right][2]);
            if (separation <= atlas_background_tolerance) overlay.orientation_consistent = false;
        }
    }

    const bool alpha_honoured = overlay.transparent_background == overlay.transparent_sampled;
    const bool drew_icon = overlay.opaque_non_background > 0
        && overlay.opaque_non_background
            >= std::min<std::int64_t>(atlas_overlay_min_opaque_evidence, overlay.opaque_sampled);
    return alpha_honoured && drew_icon && overlay.orientation_consistent;
}

void ViewerHost::release_atlas_overlay() {
    if (atlas_overlay_) atlas_overlay_->free_rids();
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
