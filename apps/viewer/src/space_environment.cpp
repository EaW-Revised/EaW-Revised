#include "space_environment.hpp"
#include "space_environment_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {

// Clean-room modern_spatial adapter for eawr-space-primary-sky-v1. It is a
// diffuse preview: it samples one BaseTexture per surface and nothing else,
// which is exactly what the plan admits. It is not the SKYDOME, PLANET or
// NEBULA effect. `cull_disabled, depth_draw_never` is the existing preview
// policy for a sky seen from inside; it is not a culling optimisation.
constexpr std::string_view sky_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never;
uniform sampler2D eawr_diffuse : filter_linear_mipmap, repeat_enable;
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb;
}
)GODOT";

// meshgloss-light-probe: a labelled control, never the shipped sky policy. A
// constant per-channel irradiance (only the homogeneous term) makes Diffuse
// measurable, and an unnormalised light-zero direction along render -Z (the
// effect does not normalise it) makes Specular measurable on a surface whose
// normal faces render -Z. Values are invented test inputs.
[[nodiscard]] space::SkyLightPolicy light_probe_policy() {
    space::SkyLightPolicy policy;
    policy.id = "meshgloss-light-probe-control";
    policy.sph[0][15] = 0.5F;
    policy.sph[1][15] = 0.25F;
    policy.sph[2][15] = 0.125F;
    policy.light_direction = {0.0F, 0.0F, -3.0F};
    policy.light_specular = {1.0F, 1.0F, 1.0F};
    policy.light_scale = {1.0F, 1.0F, 1.0F, 1.0F};
    policy.cause = "labelled control only: invented constant irradiance (0.5, 0.25, 0.125) and light-zero specular "
                   "(1, 1, 1) along render -Z (0, 0, -3), in the render basis, to prove the Diffuse and Specular "
                   "bindings reach the GPU; not a space light rig";
    return policy;
}

[[nodiscard]] std::optional<float> parse_float(const std::string_view text) {
    if (text.empty()) return std::nullopt;
    const std::string copy(text);
    char* end = nullptr;
    const float value = std::strtof(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}

[[nodiscard]] AdditiveControl parse_additive_control(const std::string_view control) {
    AdditiveControl result;
    std::vector<std::string_view> tokens;
    for (std::size_t start = 0; start < control.size();) {
        const std::size_t end = std::min(control.find(' ', start), control.size());
        if (end > start) tokens.push_back(control.substr(start, end - start));
        start = end + 1;
    }
    if (tokens.empty() || tokens.front() != meshadditive_control) return result;
    result.enabled = true;
    bool timed = false;
    bool scaled = false;
    for (std::size_t index = 1; index < tokens.size() && result.failure.empty(); ++index) {
        const std::string_view token = tokens[index];
        if (token == "occluder" && result.base == "none") {
            result.base = "occluder";
        } else if (token.starts_with("time=") && !timed) {
            timed = true;
            const auto value = parse_float(token.substr(5));
            if (!value) result.failure = "time= needs one finite number of seconds";
            else result.inputs.time = *value;
        } else if (token.starts_with("light-scale=") && !scaled) {
            scaled = true;
            std::array<float, 4> values{};
            std::size_t count = 0;
            bool valid = true;
            const std::string_view rest = token.substr(12);
            for (std::size_t first = 0; valid && first <= rest.size();) {
                const std::size_t comma = std::min(rest.find(',', first), rest.size());
                const auto value = parse_float(rest.substr(first, comma - first));
                if (!value || count == values.size()) valid = false;
                else values[count++] = *value;
                first = comma + 1;
            }
            if (!valid || count != values.size()) {
                result.failure = "light-scale= needs four comma-separated finite numbers";
            } else {
                result.inputs.light_scale = {values[0], values[1], values[2], values[3]};
            }
        } else {
            result.failure = "unknown or repeated token '" + std::string(token) + "'";
        }
    }
    if (timed || scaled) {
        result.inputs.id = "meshadditive-synthetic-inputs-control";
        result.inputs.cause = "labelled control only: invented TIME and LIGHT_SCALE test inputs that make the "
                              "UVScrollRate and light-scale bindings measurable; not the retail clock (G-04) or "
                              "sky light scale (G-05)";
    }
    if (result.failure.empty() && !space::finite_inputs(result.inputs)) result.failure = "inputs must be finite";
    return result;
}

// Negative control: passes the structural preflight, then must be rejected by
// Godot's own compiler (an undeclared identifier).
constexpr std::string_view broken_sky_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never;
uniform sampler2D eawr_diffuse : filter_linear_mipmap, repeat_enable;
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb * eawr_undeclared_control_symbol;
}
)GODOT";

// Foreground depth-occlusion control geometry. It writes depth like any
// opaque object; it is labelled as a control and never counted as sky.
constexpr std::string_view occluder_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled;
uniform sampler2D eawr_diffuse : filter_nearest, repeat_enable;
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb;
}
)GODOT";

// Sky-shadow caster controls only; never the sky preview's material. The
// shipped single-sided sky, seen from inside, turns its back face to a light
// behind it, and even with its cast flag enabled it leaves the lit receiver
// unchanged (observed on the pinned Godot backend). A positive control needs a
// surface that demonstrably casts: this variant writes depth and is uploaded
// with both windings (the surface's own geometry and UVs), so one copy faces
// the camera and the other faces the light.
constexpr std::string_view caster_sky_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_back;
uniform sampler2D eawr_diffuse : filter_linear_mipmap, repeat_enable;
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb;
}
)GODOT";

// Lit receiver of the sky-shadow controls. Unlike the sky it takes Godot's
// directional light and shadow; it is a labelled control and never sky.
// cull_back with both windings uploaded draws exactly the camera-facing copy
// with its authored normal, so the result never depends on which way the
// quad happens to be wound.
constexpr std::string_view receiver_shader = R"GODOT(
shader_type spatial;
render_mode cull_back, ambient_light_disabled;
uniform sampler2D eawr_diffuse : filter_nearest, repeat_enable;
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb;
}
void light() {
    DIFFUSE_LIGHT += vec3(ATTENUATION);
}
)GODOT";

// Live create/destroy: each cycle draws the composed sky, releases every sky
// asset while its instances exist, then uploads again.
constexpr std::size_t lifecycle_cycles = 3;
constexpr std::uint32_t lifecycle_cycle_frames = 2;

[[nodiscard]] sim::math::Mat3x4 identity_transform() {
    using Fixed = sim::math::Fixed;
    const Fixed zero = Fixed::from_raw(0);
    const Fixed one = Fixed::from_raw(Fixed::scale);
    sim::math::Mat3x4 result{};
    result.rows[0] = {one, zero, zero, zero};
    result.rows[1] = {zero, one, zero, zero};
    result.rows[2] = {zero, zero, one, zero};
    return result;
}

[[nodiscard]] Ref<Image> decode_png(const std::vector<std::byte>& bytes) {
    PackedByteArray encoded;
    encoded.resize(static_cast<int64_t>(bytes.size()));
    if (!bytes.empty()) std::memcpy(encoded.ptrw(), bytes.data(), bytes.size());
    Ref<Image> image;
    image.instantiate();
    if (image->load_png_from_buffer(encoded) != OK || image->is_empty()) return {};
    return image;
}

[[nodiscard]] std::array<float, 3> centroid(const std::vector<space::Triangle>& triangles) {
    std::array<double, 3> sum{};
    std::size_t count = 0;
    for (const space::Triangle& triangle : triangles) {
        for (const assets::Vec3f& corner : triangle) {
            sum[0] += corner.x;
            sum[1] += corner.y;
            sum[2] += corner.z;
            ++count;
        }
    }
    if (count == 0) return {};
    return {static_cast<float>(sum[0] / static_cast<double>(count)),
            static_cast<float>(sum[1] / static_cast<double>(count)),
            static_cast<float>(sum[2] / static_cast<double>(count))};
}

[[nodiscard]] std::filesystem::path sibling(const std::filesystem::path& capture, const std::string& suffix) {
    std::filesystem::path path = capture;
    path.replace_extension(std::filesystem::path(suffix));
    return path;
}

[[nodiscard]] bool orbit_focus_at_centre(const tactical::TacticalFrame& frame,
    const std::array<float, 3>& focus, const float radius) {
    const Vector3 eye(frame.eye[0], frame.eye[1], frame.eye[2]);
    const Vector3 target(frame.target[0], frame.target[1], frame.target[2]);
    const Vector3 point(focus[0], focus[1], focus[2]);
    const Vector3 up(frame.up[0], frame.up[1], frame.up[2]);
    Transform3D view;
    view.origin = eye;
    view = view.looking_at(target, up);
    const Vector3 local = view.affine_inverse().xform(point);
    return point.distance_to(target) < 0.01F && std::abs(local.x) < 0.01F
        && std::abs(local.y) < 0.01F && local.z < 0.0F
        && std::abs(eye.distance_to(point) - radius) < 0.01F;
}

// Synthetic self-test events enter Godot's own input pipeline, so they reach
// the camera through the host's real _input callback like a device event.
void inject_key(const Key code, const bool pressed) {
    Ref<InputEventKey> event;
    event.instantiate();
    event->set_keycode(code);
    event->set_physical_keycode(code);
    event->set_pressed(pressed);
    Input::get_singleton()->parse_input_event(event);
}

// The Ctrl key's own events, as the platform sends them around a Ctrl gesture: the press and
// its auto-repeat carry the Ctrl bit, the release does not.
void inject_ctrl(const bool pressed, const bool echo = false) {
    Ref<InputEventKey> event;
    event.instantiate();
    event->set_keycode(KEY_CTRL);
    event->set_physical_keycode(KEY_CTRL);
    event->set_pressed(pressed);
    event->set_echo(echo);
    event->set_ctrl_pressed(pressed);
    Input::get_singleton()->parse_input_event(event);
}

void inject_button(const MouseButton button, const Vector2 position, const bool pressed = true,
                   const bool ctrl = false) {
    Ref<InputEventMouseButton> event;
    event.instantiate();
    event->set_button_index(button);
    event->set_pressed(pressed);
    event->set_factor(1.0F);
    event->set_position(position);
    event->set_ctrl_pressed(ctrl);
    Input::get_singleton()->parse_input_event(event);
}

void inject_motion(const Vector2 position, const Vector2 relative, const bool alt = false,
                   const bool ctrl = false) {
    Ref<InputEventMouseMotion> event;
    event.instantiate();
    event->set_position(position);
    event->set_relative(relative);
    event->set_alt_pressed(alt);
    event->set_ctrl_pressed(ctrl);
    Input::get_singleton()->parse_input_event(event);
}
} // namespace

namespace space_environment_detail {

[[nodiscard]] std::vector<MaterialBinding> meshgloss_bindings(
    const space::MeshGlossMaterial& material, const space::SkyLightPolicy& policy) {
    std::vector<MaterialBinding> bindings{
        {"BaseTexture", std::string{"space-sky-base-texture"}},
        {"Emissive", material.emissive},
        {"Diffuse", material.diffuse},
        {"Specular", material.specular},
    };
    constexpr std::array<char, 3> channels{'r', 'g', 'b'};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto& matrix = policy.sph[channel];
        for (std::size_t column = 0; column < 4; ++column) {
            bindings.push_back({"eawr_sky_sph_" + std::string(1, channels[channel]) + std::to_string(column),
                assets::Vec4f{matrix[column * 4], matrix[column * 4 + 1], matrix[column * 4 + 2], matrix[column * 4 + 3]}});
        }
    }
    bindings.push_back({"eawr_sky_light_direction", policy.light_direction});
    bindings.push_back({"eawr_sky_light_specular", policy.light_specular});
    bindings.push_back({"eawr_sky_light_scale", policy.light_scale});
    return bindings;
}

[[nodiscard]] std::vector<MaterialBinding> meshadditive_bindings(
    const space::MeshAdditiveMaterial& material, const space::MeshAdditiveInputs& inputs) {
    return {
        {"BaseTexture", std::string{"space-sky-base-texture"}},
        {"Color", material.color},
        {"UVScrollRate", material.uv_scroll_rate},
        {"eawr_sky_time", inputs.time},
        {"eawr_sky_light_scale", inputs.light_scale},
    };
}

// sky-shadow: the shipped sky, flag off (the policy under test).
// sky-shadow-cast: the shipped sky, flag on.
// sky-shadow-caster: the depth-writing caster variant, flag off.
// sky-shadow-caster-cast: the caster variant, flag on (positive control).
[[nodiscard]] bool shadow_control(const std::string_view control) {
    return control == "sky-shadow" || control == "sky-shadow-cast" || control == "sky-shadow-caster"
        || control == "sky-shadow-caster-cast";
}
[[nodiscard]] bool caster_control(const std::string_view control) {
    return control == "sky-shadow-caster" || control == "sky-shadow-caster-cast";
}

[[nodiscard]] std::string hash_bytes(const std::span<const std::byte> bytes) {
    return sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

// The map mode's reference rule: probe the authored name as written, then by
// stem with every known suffix of the kind. The winner is the effective VFS
// record, whose source and layer are retained by the loaders.
[[nodiscard]] std::optional<std::string> probe_reference(
    const vfs::Vfs& filesystem, const std::string_view root,
    const std::string_view name, const std::span<const std::string_view> suffixes) {
    std::string canonical;
    canonical.reserve(name.size());
    for (const char character : name) {
        const char folded = character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A')) : character;
        canonical.push_back(folded == '\\' ? '/' : folded);
    }
    if (canonical.empty()) return std::nullopt;
    const std::string base = std::string(root) + canonical;
    if (filesystem.stat(base)) return base;
    std::string stem = canonical;
    for (const std::string_view suffix : suffixes) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            stem.resize(stem.size() - suffix.size());
            break;
        }
    }
    for (const std::string_view suffix : suffixes) {
        const std::string candidate = std::string(root) + stem + std::string(suffix);
        if (filesystem.stat(candidate)) return candidate;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<space::Rgb8Image> rgb_of(const std::vector<std::byte>& png) {
    const Ref<Image> image = decode_png(png);
    if (image.is_null()) return std::nullopt;
    image->convert(Image::FORMAT_RGB8);
    const PackedByteArray data = image->get_data();
    space::Rgb8Image result;
    result.width = static_cast<std::uint32_t>(image->get_width());
    result.height = static_cast<std::uint32_t>(image->get_height());
    result.rgb.resize(static_cast<std::size_t>(data.size()));
    if (!result.rgb.empty()) std::memcpy(result.rgb.data(), data.ptr(), result.rgb.size());
    if (result.rgb.size() != static_cast<std::size_t>(result.width) * result.height * 3U) return std::nullopt;
    return result;
}

// Empty on success; otherwise why the requested artifact was not persisted.
[[nodiscard]] std::string write_file(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return "cannot create its parent directory (" + error.message() + ")";
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return "cannot be opened for writing";
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) return "write or close failed";
    return {};
}

[[nodiscard]] tactical::TacticalFrame tactical_frame(const FixedCamera& source) {
    return {source.width, source.height, source.vertical_fov_degrees,
        source.near_plane, source.far_plane, source.eye, source.target, source.up};
}

[[nodiscard]] FixedCamera fixed_camera(const tactical::TacticalFrame& source) {
    FixedCamera result;
    result.width = source.width;
    result.height = source.height;
    result.vertical_fov_degrees = source.vertical_fov_degrees;
    result.near_plane = source.near_plane;
    result.far_plane = source.far_plane;
    result.eye = source.eye;
    result.target = source.target;
    result.up = source.up;
    return result;
}
} // namespace space_environment_detail

void SpaceEnvironment::State::release_all() {
    if (!renderer) return;
    if (options.fog) {
        // Observed before release so the report keeps the bound state.
        if (!fog_final) fog_final = renderer->fog_status();
        if (fog_consumers.empty()) fog_consumers = renderer->fog_consumers();
        if (fog_units) fog_units->release(*renderer);
        renderer->disable_fog();
    }
    for (const Uploaded& item : uploaded) static_cast<void>(renderer->release(item.asset));
    if (occluder_uploaded) static_cast<void>(renderer->release(occluder_asset));
    occluder_uploaded = false;
    teardown_remaining = renderer->resources().size();
    teardown_instances = renderer->instance_count();
}

bool SpaceEnvironment::State::upload_all(const std::string& control_name) {
    uploaded.clear();
    sim::AssetId next_asset = 1;
    std::size_t accepted_seen = 0;
    const std::size_t accepted_total = plan.accepted_count();
    for (std::size_t index = 0; index < plan.surfaces.size(); ++index) {
        const space::SurfacePlan& surface = plan.surfaces[index];
        if (surface.status != space::SurfaceStatus::accepted || !surface.model || !surface.texture) continue;
        ++accepted_seen;
        // The broken-shader control breaks only the last surface, so earlier
        // uploads exist and the partial-failure release path is exercised.
        const bool broken = control_name == "broken-shader" && accepted_seen == accepted_total;
        // A MeshGloss surface draws through its own adapter with its authored
        // values; the broken-shader and caster controls keep their labelled
        // diffuse programs. The opaque pass requires light-scale alpha 1.
        const bool gloss = surface.material == space::SkyMaterial::meshgloss && !broken
            && !caster_control(control_name);
        if (gloss && (!surface.meshgloss || sky_light.light_scale.w != 1.0F)) {
            material_status = "upload_failed";
            upload_failure[index] = "a MeshGloss sky surface needs its authored values and light-scale alpha 1";
            release_all();
            uploaded.clear();
            return false;
        }
        // A MeshAdditive surface exists only under the labelled control, which
        // never combines with the broken-shader or caster controls.
        const bool additive_surface = surface.material == space::SkyMaterial::meshadditive;
        if (additive_surface && (!additive.enabled || !surface.meshadditive || broken || caster_control(control_name)
                                 || !space::finite_inputs(additive.inputs))) {
            material_status = "upload_failed";
            upload_failure[index] = "a MeshAdditive sky surface needs the meshadditive-synthetic control, its authored "
                                    "values and finite declared inputs";
            release_all();
            uploaded.clear();
            return false;
        }
        std::string_view program = sky_shader;
        if (broken) program = broken_sky_shader;
        else if (caster_control(control_name)) program = caster_sky_shader;
        else if (gloss) program = meshgloss_sky_shader;
        else if (additive_surface) program = meshadditive_sky_shader;
        const MaterialDescription material{
            .schema_version = MaterialDescription::current_schema_version,
            .route = MaterialRoute::modern_spatial,
            .pass = additive_surface ? RenderPass::transparent : RenderPass::opaque,
            .program = std::string(program),
            .technique = {},
            .pass_name = {},
            .bindings = gloss ? meshgloss_bindings(*surface.meshgloss, sky_light)
                : (additive_surface ? meshadditive_bindings(*surface.meshadditive, additive.inputs)
                                    : std::vector<MaterialBinding>{{"eawr_diffuse", std::string{"space-sky-diffuse"}}}),
        };
        const sim::AssetId asset = next_asset++;
        std::optional<assets::Model> caster;
        if (caster_control(control_name)) {
            // Control only: the same surface with its reversed triangles added.
            caster = *surface.model;
            std::vector<std::uint16_t>& indices = caster->meshes.front().submeshes.front().indices;
            const std::size_t count = indices.size();
            for (std::size_t first = 0; first + 2 < count; first += 3) {
                indices.insert(indices.end(), {indices[first], indices[first + 2], indices[first + 1]});
            }
        }
        const auto result = renderer->upload(asset, caster ? *caster : *surface.model, *surface.texture, material);
        if (!result) {
            compiler[index] = result.error().code == diagnostic_codes::shader_compile_failed ? "rejected" : "not_observed";
            upload_failure[index] = core::format_diagnostic(result.error());
            material_status = result.error().code == diagnostic_codes::shader_compile_failed
                ? "compile_failed" : "upload_failed";
            partial_failure = !uploaded.empty();
            release_all();
            partial_failure_remaining = teardown_remaining.value_or(0);
            uploaded.clear();
            return false;
        }
        compiler[index] = "compiled";
        // Before any instance exists: a sky that encloses the scene must not
        // cast. The renderer applies it when the instance is created. Only
        // the labelled *-cast shadow controls let it cast, to prove that the
        // lit receiver would detect a casting sky.
        sky_casts = control_name == "sky-shadow-cast" || control_name == "sky-shadow-caster-cast";
        renderer->set_casts_shadows(asset, sky_casts);
        uploaded.push_back({index, asset, static_cast<sim::EntityId>(asset), true});
    }
    material_status = "compiled";
    return true;
}

bool SpaceEnvironment::State::compose_occluder() {
    // A screen-aligned quad halfway between the eye and surface 0's centroid.
    // Its extent is predeclared from surface 0's own geometry: a partial cover
    // keeps surface 0 measurable, a full cover must invalidate its evidence.
    if (uploaded.empty()) return false;
    const space::SurfacePlan& first = plan.surfaces[uploaded.front().surface];
    const auto triangles = space::render_triangles(first.model->meshes.front().submeshes.front());
    const std::array<float, 3> centre = centroid(triangles);
    float radius = 0.0F;
    for (const space::Triangle& triangle : triangles) {
        for (const assets::Vec3f& corner : triangle) {
            const float dx = corner.x - centre[0];
            const float dy = corner.y - centre[1];
            const float dz = corner.z - centre[2];
            radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }
    const std::array<float, 3> eye = camera.eye;
    std::array<float, 3> forward{centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2]};
    const float distance = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (!(distance > camera.near_plane * 4.0F) || !(radius > 0.0F)) return false;
    for (float& value : forward) value /= distance;
    // Screen axes of the look-at camera: right = up x back, up' = back x right.
    const std::array<float, 3> back{-forward[0], -forward[1], -forward[2]};
    const auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    std::array<float, 3> right = cross(camera.up, back);
    const float right_length = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (!(right_length > 1.0e-6F)) return false;
    for (float& value : right) value /= right_length;
    const std::array<float, 3> up = cross(back, right);
    const bool full = control == "occluder-full";
    const float half = (full ? 1.5F : 0.3F) * radius * 0.5F;
    const std::array<float, 3> middle{eye[0] + forward[0] * distance * 0.5F, eye[1] + forward[1] * distance * 0.5F,
                                      eye[2] + forward[2] * distance * 0.5F};
    const auto corner = [&](const float sx, const float sy) {
        // Render basis -> source basis is the inverse of (x, y, z) -> (x, z, -y).
        const std::array<float, 3> render{middle[0] + (right[0] * sx + up[0] * sy) * half,
                                          middle[1] + (right[1] * sx + up[1] * sy) * half,
                                          middle[2] + (right[2] * sx + up[2] * sy) * half};
        return assets::Vec3f{render[0], -render[2], render[1]};
    };
    assets::Submesh submesh;
    submesh.shader = "eawr-space-occluder-control";
    for (const auto& [sx, sy, u, v] : {std::array<float, 4>{-1, 1, 0, 0}, std::array<float, 4>{1, 1, 1, 0},
                                       std::array<float, 4>{1, -1, 1, 1}, std::array<float, 4>{-1, -1, 0, 1}}) {
        assets::Vertex vertex;
        vertex.position = corner(sx, sy);
        vertex.normal = {0.0F, 0.0F, 1.0F};
        vertex.texcoord[0] = {u, v};
        submesh.vertices.push_back(vertex);
    }
    submesh.indices = {0, 1, 2, 0, 2, 3};
    assets::Model model;
    model.source.logical_path = "eawr-space-occluder-control";
    assets::Mesh mesh;
    mesh.name = "OccluderControl";
    mesh.submeshes.push_back(submesh);
    model.meshes.push_back(std::move(mesh));
    assets::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.format = assets::PixelFormat::rgba8;
    assets::MipLevel mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.bytes = {std::byte{40}, std::byte{200}, std::byte{60}, std::byte{255}};
    texture.mips.push_back(std::move(mip));
    const MaterialDescription material{
        .schema_version = MaterialDescription::current_schema_version,
        .route = MaterialRoute::modern_spatial,
        .pass = RenderPass::opaque,
        .program = std::string(occluder_shader),
        .technique = {},
        .pass_name = {},
        .bindings = {{"eawr_diffuse", std::string{"space-occluder-control"}}},
    };
    if (!renderer->upload(occluder_asset, model, texture, material)) return false;
    occluder_uploaded = true;
    occluder_mask = space::rasterize(camera, space::render_triangles(submesh));
    return true;
}

bool SpaceEnvironment::State::compose_receiver() {
    // A lit square below the line of sight to surface 0, lit by one fixed
    // directional light aimed from surface 0's centre through the receiver's
    // centre, so that a casting sky would shadow all of it. It is a control
    // fixture, not a source-backed space lighting environment.
    if (uploaded.empty()) return false;
    const space::SurfacePlan& first = plan.surfaces[uploaded.front().surface];
    const auto triangles = space::render_triangles(first.model->meshes.front().submeshes.front());
    const std::array<float, 3> centre = centroid(triangles);
    float radius = 0.0F;
    for (const space::Triangle& triangle : triangles) {
        for (const assets::Vec3f& corner : triangle) {
            const float dx = corner.x - centre[0];
            const float dy = corner.y - centre[1];
            const float dz = corner.z - centre[2];
            radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }
    const std::array<float, 3> eye = camera.eye;
    std::array<float, 3> forward{centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2]};
    const float distance = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (!(distance > camera.near_plane * 4.0F) || !(radius > 0.0F)) return false;
    for (float& value : forward) value /= distance;
    const std::array<float, 3> back{-forward[0], -forward[1], -forward[2]};
    const auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    std::array<float, 3> right = cross(camera.up, back);
    const float right_length = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (!(right_length > 1.0e-6F)) return false;
    for (float& value : right) value /= right_length;
    const std::array<float, 3> up = cross(back, right);
    // The eye-to-centre line has no component along `up`, so the plane
    // spanned by `right` and `forward`, dropped below that line, faces both
    // the camera (at 0.6 of the distance) and the light (through the centre).
    // Its half size keeps every parallel light ray inside surface 0.
    const float drop = distance * 0.3F;
    const float half = radius * 0.35F;
    std::array<float, 3> middle{};
    for (std::size_t axis = 0; axis < 3; ++axis) middle[axis] = eye[axis] + forward[axis] * distance * 0.6F - up[axis] * drop;
    std::array<float, 3> toward{centre[0] - middle[0], centre[1] - middle[1], centre[2] - middle[2]};
    const float toward_length = std::sqrt(toward[0] * toward[0] + toward[1] * toward[1] + toward[2] * toward[2]);
    for (float& value : toward) value /= toward_length;
    // Render basis -> source basis is the inverse of (x, y, z) -> (x, z, -y).
    const auto source = [](const std::array<float, 3>& render) { return assets::Vec3f{render[0], -render[2], render[1]}; };
    assets::Submesh submesh;
    submesh.shader = "eawr-space-lit-receiver-control";
    for (const auto& [sx, sy, u, v] : {std::array<float, 4>{-1, 1, 0, 0}, std::array<float, 4>{1, 1, 1, 0},
                                       std::array<float, 4>{1, -1, 1, 1}, std::array<float, 4>{-1, -1, 0, 1}}) {
        std::array<float, 3> render{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            render[axis] = middle[axis] + (right[axis] * sx + forward[axis] * sy) * half;
        }
        assets::Vertex vertex;
        vertex.position = source(render);
        vertex.normal = source(up);
        vertex.texcoord[0] = {u, v};
        submesh.vertices.push_back(vertex);
    }
    // Both windings: cull_back draws exactly the camera-facing copy.
    submesh.indices = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    assets::Model model;
    model.source.logical_path = "eawr-space-lit-receiver-control";
    assets::Mesh mesh;
    mesh.name = "LitReceiverControl";
    mesh.submeshes.push_back(submesh);
    model.meshes.push_back(std::move(mesh));
    assets::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.format = assets::PixelFormat::rgba8;
    assets::MipLevel mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.bytes = {std::byte{200}, std::byte{200}, std::byte{200}, std::byte{255}};
    texture.mips.push_back(std::move(mip));
    GodotRenderer::LightingState lighting;
    lighting.toward_light = toward;
    lighting.shadows = true;
    // Covers the receiver and surface 0 behind it.
    lighting.shadow_max_distance = distance * 2.0F;
    lighting.shadow_atlas_size = 4096;
    renderer->set_lighting(lighting);
    const MaterialDescription material{
        .schema_version = MaterialDescription::current_schema_version,
        .route = MaterialRoute::modern_spatial,
        .pass = RenderPass::opaque,
        .program = std::string(receiver_shader),
        .technique = {},
        .pass_name = {},
        .bindings = {{"eawr_diffuse", std::string{"space-lit-receiver-control"}}},
    };
    if (!renderer->upload(occluder_asset, model, texture, material)) return false;
    // The receiver must not shadow itself: only the sky's cast is measured.
    renderer->set_casts_shadows(occluder_asset, false);
    occluder_uploaded = true;
    foreground_role = "lit_receiver_control";
    occluder_mask = space::rasterize(camera, space::render_triangles(submesh));
    return true;
}

std::optional<int> SpaceEnvironment::State::lifecycle_step() {
    // The composed sky is submitted and drawn, so its instances exist when
    // its assets are released through the lease path and uploaded again.
    renderer->submit(configured_snapshot);
    if (++lifecycle_frames < lifecycle_cycle_frames) return std::nullopt;
    lifecycle_frames = 0;
    lifecycle_instances_before.push_back(renderer->instance_count());
    lifecycle_resources_before.push_back(renderer->resources().size());
    for (const Uploaded& item : uploaded) static_cast<void>(renderer->release(item.asset));
    lifecycle_instances_after.push_back(renderer->instance_count());
    lifecycle_released.push_back(renderer->resources().size());
    if (!upload_all(control)) {
        completed = true;
        static_cast<void>(give_up("sky surface re-upload failed in live lifecycle cycle "
            + std::to_string(lifecycle_released.size())));
        return 2;
    }
    lifecycle_reuploaded.push_back(renderer->resources().size());
    return std::nullopt;
}

bool SpaceEnvironment::State::activate_camera(Node3D& node) {
    const viewer::MapCameraSource& source = *options.map_camera;
    // --eawr-capture locks the bridge to the parsed fixed camera; otherwise
    // the authored initial pose in the current host viewport is the camera.
    const bool locked = !options.capture_path.empty();
    const Vector2 viewport = node.get_viewport()->get_visible_rect().size;
    const std::uint32_t width = locked ? camera.width : static_cast<std::uint32_t>(viewport.x);
    const std::uint32_t height = locked ? camera.height : static_cast<std::uint32_t>(viewport.y);
    auto* display = DisplayServer::get_singleton();
    const bool terminal_probe = options.camera_terminal_baseline_test
        || options.camera_terminal_hold_test || options.camera_terminal_release_test;
    auto created = std::make_unique<viewer::MapCameraBridge>(
        terminal_probe || (display && display->window_is_focused()), camera_input::Context::space);
    const std::optional<tactical::TacticalFrame> fixed = locked
        ? std::optional<tactical::TacticalFrame>(tactical_frame(camera)) : std::nullopt;
    if (auto active = created->activate(source.config, source.constants, source.bindings_json,
            width, height, fixed); !active) {
        return give_up(core::format_diagnostic(active.error()));
    }
    bridge = std::move(created);
    host = &node;
    if (locked || !(options.real_time_clock || options.camera_selftest)) {
        // Keep the render target at the capture identity while the real host
        // window is resized: by the locked graphical probe, or by a window
        // manager (FancyZones snaps a new window into its zone) under an
        // unlocked probe, which then never sees a resize (a terminal probe
        // ignores resizes anyway). Only an interactive run and an unlocked
        // self-test, whose subject is following real resizes, draw at the
        // window size.
        pin_capture_viewport(*node.get_window(), width, height);
    }
    if (!locked) {
        camera = fixed_camera(bridge->frame());
        camera_status = space::CameraStatus::valid;
        camera_source = "interactive space map camera (project-authored config, Space_Mode constants)";
    }
    camera_initial = camera;
    window_size = node.get_window()->get_size();
    node.set_process_input(true);
    node.set_process_unhandled_input(true);
    return true;
}

void SpaceEnvironment::State::record_submissions() {
    // Observed from the renderer's own ordered work list, not restated from
    // the plan.
    observed = renderer->submission_evidence();
    bool matched = true;
    std::vector<std::pair<sim::EntityId, sim::AssetId>> seen;
    for (const auto& item : observed) seen.emplace_back(item.entity_id, item.asset_id);
    for (const Uploaded& item : uploaded) {
        if (std::find(seen.begin(), seen.end(), std::make_pair(item.entity, item.asset)) == seen.end()) {
            matched = false;
            missing_submissions.push_back("entity " + std::to_string(item.entity) + " asset "
                + std::to_string(item.asset));
        }
    }
    // Every surface is in the opaque pass except a MeshAdditive one, which is
    // in the transparent pass; the occluder control is opaque.
    for (const auto& item : observed) {
        const auto sky = std::find_if(uploaded.begin(), uploaded.end(),
            [&](const Uploaded& entry) { return entry.asset == item.asset_id; });
        const bool additive_asset = sky != uploaded.end()
            && plan.surfaces[sky->surface].material == space::SkyMaterial::meshadditive;
        if (item.pass != (additive_asset ? RenderPass::transparent : RenderPass::opaque)) matched = false;
    }
    const std::size_t expected = uploaded.size() + (occluder_uploaded ? 1U : 0U);
    if (observed.size() != expected) matched = false;
    submission_status = matched ? "matched" : "mismatch";
}

void SpaceEnvironment::State::selftest_tick(const std::uint32_t tick) {
    if (!options.camera_selftest || !bridge || !host) return;
    viewer::MapCameraBridge& camera_bridge = *bridge;
    const bool locked = camera_bridge.controller().capture_locked();
    const Vector2 centre = host->get_viewport()->get_visible_rect().size * 0.5F;
    const auto& controller = camera_bridge.controller();
    const auto unchanged = [&] {
        return tactical_frame(fixed_camera(camera_bridge.frame())) == tactical_frame(camera_initial);
    };
    const auto check = [this](std::string name, const bool passed) { checks.emplace_back(std::move(name), passed); };
    const auto notify = [&](const int what) { host->get_tree()->get_root()->propagate_notification(what); };
    const auto remember = [&] {
        mark = camera_bridge.frame().target;
        zoom_mark = controller.state().zoom;
        target_zoom_mark = controller.target_zoom();
        yaw_mark = controller.yaw_degrees();
        pitch_mark = controller.state().pitch_degrees;
        const auto& eye = camera_bridge.frame().eye;
        orbit_radius_mark = std::hypot(eye[0] - mark[0], eye[1] - mark[1], eye[2] - mark[2]);
    };
    switch (tick) {
    case 1:
        notify(Node::NOTIFICATION_APPLICATION_FOCUS_IN);
        check("focus established before input", camera_bridge.adapter().focused() && focus_notifications > 0);
        inject_key(KEY_D, true);
        break;
    case 2:
        // A declared one-second step makes the clamp independent of how
        // quickly the graphical runner schedules frames.
        if (auto moved = camera_bridge.step(1.0F); !moved) failure = core::format_diagnostic(moved.error());
        break;
    case 3: inject_key(KEY_D, false); break;
    case 4:
        check("pan reaches authored X clamp", locked ? unchanged()
            : camera_bridge.frame().target[0] == controller.render_bounds().max_x);
        break;
    case 5:
        remember();
        inject_button(MOUSE_BUTTON_MIDDLE, centre);
        break;
    // A middle drag without Ctrl translates (FoC); left and down, away from the X clamp.
    case 6: inject_motion(centre, Vector2(-20.0F, 20.0F)); break;
    case 8:
        check("grabbed drag translates without turning", locked ? unchanged()
            : controller.yaw_degrees() == yaw_mark && controller.state().zoom == zoom_mark
                && camera_bridge.frame().target[0] < mark[0]
                && camera_bridge.frame().target[2] > mark[2]);
        inject_button(MOUSE_BUTTON_MIDDLE, centre, false);
        break;
    case 10:
        remember();
        distance_mark = controller.state().distance;
        inject_button(MOUSE_BUTTON_WHEEL_DOWN, centre);
        break;
    case 12:
        check("wheel changes only zoom", locked ? unchanged()
            : controller.state().zoom > zoom_mark && controller.yaw_degrees() == yaw_mark
                && camera_bridge.frame().target == mark);
        distance_step = controller.state().distance;
        if (const auto target = tactical::solve(camera_bridge.constants(), controller.target_zoom())) {
            check("wheel distance eases toward target", locked ? unchanged()
                : distance_step > distance_mark && distance_step < target.value().distance);
        } else check("wheel distance eases toward target", false);
        break;
    case 13:
        check("wheel distance continues on idle frames", locked ? unchanged()
            : controller.state().distance > distance_step);
        inject_motion(centre, Vector2(-20.0F, 0.0F), true);
        break;
    case 14:
        check("Alt mouse motion pans target", locked ? unchanged()
            : camera_bridge.frame().target != mark);
        remember();
        inject_key(KEY_A, true);
        break;
    case 15:
        pan_moved = camera_bridge.frame().target != mark;
        notify(Node::NOTIFICATION_APPLICATION_FOCUS_OUT);
        break;
    case 16: remember(); break;
    case 18: notify(Node::NOTIFICATION_APPLICATION_FOCUS_IN); break;
    case 20:
        // A is still physically down; the regained focus must not revive it.
        check("focus loss cancels held pan", locked ? (unchanged() && focus_notifications >= 3)
            : pan_moved && camera_bridge.frame().target == mark && camera_bridge.adapter().focused()
                && !camera_bridge.adapter().is_held(camera_input::Action::pan_left));
        inject_key(KEY_A, false);
        break;
    case 21:
        remember();
        inject_key(KEY_A, true);
        break;
    case 23:
        check("fresh press after focus regain pans", locked ? unchanged()
            : camera_bridge.frame().target != mark);
        inject_key(KEY_A, false);
        break;
    case 24:
        remember();
        inject_key(KEY_A, true);
        break;
    case 26:
        pan_moved = camera_bridge.frame().target != mark;
        window_size = host->get_window()->get_size();
        generation_mark = camera_bridge.adapter().counters().viewport_generation;
        host->get_window()->set_size(window_size - Vector2i(16, 16));
        break;
    case 28: remember(); break;
    case 31:
        check("real resize cancels held pan", locked ? (unchanged()
            && host->get_window()->get_size() != window_size
            && camera_bridge.adapter().counters().viewport_generation == generation_mark)
            : pan_moved && resize_notifications > 0
                && camera_bridge.adapter().counters().viewport_generation > generation_mark
                && camera_bridge.frame().target == mark);
        if (!locked) host->get_window()->set_size(window_size);
        inject_key(KEY_A, false);
        break;
    case 33:
        remember();
        inject_motion(Vector2(0.0F, centre.y), Vector2());
        break;
    case 34:
        edge_changed_only_target = camera_bridge.frame().target != mark
            && controller.target_zoom() == target_zoom_mark && controller.yaw_degrees() == yaw_mark;
        notify(Node::NOTIFICATION_WM_MOUSE_EXIT);
        remember();
        break;
    case 35:
        check("edge pan changes only target", locked ? unchanged() : edge_changed_only_target);
        check("pointer exit cancels edge pan", locked ? (unchanged() && !camera_bridge.adapter().pointer_valid())
            : !camera_bridge.adapter().pointer_valid() && camera_bridge.frame().target == mark);
        break;
    case 37: inject_key(KEY_HOME, true); break;
    case 40:
        check("reset restores authored pose", locked ? unchanged()
            : camera_bridge.resets() == 1U
                && camera_bridge.frame().target[0] == camera_bridge.config().target_x
                && camera_bridge.frame().target[1] == camera_bridge.config().target_height
                && camera_bridge.frame().target[2] == -camera_bridge.config().target_y
                && controller.yaw_degrees() == camera_bridge.config().yaw_degrees
                && controller.state().zoom == camera_bridge.config().zoom);
        inject_key(KEY_HOME, false);
        break;
    case 42:
        check("input callbacks reached space adapter", camera_bridge.input_callbacks() >= 8U);
        check("capture lock isolates input", !locked
            || (unchanged() && camera_bridge.steps() == 0U
                && camera_bridge.adapter().counters().ignored_ineligible > 0));
        break;
    case 44:
        remember();
        inject_ctrl(true);
        inject_ctrl(true, true);
        inject_button(MOUSE_BUTTON_MIDDLE, centre, true, true);
        break;
    // Ctrl + middle-drag right and up by 40% of the height: 40 mouse units at
    // the fixture's Pitch_Per_Mouse_Unit -1.25 tilt 50 degrees toward the
    // horizon, so from the 45-degree default the orbit passes under the
    // battle plane, above the XML Pitch_Min of -20.
    case 45:
        inject_motion(centre, Vector2(8.0F, -0.8F * centre.y), false, true);
        inject_ctrl(true, true);
        break;
    case 47:
        check("Ctrl grabbed drag orbits under the plane", locked ? unchanged()
            : controller.yaw_degrees() < yaw_mark && controller.state().pitch_degrees < 0.0F
                && controller.state().pitch_degrees < pitch_mark
                && controller.state().pitch_degrees >= camera_bridge.orbit_pitch_range().min_degrees
                && camera_bridge.frame().eye[1] < camera_bridge.frame().target[1]
                && controller.state().zoom == zoom_mark
                && orbit_focus_at_centre(camera_bridge.frame(), mark, orbit_radius_mark));
        inject_button(MOUSE_BUTTON_MIDDLE, centre, false, true);
        inject_ctrl(false);
        break;
    // A Ctrl click does not reset; a plain middle click resets the view in place.
    case 49:
        inject_ctrl(true);
        inject_button(MOUSE_BUTTON_MIDDLE, centre, true, true);
        break;
    case 50:
        inject_button(MOUSE_BUTTON_MIDDLE, centre, false, true);
        inject_ctrl(false);
        break;
    case 52:
        check("Ctrl click keeps the view", locked ? unchanged()
            : camera_bridge.view_resets() == 0U && controller.orbit_pitch_offset() != 0.0F);
        remember();
        inject_button(MOUSE_BUTTON_MIDDLE, centre);
        break;
    case 53: inject_button(MOUSE_BUTTON_MIDDLE, centre, false); break;
    case 55:
        check("middle click resets the view around the target", locked ? unchanged()
            : camera_bridge.view_resets() == 1U
                && controller.yaw_degrees() == camera_bridge.config().yaw_degrees
                && controller.state().zoom == camera_bridge.config().zoom
                && controller.orbit_pitch_offset() == 0.0F
                && camera_bridge.frame().target[0] == mark[0]
                && camera_bridge.frame().target[2] == mark[2]);
        break;
    default: break;
    }
}

std::optional<int> SpaceEnvironment::State::interactive_process(const double delta) {
    const std::uint32_t terminal = options.warmup_frames + options.timed_frames;
    if (compare_started) {
        // A frozen-pose drawn check, not sky fidelity: the same pose with the
        // sky submission removed must differ from the interactive capture.
        renderer->submit(disabled_snapshot);
        if (++compare_frames < 4) return std::nullopt;
        auto disabled_capture = renderer->capture(camera);
        if (!disabled_capture) return fail(core::format_diagnostic(disabled_capture.error()));
        capture_hashes["sky_disabled"] = hash_bytes(disabled_capture.value().png_bytes);
        const auto drawn = rgb_of(interactive_capture);
        const auto empty = rgb_of(disabled_capture.value().png_bytes);
        if (!drawn || !empty || drawn->width != empty->width || drawn->height != empty->height) {
            drawn_status = "failed";
            return fail("interactive capture or its sky-disabled control could not be decoded");
        }
        for (std::size_t offset = 0; offset + 2 < drawn->rgb.size(); offset += 3) {
            ++drawn_sampled_pixels;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const int difference = static_cast<int>(drawn->rgb[offset + channel])
                    - static_cast<int>(empty->rgb[offset + channel]);
                if (std::abs(difference) > static_cast<int>(space::changed_threshold)) {
                    ++drawn_changed_pixels;
                    break;
                }
            }
        }
        return finish_interactive();
    }
    if (!settle_started) {
        if (!std::isfinite(delta) || delta < 0.0 || delta > static_cast<double>(std::numeric_limits<float>::max())) {
            return fail("space camera process delta is invalid");
        }
        const std::uint32_t tick = frame + 1U;
        const bool terminal_probe = options.camera_terminal_baseline_test
            || options.camera_terminal_hold_test || options.camera_terminal_release_test;
        const bool terminal_pan = options.camera_terminal_hold_test || options.camera_terminal_release_test;
        if (terminal_pan && tick == terminal) {
            if (!bridge->adapter().is_held(camera_input::Action::pan_right)) {
                return fail("terminal pan input did not reach the space adapter before the terminal step");
            }
            terminal_held_at_step = true;
        }
        const float seconds = terminal_probe
            ? (tick == terminal ? terminal_step_seconds : 1.0F / 60.0F) : static_cast<float>(delta);
        if (auto stepped = bridge->step(seconds); !stepped) return fail(core::format_diagnostic(stepped.error()));
        selftest_tick(tick);
        if (terminal_pan) {
            // Route the declared key edges through the adapter at fixed frame
            // points. Godot's event queue can lag a process callback under load.
            if (tick == terminal - 1U || (tick == terminal && options.camera_terminal_release_test)) {
                const bool pressed = tick == terminal - 1U;
                if (auto handled = bridge->handle(camera_input::RawEvent{
                        .kind = camera_input::RawKind::key,
                        .code = *camera_input::key_code("D"),
                        .pressed = pressed}); !handled) {
                    return fail(core::format_diagnostic(handled.error()));
                }
            }
        }
        if (!failure.empty()) return fail(failure);
        camera = fixed_camera(bridge->frame());
        renderer->set_camera(camera);
    }
    ++frame;
    renderer->submit(configured_snapshot);
    if (frame == 1) record_submissions();
    if (frame == options.warmup_frames) {
        timing_start = std::chrono::steady_clock::now();
        return std::nullopt;
    }
    if (frame < terminal) return std::nullopt;
    if (!settle_started) {
        // Advancement stops at the declared terminal frame; the pose is
        // frozen through settling and readback.
        timed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - timing_start).count();
        settle_started = true;
        steps_at_freeze = bridge->steps();
        return std::nullopt;
    }
    if (++settle_frames < settle_frame_count) return std::nullopt;
    if (bridge->steps() != steps_at_freeze
        || tactical_frame(fixed_camera(bridge->frame())) != tactical_frame(camera)) {
        return fail("space camera changed during terminal settle");
    }
    auto capture = renderer->capture(camera);
    if (!capture || capture.value().png_bytes.empty()) {
        return fail(capture ? std::string("capture is empty") : core::format_diagnostic(capture.error()));
    }
    if (capture.value().width != camera.width || capture.value().height != camera.height) {
        return fail("space camera capture dimensions differ from camera identity");
    }
    if (!rgb_of(capture.value().png_bytes)) return fail("interactive space capture PNG could not be decoded");
    capture_hashes["configured"] = hash_bytes(capture.value().png_bytes);
    if (!options.unlocked_capture_path.empty()
        && !persist(options.unlocked_capture_path, capture.value().png_bytes)) {
        return fail("requested artifact was not written: " + artifacts_failed.back());
    }
    interactive_capture = std::move(capture.value().png_bytes);
    compare_started = true;
    return std::nullopt;
}

std::optional<int> SpaceEnvironment::State::finish_interactive() {
    completed = true;
    release_all();
    // Sky fidelity is a fixed-camera claim: its masks and comparison phases
    // are never computed for an interactive pose, so none can be inherited.
    pixel_status = "not_evaluated_interactive";
    drawn_status = drawn_changed_pixels >= 64U ? "verified" : "failed";
    const bool checks_ok = selftest_passed();
    const bool passed = load_status == "loaded" && material_status == "compiled"
        && submission_status == "matched" && drawn_status == "verified" && checks_ok
        && failure.empty() && artifacts_failed.empty()
        && teardown_remaining == std::optional<std::size_t>(0);
    if (!passed && failure.empty()) {
        if (!artifacts_failed.empty()) failure = "requested artifact was not written: " + artifacts_failed.front();
        else if (submission_status != "matched") failure = "observed submissions disagree with the plan";
        else if (drawn_status != "verified") failure = "the frozen interactive pose drew no measurable sky";
        else if (!checks_ok) failure = "space camera graphical selftest failed";
        else failure = "renderer resources remained after teardown";
    }
    status = passed ? (options.camera_selftest ? "space_camera_selftest_passed" : "space_camera_render_passed")
                    : "failed";
    if (!write_report()) return 2;
    return passed ? 0 : 2;
}

std::optional<int> SpaceEnvironment::State::finish() {
    completed = true;
    const auto configured = captures.find("configured");
    const auto disabled = captures.find("sky_disabled");
    std::optional<space::Rgb8Image> configured_rgb;
    std::optional<space::Rgb8Image> disabled_rgb;
    if (configured != captures.end()) configured_rgb = rgb_of(configured->second);
    if (disabled != captures.end()) disabled_rgb = rgb_of(disabled->second);
    std::vector<space::SurfaceRegions> regions;
    std::vector<std::optional<space::Rgb8Image>> isolated;
    for (std::size_t index = 0; index < uploaded.size(); ++index) {
        const space::SurfacePlan& surface = plan.surfaces[uploaded[index].surface];
        const assets::Submesh& submesh = surface.model->meshes.front().submeshes.front();
        space::SurfaceRegions region;
        region.mask = space::rasterize(camera, space::render_triangles(submesh));
        const auto quadrants = space::render_triangles_by_uv_quadrant(submesh);
        for (std::size_t quadrant = 0; quadrant < 4; ++quadrant) {
            region.quadrants[quadrant] = space::rasterize(camera, quadrants[quadrant]);
        }
        if (!options.capture_path.empty()) {
            // Predeclared regions leave the run as plain PGM masks (dilated by
            // the evaluation margin) so an external control can attribute a
            // change to one surface without trusting this process.
            const space::ScreenMask grown = space::dilate(region.mask, space::mask_margin);
            std::string header = "P5\n" + std::to_string(grown.width) + " " + std::to_string(grown.height) + "\n255\n";
            std::vector<std::byte> pgm;
            for (const char character : header) pgm.push_back(static_cast<std::byte>(character));
            for (const std::uint8_t bit : grown.bits) pgm.push_back(bit != 0U ? std::byte{255} : std::byte{0});
            // A failure is recorded and fails the run below, after release.
            static_cast<void>(persist(sibling(options.capture_path, ".mask-surface-" + std::to_string(index) + ".pgm"), pgm));
        }
        regions.push_back(std::move(region));
        std::optional<space::Rgb8Image> alone;
        if (index < isolated_captures.size() && isolated_captures[index]) alone = rgb_of(*isolated_captures[index]);
        isolated.push_back(std::move(alone));
    }
    if (!configured_rgb || !disabled_rgb) {
        pixel_status = "failed";
        pixels.failure = "configured or sky-disabled capture could not be decoded";
    } else {
        pixels = space::evaluate_pixels(regions, *configured_rgb, *disabled_rgb, isolated, occluder_mask);
        pixel_status = pixels.status;
    }
    if (shadow_control(control)) {
        // The receiver is the foreground control region: any sky-caused
        // change there is a shadow, since the unshaded sky emits no light.
        if (pixels.occlusion_status == "verified") lit_foreground = "unchanged";
        else if (pixels.occlusion_status == "failed") lit_foreground = "shadowed_by_sky";
        else lit_foreground = pixels.occlusion_status;
    }
    if (options.fog) evaluate_fog();
    release_all();

    const bool camera_ok = !bridge || (selftest_passed()
        && (!options.camera_selftest || resize_active_at_capture));
    const bool passed = load_status == "loaded" && material_status == "compiled"
        && submission_status == "matched" && pixel_status == "verified"
        && artifacts_failed.empty() && camera_ok
        && teardown_remaining == std::optional<std::size_t>(0)
        && (!options.fog || fog_status == "verified");
    if (!passed && failure.empty()) {
        if (!artifacts_failed.empty()) failure = "requested artifact was not written: " + artifacts_failed.front();
        else if (load_status == "blocked") {
            failure = "space primary sky plan is " + std::string(space::to_string(plan.status))
                + (plan.detail.empty() ? "" : ": " + plan.detail);
        } else if (options.fog && fog_status != "verified" && pixel_status == "verified") failure = fog_failure;
        else if (!camera_ok) failure = "space camera graphical selftest failed";
        else if (lit_foreground == "shadowed_by_sky") {
            failure = std::string("the sky shadowed the lit foreground receiver (sky casting ")
                + (sky_casts ? "enabled by a labelled control)" : "disabled)");
        } else if (submission_status != "matched") failure = "observed submissions disagree with the plan";
        else if (pixel_status != "verified") failure = pixels.failure.empty() ? "pixel evidence " + pixel_status : pixels.failure;
        else failure = "renderer resources remained after teardown";
    }
    status = passed ? (options.fog ? "space_fog_passed" : "space_primary_sky_passed") : "failed";
    if (!write_report()) return 2;
    return passed ? 0 : 2;
}

SpaceEnvironment::SpaceEnvironment(Options options) : state_(std::make_unique<State>(std::move(options))) {}
SpaceEnvironment::~SpaceEnvironment() = default;

bool SpaceEnvironment::ready(Node3D& host, const assets::Map& map, const vfs::Vfs& filesystem,
                             const assets::ObjectTypeCatalog& catalog, const bool catalog_loaded,
                             const std::string& catalog_failure) {
    State& state = *state_;
    // Only an explicit evidence control or fog selects the primary-sky harness.
    if (state.options.control.empty() && !state.options.fog) {
        state.view = std::make_unique<EnvironmentView>(state.options);
        return state.view->ready(host, map, filesystem, catalog, catalog_loaded, catalog_failure);
    }
    state.catalog_failure = catalog_failure;
    state.declared_placements = map.placements.size();
    if (!state.options.control.empty()) state.control = state.options.control;
    state.additive = parse_additive_control(state.control);
    if (state.additive.enabled) {
        if (!state.additive.failure.empty()) {
            return state.give_up("--eawr-space-control " + std::string(meshadditive_control) + ": "
                                 + state.additive.failure);
        }
        // The rest of the scene keeps its own controls: none or the occluder.
        // The report still names the labelled control (control_label()), and
        // the fixed-camera guard below still applies to it.
        state.control = state.additive.base;
    }
    constexpr std::array<std::string_view, 11> controls{"none", "drop-submission", "broken-shader", "occluder",
        "occluder-full", "reload-cycle", "sky-shadow", "sky-shadow-cast", "sky-shadow-caster", "sky-shadow-caster-cast",
        "meshgloss-light-probe"};
    if (std::find(controls.begin(), controls.end(), state.control) == controls.end()) {
        return state.give_up("--eawr-space-control must be none, drop-submission, broken-shader, occluder, "
                             "occluder-full, reload-cycle, sky-shadow, sky-shadow-cast, sky-shadow-caster, "
                             "sky-shadow-caster-cast or meshgloss-light-probe");
    }
    if (state.control == "meshgloss-light-probe") state.sky_light = light_probe_policy();
    if (state.options.fog) {
        // The fog join is evidenced on the fixed camera frame only, with the
        // shipped sky and no foreground control whose masks would overlap it.
        if (state.options.map_camera) {
            return state.give_up("space fog supports only the fixed --eawr-space-camera path, not a space map camera");
        }
        if (state.control != "none" || state.additive.enabled) {
            return state.give_up("space fog supports only --eawr-space-control none");
        }
        if (state.options.catalog == nullptr) {
            return state.give_up("space fog admission needs the XML catalog, which did not load: " + catalog_failure);
        }
    }
    // An unlocked space map camera draws from its authored pose; the fixed
    // camera, its controls and their evidence belong to --eawr-capture runs.
    const bool unlocked_camera = state.options.map_camera && state.options.capture_path.empty();
    if (unlocked_camera) {
        if (!state.options.camera.empty()) {
            return state.give_up("an unlocked space map camera takes its initial pose from --eawr-map-camera-config; "
                                 "--eawr-space-camera is the fixed capture camera and needs --eawr-capture");
        }
        if (state.control != "none" || state.additive.enabled) {
            return state.give_up("--eawr-space-control applies only to the fixed capture camera");
        }
    } else {
        if (state.options.camera.empty()) {
            state.camera_status = space::CameraStatus::malformed;
            return state.give_up("a space map needs --eawr-space-camera: no corpus camera is frozen for this slice, "
                                 "and none is inferred from source volumes or extents");
        }
        const FixedCamera defaults;
        const space::CameraParse parsed = space::parse_camera(state.options.camera, defaults.width, defaults.height);
        state.camera_status = parsed.status;
        state.camera = parsed.camera;
        if (parsed.status != space::CameraStatus::valid) {
            return state.give_up("--eawr-space-camera is " + std::string(space::to_string(parsed.status)));
        }
    }
    if (!unlocked_camera) {
        // The OS may give the window a different size from the fixed camera.
        // Every fixed-camera run reads the root viewport back and checks its
        // size, captured or not (the fog paint evidence writes no capture),
        // so pin that render target to the camera identity before the first
        // frame is drawn.
        pin_capture_viewport(*host.get_window(), state.camera.width, state.camera.height);
    }

    // Plan. All I/O stays here; the plan only sees loaded values.
    const assets::ObjectTypeRef* type = nullptr;
    if (!map.environments.empty() && map.environments.front().primary_sky) {
        type = assets::find_object_type(catalog, *map.environments.front().primary_sky);
    }
    std::map<std::string, space::TextureLookup> textures;
    space::PlanInput input;
    input.map = &map;
    input.sky_type = type;
    input.catalog_loaded = catalog_loaded;
    input.qualifications = space::qualifications();
    input.material_routes = space::material_routes();
    // Only the labelled meshadditive-synthetic control widens the routes.
    if (state.additive.enabled) input.material_routes = space::meshadditive_material_routes();
    input.model = [&](const std::string_view declared) {
        space::ModelLookup lookup;
        const auto path = probe_reference(filesystem, "data/art/models/", declared, model_suffixes);
        if (!path) {
            lookup.status = space::ModelLookup::Status::not_in_vfs;
            lookup.logical_path = std::string(declared);
            lookup.failure = "no data/art/models/ record for the declared model or its stem";
            return lookup;
        }
        lookup.logical_path = *path;
        auto bytes = filesystem.open(*path);
        auto model = assets::load_model(filesystem, *path);
        if (!bytes || !model) {
            lookup.status = space::ModelLookup::Status::failed_to_load;
            lookup.failure = !bytes ? core::format_diagnostic(bytes.error()) : core::format_diagnostic(model.error());
            return lookup;
        }
        lookup.sha256 = hash_bytes(bytes.value());
        lookup.status = space::ModelLookup::Status::resolved;
        lookup.model = std::move(model.value());
        return lookup;
    };
    input.texture = [&](const std::string_view declared) {
        const auto cached = textures.find(std::string(declared));
        if (cached != textures.end()) return cached->second;
        space::TextureLookup lookup;
        const auto path = probe_reference(filesystem, "data/art/textures/", declared, texture_suffixes);
        if (!path) {
            lookup.status = space::TextureLookup::Status::not_in_vfs;
            lookup.logical_path = std::string(declared);
            lookup.failure = "no data/art/textures/ record for the declared texture or its stem";
        } else {
            lookup.logical_path = *path;
            auto bytes = filesystem.open(*path);
            auto texture = assets::load_texture(filesystem, *path);
            if (!bytes || !texture) {
                lookup.status = space::TextureLookup::Status::failed_to_decode;
                lookup.failure = !bytes ? core::format_diagnostic(bytes.error())
                                        : core::format_diagnostic(texture.error());
            } else {
                lookup.status = space::TextureLookup::Status::resolved;
                lookup.sha256 = hash_bytes(bytes.value());
                lookup.texture = std::move(texture.value());
            }
        }
        textures.emplace(std::string(declared), lookup);
        return lookup;
    };
    state.plan = space::build_plan(input);
    state.plan_built = true;
    state.compiler.assign(state.plan.surfaces.size(), "not_attempted");
    state.upload_failure.assign(state.plan.surfaces.size(), "");
    const bool sky_blocked = state.plan.status != space::PlanStatus::ready;
    state.load_status = sky_blocked ? "blocked" : "loaded";
    if (sky_blocked) {
        return state.give_up("space primary sky plan is " + std::string(space::to_string(state.plan.status))
                                 + (state.plan.detail.empty() ? "" : ": " + state.plan.detail),
                             "space_blocked");
    }

    // One renderer on the host's own scenario; no second device and no
    // per-object scene nodes. The camera is live before any upload or frame.
    state.renderer = std::make_unique<GodotRenderer>(host);
    if (state.options.map_camera && !state.activate_camera(host)) return false;
    state.renderer->set_camera(state.camera);
    if (state.options.fog) {
        const auto enabled = state.renderer->enable_fog({{state.options.fog->stream(), state.options.fog->team()}});
        if (!enabled) return state.give_up(core::format_diagnostic(enabled.error()));
        state.fog_renderer_stream = state.options.fog->stream();
    }
    if (!sky_blocked && !state.upload_all(state.control)) {
        return state.give_up("sky surface upload failed: " + state.material_status);
    }
    const bool occluded = state.control == "occluder" || state.control == "occluder-full";
    if (occluded && !state.compose_occluder()) {
        return state.give_up("the foreground occluder control could not be composed");
    }
    if (shadow_control(state.control) && !state.compose_receiver()) {
        return state.give_up("the lit foreground receiver control could not be composed");
    }

    if (state.options.fog) {
        state.fog_units.emplace();
        if (!state.fog_units->compose(*state.renderer, map, state.options.map_sha256, filesystem,
                                      *state.options.catalog, state.options.fog_admit)) {
            return state.give_up(state.fog_units->failure());
        }
    }

    std::vector<sim::RenderInstance> base;
    if (state.occluder_uploaded) base.push_back({occluder_entity, occluder_asset, identity_transform()});
    std::vector<sim::RenderInstance> all = base;
    if (state.control == "drop-submission" && !state.uploaded.empty()) state.uploaded.back().submitted = false;
    for (const State::Uploaded& item : state.uploaded) {
        if (item.submitted) all.push_back({item.entity, item.asset, identity_transform()});
    }
    // With fog, every sky-only submission carries the selected immutable grid
    // set too, so the renderer binds it once and the sky evidence phases are
    // unchanged: the units are submitted only by the fog phases.
    const auto snapshot_of = [&](std::vector<sim::RenderInstance> instances) {
        return state.options.fog ? state.options.fog->snapshot(std::move(instances))
                                 : std::make_shared<const sim::RenderSnapshot>(0, std::move(instances));
    };
    state.sky_instances = all;
    state.configured_snapshot = snapshot_of(all);
    state.disabled_snapshot = snapshot_of(base);
    state.isolated_captures.assign(state.uploaded.size(), std::nullopt);
    for (std::size_t index = 0; index < state.uploaded.size(); ++index) {
        const State::Uploaded& item = state.uploaded[index];
        if (!item.submitted) continue;
        std::vector<sim::RenderInstance> alone = base;
        alone.push_back({item.entity, item.asset, identity_transform()});
        state.phases.push_back({"surface_" + std::to_string(index), snapshot_of(std::move(alone)), index});
    }
    state.phases.push_front({"sky_disabled", state.disabled_snapshot, std::nullopt});
    if (state.options.fog) {
        state.phases.push_back({"fog_units", nullptr, std::nullopt, 1});
        if (!state.options.fog_paint_evidence.empty()) {
            state.phases.push_back({"fog_units_painted", nullptr, std::nullopt, 2});
            state.phases.push_back({"fog_units_restored", nullptr, std::nullopt, 3});
        }
    }
    return true;
}

void SpaceEnvironment::camera_event(const viewer::camera_input::RawEvent& event) {
    State& state = *state_;
    if (state.view) { state.view->camera_event(event); return; }
    if (!state.bridge) return;
    if (state.options.camera_terminal_baseline_test || state.options.camera_terminal_hold_test
        || state.options.camera_terminal_release_test) return;
    if (auto handled = state.bridge->handle(event); !handled) state.failure = core::format_diagnostic(handled.error());
}

void SpaceEnvironment::camera_focus(const bool focused) {
    State& state = *state_;
    if (state.view) { state.view->camera_focus(focused); return; }
    if (!state.bridge) return;
    if (state.options.camera_terminal_baseline_test || state.options.camera_terminal_hold_test
        || state.options.camera_terminal_release_test) return;
    ++state.focus_notifications;
    state.bridge->set_focus(focused);
}

void SpaceEnvironment::camera_pointer_left() {
    if (state_->view) { state_->view->camera_pointer_left(); return; }
    if (state_->options.camera_terminal_baseline_test || state_->options.camera_terminal_hold_test
        || state_->options.camera_terminal_release_test) return;
    if (state_->bridge) state_->bridge->pointer_left();
}

void SpaceEnvironment::camera_viewport(const float width, const float height) {
    State& state = *state_;
    if (state.view) { state.view->camera_viewport(width, height); return; }
    if (!state.bridge) return;
    if (state.options.camera_terminal_baseline_test || state.options.camera_terminal_hold_test
        || state.options.camera_terminal_release_test) return;
    ++state.resize_notifications;
    // Frozen from the terminal frame through readback.
    if (state.settle_started) return;
    if (auto resized = state.bridge->set_viewport(width, height); !resized) {
        state.failure = core::format_diagnostic(resized.error());
    }
}

std::optional<int> SpaceEnvironment::process(const double delta) {
    State& state = *state_;
    if (state.view) return state.view->process(delta);
    if (state.completed || !state.renderer) return std::nullopt;
    if (state.interactive()) return state.interactive_process(delta);
    if (state.frame > state.options.warmup_frames + state.options.timed_frames) {
        // Comparison phases: the same camera with only the submission changed.
        // The renderer reads back the last drawn frame, so frames are drawn
        // after each change before the capture.
        State::Phase& phase = state.phases.front();
        if (phase.fog_step != 0 && !phase.snapshot) phase.snapshot = state.fog_phase_snapshot(phase.fog_step);
        state.renderer->submit(phase.snapshot);
        if (!state.fog_bound()) return state.fail(state.fog_failure);
        if (++state.phase_frames < 4) return std::nullopt;
        state.phase_frames = 0;
        auto capture = state.renderer->capture(state.camera);
        if (!capture) {
            state.completed = true;
            state.give_up(core::format_diagnostic(capture.error()));
            return 2;
        }
        state.capture_hashes[phase.name] = hash_bytes(capture.value().png_bytes);
        if (!state.options.capture_path.empty()
            && !state.persist(sibling(state.options.capture_path, "." + phase.name + ".png"), capture.value().png_bytes)) {
            state.completed = true;
            state.give_up("requested artifact was not written: " + state.artifacts_failed.back());
            return 2;
        }
        if (phase.fog_step != 0) {
            const std::string label(State::fog_label(phase.fog_step));
            const auto status = state.renderer->fog_status();
            state.fog_phase_hashes[label] = state.capture_hashes[phase.name];
            state.fog_phase_uploads[label] = status.cache.uploads;
            state.fog_phase_updates[label] = status.cache.updates;
            state.fog_phase_revisions[label] = status.bound_revision.value_or(0);
            if (phase.fog_step == 1) {
                // Presence witnesses from the renderer itself: an all-dark
                // unit cannot show that it was drawn.
                state.fog_observed = state.renderer->submission_evidence();
                state.fog_consumers = state.renderer->fog_consumers();
            }
            if (!state.options.fog_paint_evidence.empty()) {
                std::filesystem::path path = state.options.fog_paint_evidence;
                path.replace_extension(std::filesystem::path("." + label + ".png"));
                if (!state.persist(path, capture.value().png_bytes)) {
                    return state.fail("requested artifact was not written: " + state.artifacts_failed.back());
                }
            }
        }
        if (phase.isolated) state.isolated_captures[*phase.isolated] = capture.value().png_bytes;
        else state.captures[phase.name] = std::move(capture.value().png_bytes);
        state.phases.pop_front();
        if (!state.phases.empty()) return std::nullopt;
        return state.finish();
    }
    if (state.control == "reload-cycle" && state.lifecycle_released.size() < lifecycle_cycles) {
        return state.lifecycle_step();
    }
    if (state.bridge) {
        // Locked to the fixed camera: the step is inert and hostile self-test
        // input is ignored; the fixed camera below is never rewritten.
        if (auto stepped = state.bridge->step(static_cast<float>(delta)); !stepped) {
            return state.fail(core::format_diagnostic(stepped.error()));
        }
        state.selftest_tick(state.frame + 1U);
        if (!state.failure.empty()) return state.fail(state.failure);
    }
    ++state.frame;
    state.renderer->submit(state.configured_snapshot);
    if (!state.fog_bound()) return state.fail(state.fog_failure);
    if (state.frame == 1) state.record_submissions();
    if (state.frame == state.options.warmup_frames) {
        state.timing_start = std::chrono::steady_clock::now();
        return std::nullopt;
    }
    if (state.frame < state.options.warmup_frames + state.options.timed_frames) return std::nullopt;
    state.timed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.timing_start).count();
    auto capture = state.renderer->capture(state.camera);
    if (!capture || capture.value().png_bytes.empty()) {
        state.completed = true;
        state.give_up(capture ? std::string("capture is empty") : core::format_diagnostic(capture.error()));
        return 2;
    }
    if (capture.value().width != state.camera.width || capture.value().height != state.camera.height) {
        state.completed = true;
        state.give_up("the capture viewport " + std::to_string(capture.value().width) + "x"
            + std::to_string(capture.value().height) + " disagrees with the fixed camera viewport "
            + std::to_string(state.camera.width) + "x" + std::to_string(state.camera.height));
        return 2;
    }
    if (state.bridge && state.options.camera_selftest && state.host) {
        // The locked probe's host resize must still be in effect at readback.
        state.resize_active_at_capture = state.host->get_window()->get_size() != state.window_size;
        if (!state.resize_active_at_capture) {
            return state.fail("locked capture restored the host window before readback");
        }
        state.host->get_window()->set_size(state.window_size);
    }
    state.capture_hashes["configured"] = hash_bytes(capture.value().png_bytes);
    state.capture_size = {capture.value().width, capture.value().height};
    if (!state.options.capture_path.empty() && !state.persist(state.options.capture_path, capture.value().png_bytes)) {
        state.completed = true;
        state.give_up("requested artifact was not written: " + state.artifacts_failed.back());
        return 2;
    }
    state.captures["configured"] = std::move(capture.value().png_bytes);
    ++state.frame;
    return std::nullopt;
}
} // namespace eawr::presentation::godot_backend
