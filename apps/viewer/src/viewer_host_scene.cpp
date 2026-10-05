#include "viewer_host_internal.hpp"
#include "audio_output.hpp"
#include "startup_trace.hpp"
#include <godot_cpp/classes/rendering_server.hpp>

namespace eawr::presentation::godot_backend {

[[nodiscard]] std::optional<std::vector<std::byte>> viewer_host_detail::read_bytes(
    const ViewerPath& path) {
    if (path.is_godot_resource()) {
        const std::string& identifier = path.value();
        const Ref<FileAccess> input = FileAccess::open(
            String::utf8(identifier.data(), static_cast<int64_t>(identifier.size())),
            FileAccess::READ);
        if (input.is_null()) return std::nullopt;
        const PackedByteArray packed = input->get_buffer(input->get_length());
        std::vector<std::byte> result(static_cast<std::size_t>(packed.size()));
        if (!result.empty()) std::memcpy(result.data(), packed.ptr(), result.size());
        return result;
    }
    std::ifstream input(path.native(), std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const std::streampos end = input.tellg();
    if (end < 0) return std::nullopt;
    std::vector<std::byte> result(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!result.empty()) {
        input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    }
    return input || result.empty() ? std::optional(std::move(result)) : std::nullopt;
}

bool ViewerHost::load_scene() {
    const auto scene = read_bytes(options_->scene_path);
    if (!scene) {
        status_message_ = "common scene could not be read";
        return false;
    }
    scene_hash_ = hash_bytes(*scene);
    if (scene_hash_ != expected_scene_hash) {
        status_message_ = "common scene SHA-256 mismatch";
        return false;
    }
    active_content_profile_ = options_->profile.empty() ? (!options_->mod_root.empty()
        ? "remake" : std::filesystem::is_directory(options_->game_root / "corruption" / "Data")
            ? "foc" : "eaw") : options_->profile;
    auto filesystem = mount_content_layers(
        options_->game_root, options_->mod_root, active_content_profile_, status_message_);
    if (!filesystem) return false;
    // One plan separates the frozen Hangar draw (default run or explicit
    // `--eawr-mesh Hangar`) from the opt-in exploratory Hull preview and from
    // unpinned models; each carries its own mesh, material, texture, hash pins
    // and camera policy.
    auto planned = model_preview::plan_for({options_->model_path, options_->mesh_name,
        options_->texture_path, !options_->animation_path.empty()});
    if (!planned) {
        status_message_ = planned.failure;
        return false;
    }
    const model_preview::Plan& plan = *planned.value;
    auto model_bytes = filesystem->open(options_->model_path);
    if (!model_bytes || !model_preview::hash_matches(
            hash_bytes(model_bytes.value()), plan.expected_model_sha256)) {
        status_message_ = "selected model missing or SHA-256 mismatch";
        return false;
    }
    auto loaded_model = assets::load_model(*filesystem, options_->model_path);
    if (!loaded_model) {
        status_message_ = core::format_diagnostic(loaded_model.error());
        return false;
    }
    model_.source = loaded_model.value().source;
    model_.bones = loaded_model.value().bones;
    const auto selection = model_preview::select_submesh(loaded_model.value(), plan);
    if (!selection) {
        status_message_ = selection.failure;
        return false;
    }
    const assets::Mesh& source_mesh = loaded_model.value().meshes[selection.value->mesh_index];
    const assets::Submesh* selected_submesh =
        &source_mesh.submeshes[selection.value->submesh_index];
    const model_preview::LegacySelection* selected_material = &selection.value->material;
    {
        assets::Mesh mesh = source_mesh;
        mesh.submeshes = {*selected_submesh};
        model_.meshes.push_back(std::move(mesh));
    }
    if (plan.camera == model_preview::CameraPolicy::legacy_mesh_bounds) {
        const float center_x = (source_mesh.bounds_min.x + source_mesh.bounds_max.x) * 0.5F;
        const float center_y = (source_mesh.bounds_min.y + source_mesh.bounds_max.y) * 0.5F;
        const float center_z = (source_mesh.bounds_min.z + source_mesh.bounds_max.z) * 0.5F;
        const float extent_x = source_mesh.bounds_max.x - source_mesh.bounds_min.x;
        const float extent_y = source_mesh.bounds_max.y - source_mesh.bounds_min.y;
        const float extent_z = source_mesh.bounds_max.z - source_mesh.bounds_min.z;
        const float extent = std::max({extent_x, extent_y, extent_z, 1.0F});
        capture_camera_.target = {center_x, center_z, -center_y};
        capture_camera_.eye = {center_x, center_z + extent * 0.35F,
            -center_y + extent * 2.25F};
        capture_camera_.near_plane = std::max(0.01F, extent * 0.01F);
        capture_camera_.far_plane = std::max(100.0F, extent * 20.0F);
    } else if (plan.camera == model_preview::CameraPolicy::hull_bounds_fit) {
        // Trust the bounds only after the rest hierarchy agrees with the
        // renderer's rest placement, then frame the instance the fixed
        // capture actually submits.
        auto rest = model_preview::rest_placement(loaded_model.value(),
            selection.value->mesh_index, selection.value->submesh_index);
        if (!rest) {
            status_message_ = rest.failure;
            return false;
        }
        if (!fixed_capture_snapshot_ || fixed_capture_snapshot_->instances().empty()) {
            status_message_ = "exploratory Hull preview requires the fixed capture instance";
            return false;
        }
        const std::vector<PresentationTransform> placed =
            adapt_snapshot(*fixed_capture_snapshot_);
        const model_preview::Bounds world =
            model_preview::world_bounds(rest.value->asset, placed.front().column_major);
        FixedCamera framing = capture_camera_;
        const Vector2 viewport = get_viewport()->get_visible_rect().size;
        if (viewport.x >= 1.0F && viewport.y >= 1.0F) {
            framing.width = static_cast<std::uint32_t>(viewport.x);
            framing.height = static_cast<std::uint32_t>(viewport.y);
        }
        auto fit = model_preview::fit_camera(world, framing, model_preview::hull_view_direction,
            model_preview::hull_fit_margin);
        if (!fit) {
            status_message_ = fit.failure;
            return false;
        }
        capture_camera_ = fit.value->camera;
        model_preview_ = std::make_unique<ModelPreview>();
        model_preview_->rest = std::move(*rest.value);
        model_preview_->world = world;
        model_preview_->fit = *fit.value;
    }
    if (plan.exploratory) {
        if (!model_preview_) {
            status_message_ = "exploratory preview has no framing evidence";
            return false;
        }
        // Its framing is checked against a read-back even without a capture,
        // so the viewport keeps the framed size whatever the OS window.
        pin_capture_viewport(*get_window(), capture_camera_.width, capture_camera_.height);
        model_preview_->plan = plan;
        model_preview_->mesh_name = source_mesh.name;
        model_preview_->mesh_bone = source_mesh.bone;
        model_preview_->submesh_index = selection.value->submesh_index;
        model_preview_->index_count = selected_submesh->indices.size();
        model_preview_->technique = selected_material->technique;
        model_preview_->pass = selected_material->pass;
        model_preview_->base_texture =
            model_preview::base_texture_name(*selected_submesh).value_or(std::string{});
    }
    selected_model_path_ = options_->model_path;
    selected_model_hash_ = hash_bytes(model_bytes.value());
    selected_program_ = selected_submesh->shader;
    auto texture_path = model_preview::texture_path_for(plan, *selected_submesh);
    if (!texture_path) {
        status_message_ = texture_path.failure;
        return false;
    }
    selected_texture_path_ = std::move(*texture_path.value);
    auto texture_bytes = filesystem.value().open(selected_texture_path_);
    if (!texture_bytes || !model_preview::hash_matches(
            hash_bytes(texture_bytes.value()), plan.expected_texture_sha256)) {
        status_message_ = "selected texture missing or SHA-256 mismatch";
        return false;
    }
    auto loaded_texture = assets::load_texture(filesystem.value(), selected_texture_path_);
    if (!loaded_texture) {
        status_message_ = core::format_diagnostic(loaded_texture.error());
        return false;
    }
    selected_texture_hash_ = hash_bytes(texture_bytes.value());
    if (!options_->animation_path.empty()) {
        auto animation_bytes = filesystem.value().open(options_->animation_path);
        if (!animation_bytes) {
            status_message_ = core::format_diagnostic(animation_bytes.error());
            return false;
        }
        auto loaded_animation = assets::load_animation(filesystem.value(), options_->animation_path);
        if (!loaded_animation) {
            status_message_ = core::format_diagnostic(loaded_animation.error());
            return false;
        }
        auto player = animation::Player::create(model_, &loaded_animation.value());
        if (!player) {
            status_message_ = core::format_diagnostic(player.error());
            return false;
        }
        auto pose = player.value().sample({options_->animation_time_seconds,
            animation::PlaybackMode::loop, 0.0F});
        if (!pose) {
            status_message_ = core::format_diagnostic(pose.error());
            return false;
        }
        animation_ = std::move(loaded_animation.value());
        selected_animation_path_ = options_->animation_path;
        selected_animation_hash_ = hash_bytes(animation_bytes.value());
        animation_player_ = std::move(player.value());
        animation_pose_ = std::move(pose.value());
        animation_time_seconds_ = animation_pose_->sampled_time_seconds;
    } else if (!model_.bones.empty()
        && (!selected_submesh->skin_bones.empty() || model_.meshes.front().bone >= 0)) {
        auto player = animation::Player::create(model_);
        if (!player) {
            status_message_ = core::format_diagnostic(player.error());
            return false;
        }
        auto pose = player.value().sample({});
        if (!pose) {
            status_message_ = core::format_diagnostic(pose.error());
            return false;
        }
        animation_player_ = std::move(player.value());
        animation_pose_ = std::move(pose.value());
    }
    texture_ = std::move(loaded_texture.value());
    material_ = MaterialDescription{
        .schema_version = 1,
        .route = MaterialRoute::legacy_effect,
        .pass = RenderPass::opaque,
        .program = selected_submesh->shader,
        .technique = selected_material->technique,
        .pass_name = selected_material->pass,
        .bindings = {},
    };
    for (const assets::MaterialParameter& parameter : model_.meshes.front().submeshes.front().parameters) {
        material_.bindings.push_back({parameter.name, parameter.value});
    }
    return true;
}

bool ViewerHost::apply_animation_pose(
    const std::shared_ptr<const sim::RenderSnapshot>& snapshot) {
    if (!animation_pose_ || !snapshot || !renderer_) return true;
    for (const sim::RenderInstance& instance : snapshot->instances()) {
        auto applied = renderer_->set_skin_pose(
            instance.entity_id, instance.asset_id, animation_pose_->bones);
        if (!applied) {
            status_message_ = core::format_diagnostic(applied.error());
            return false;
        }
    }
    skin_palette_bound_ = !renderer_->skin_bindings().empty();
    if (!skin_palette_bound_) {
        status_message_ = "animation pose did not produce a live Godot skin binding";
        return false;
    }
    return true;
}

bool ViewerHost::load_replay() {
    const auto replay_bytes = read_bytes(options_->replay_path);
    if (!replay_bytes) {
        status_message_ = "replay fixture could not be read";
        return false;
    }
    replay_hash_ = hash_bytes(*replay_bytes);
    std::vector<std::uint8_t> bytes(replay_bytes->size());
    if (!bytes.empty()) std::memcpy(bytes.data(), replay_bytes->data(), bytes.size());
    auto replay = sim::parse_replay(bytes, options_->replay_path.value());
    if (!replay) {
        status_message_ = core::format_diagnostic(replay.error());
        return false;
    }
    auto world_result = sim::World::create(replay.value());
    if (!world_result) {
        status_message_ = core::format_diagnostic(world_result.error());
        return false;
    }
    sim::World world = std::move(world_result.value());
    snapshots_.push_back(world.snapshot());
    const sim::InlineExecutor executor;
    while (world.completed_tick() < world.final_tick_count()) {
        auto tick = world.step(executor);
        if (!tick) {
            status_message_ = core::format_diagnostic(tick.error());
            return false;
        }
        snapshots_.push_back(tick.value().snapshot);
    }
    // The retained P0 prototype captures on its frame 119 after submitting
    // snapshots_[119 / 100], i.e. replay tick 1. Pin the production fixed
    // capture to that observed tick rather than the previously guessed tick 0.
    if (snapshots_.size() > 1 && !snapshots_[1]->instances().empty()) {
        fixed_capture_snapshot_ = std::make_shared<const sim::RenderSnapshot>(
            snapshots_[1]->completed_tick(),
            std::vector<sim::RenderInstance>{snapshots_[1]->instances().front()});
    }
    return snapshots_.size() > 1;
}
} // namespace eawr::presentation::godot_backend
