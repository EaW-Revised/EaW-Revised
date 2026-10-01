#include "map_camera.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace eawr::viewer {
namespace {

[[nodiscard]] core::Diagnostic invalid(std::string message) {
    core::Diagnostic result;
    result.code = "EAWR-CAMERA-0501";
    result.message = std::move(message);
    return result;
}

[[nodiscard]] bool attributes_are(const pugi::xml_node node,
    const std::initializer_list<std::string_view> names) {
    std::size_t count{};
    for (const auto attribute : node.attributes()) {
        ++count;
        bool known{};
        for (const auto name : names) if (attribute.name() == name) known = true;
        if (!known) return false;
    }
    return count == names.size();
}

[[nodiscard]] bool scalar(const pugi::xml_node node, const char* name, float& out) {
    const auto attribute = node.attribute(name);
    if (!attribute) return false;
    auto parsed = presentation::camera::parse_scalar(attribute.value());
    if (!parsed) return false;
    out = parsed.value();
    return true;
}

// How follow_ground treats the target height before the eye floor.
enum class GroundHeight : std::uint8_t {
    ease, // Toward the ground under the target by the XML smooth times.
    snap, // Onto the ground under the target.
    keep, // Unchanged: FoC's view reset leaves the camera location alone.
};

// Terrain following for one controller (see MapCameraBridge::set_ground).
// Render (x, y, z) is source (x, -z, y). The eye floor depends only on the
// target's X/Z, zoom, pitch and yaw, so it is recomputed identically each step.
[[nodiscard]] core::Result<void> follow_ground(presentation::camera::BoundedTacticalController& controller,
    const presentation::camera::Constants& constants, const TerrainGround& ground,
    const float seconds, const GroundHeight mode) {
    if (ground.heights.empty() || constants.location_follows_terrain == 0.0F
        || controller.capture_locked()) {
        return core::Result<void>::success();
    }
    const auto& frame = controller.tactical_frame();
    const auto under_target = ground.at(frame.target[0], -frame.target[2]);
    if (!under_target) return core::Result<void>::failure(invalid("terrain ground has no height at the target"));
    float height = mode == GroundHeight::keep ? frame.target[1] : *under_target;
    if (mode == GroundHeight::ease) {
        const float current = frame.target[1];
        auto eased = presentation::camera::smooth_toward(current, *under_target,
            *under_target > current ? constants.location_height_up_smooth_time
                                    : constants.location_height_down_smooth_time, seconds);
        if (!eased) return core::Result<void>::failure(eased.error());
        height = eased.value();
    }
    if (const auto under_eye = ground.at(frame.eye[0], -frame.eye[2])) {
        // The eye's rise over the target, by the same arithmetic as the frame.
        const std::array<float, 3> origin{};
        const auto rise = presentation::camera::eye_position(std::span<const float, 3>{origin},
            controller.state().distance, controller.state().pitch_degrees, controller.yaw_degrees());
        if (!rise) return core::Result<void>::failure(rise.error());
        height = std::max(height, *under_eye + constants.min_height_above_terrain - rise.value()[1]);
    }
    return controller.set_target_height(height);
}

[[nodiscard]] bool eye_clears_ground(const presentation::camera::BoundedTacticalController& controller,
    const presentation::camera::Constants& constants, const TerrainGround& ground) {
    const auto& eye = controller.frame().eye;
    const auto height = ground.at(eye[0], -eye[2]);
    return height && eye[1] + 1e-3F >= *height + constants.min_height_above_terrain;
}

} // namespace

std::optional<float> TerrainGround::at(const float x, const float y) const {
    if (width < 2 || height < 2 || !std::isfinite(spacing) || !(spacing > 0.0F)
        || heights.size() != static_cast<std::size_t>(width) * height
        || !std::isfinite(x) || !std::isfinite(y)) {
        return std::nullopt;
    }
    const float gx = std::clamp(x / spacing, 0.0F, static_cast<float>(width - 1));
    const float gy = std::clamp(y / spacing, 0.0F, static_cast<float>(height - 1));
    const auto column = std::min(static_cast<std::uint32_t>(gx), width - 2);
    const auto row = std::min(static_cast<std::uint32_t>(gy), height - 2);
    const float fx = gx - static_cast<float>(column);
    const float fy = gy - static_cast<float>(row);
    const auto sample = [this](const std::uint32_t c, const std::uint32_t r) {
        return heights[static_cast<std::size_t>(r) * width + c];
    };
    const float h00 = sample(column, row);
    const float h10 = sample(column + 1, row);
    const float h11 = sample(column + 1, row + 1);
    const float h01 = sample(column, row + 1);
    // Triangles (00, 10, 11) and (00, 11, 01).
    return fx >= fy ? h00 + fx * (h10 - h00) + fy * (h11 - h10)
                    : h00 + fx * (h11 - h01) + fy * (h01 - h00);
}

core::Result<MapCameraConfig> parse_map_camera_config(
    const std::string_view xml, const std::string_view map_path,
    const std::string_view map_sha256, const presentation::camera::Mode mode) {
    using Result = core::Result<MapCameraConfig>;
    if (mode == presentation::camera::Mode::unlocked) {
        return Result::failure(invalid("the unlocked camera mode has no map camera config"));
    }
    // Land keeps schema eawr-map-camera v1/v2; space is a separate schema with
    // only v1, so neither document can be mistaken for the other mode.
    const bool space = mode == presentation::camera::Mode::space;
    const std::string_view schema = space ? "eawr-space-map-camera" : "eawr-map-camera";
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size(), pugi::parse_default)) {
        return Result::failure(invalid("map camera config XML is malformed"));
    }
    const auto root = document.child("map_camera");
    const auto bounds = root.child("bounds");
    const auto initial = root.child("initial");
    const auto bindings = root.child("bindings");
    if (!root || document.document_element() != root
        || !attributes_are(root, {"schema", "version", "provenance", "map_path", "map_sha256"})
        || std::string_view(root.attribute("schema").value()) != schema
        || (std::string_view(root.attribute("version").value()) != "1"
            && (space || std::string_view(root.attribute("version").value()) != "2"))
        || std::string_view(root.attribute("provenance").value()) != "project-authored"
        || std::string_view(root.attribute("map_path").value()) != map_path
        || std::string_view(root.attribute("map_sha256").value()) != map_sha256
        || !bounds || !initial || !bindings || root.first_child() != bounds
        || bounds.next_sibling() != initial
        || initial.next_sibling() != bindings
        || (bindings.next_sibling()
            && (std::string_view(bindings.next_sibling().name()) != "constant_overrides"
                || bindings.next_sibling().next_sibling()))
        || !attributes_are(bounds, {"min_x", "max_x", "min_y", "max_y", "source_id", "authority"})
        || !attributes_are(initial, {"target_x", "target_y", "target_height", "zoom", "yaw_degrees"})
        || !attributes_are(bindings, {"path"})
        || bounds.first_child() || initial.first_child() || bindings.first_child()) {
        return Result::failure(invalid("map camera config schema, identity, provenance or shape is invalid"));
    }
    MapCameraConfig config;
    config.version = std::string_view(root.attribute("version").value()) == "2" ? 2U : 1U;
    config.mode = mode;
    config.map_path = std::string(map_path);
    config.map_sha256 = std::string(map_sha256);
    config.bindings_path = bindings.attribute("path").value();
    config.bounds.logical_path = config.map_path;
    config.bounds.source_id = bounds.attribute("source_id").value();
    config.bounds.authority = bounds.attribute("authority").value();
    if (config.bindings_path.empty() || config.bounds.source_id.empty()
        || config.bounds.authority != "project-authored"
        || !scalar(bounds, "min_x", config.bounds.min_x)
        || !scalar(bounds, "max_x", config.bounds.max_x)
        || !scalar(bounds, "min_y", config.bounds.min_y)
        || !scalar(bounds, "max_y", config.bounds.max_y)
        || !scalar(initial, "target_x", config.target_x)
        || !scalar(initial, "target_y", config.target_y)
        || !scalar(initial, "target_height", config.target_height)
        || !scalar(initial, "zoom", config.zoom)
        || !scalar(initial, "yaw_degrees", config.yaw_degrees)
        || config.zoom < 0.0F || config.zoom > 1.0F) {
        return Result::failure(invalid("map camera bounds or initial pose is invalid"));
    }
    if (const auto overrides = bindings.next_sibling(); overrides) {
        if (!attributes_are(overrides, {"schema", "version", "source_id", "authority"})
            || std::string_view(overrides.attribute("schema").value()) != "eawr-map-camera-overrides"
            || std::string_view(overrides.attribute("version").value()) != "1") {
            return Result::failure(invalid(
                "map camera constant_overrides schema, version or attributes are invalid"));
        }
        config.overrides_source_id = overrides.attribute("source_id").value();
        config.overrides_authority = overrides.attribute("authority").value();
        for (const auto child : overrides.children()) {
            if (child.type() != pugi::node_element || std::string_view(child.name()) != "override"
                || !attributes_are(child, {"tag", "value"}) || child.first_child()) {
                return Result::failure(invalid(
                    "map camera constant_overrides may contain only <override tag value/> elements"));
            }
            const std::string_view tag = child.attribute("tag").value();
            const std::string_view value = child.attribute("value").value();
            if (tag == "Tactical_Overview_Clicks") {
                std::uint32_t clicks{};
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), clicks);
                if (mode != presentation::camera::Mode::space || config.overview_clicks
                    || error != std::errc{} || end != value.data() + value.size()
                    || clicks < 1 || clicks > 1000000) {
                    return Result::failure(invalid("space Tactical_Overview_Clicks override must be a unique whole number from 1 to 1000000"));
                }
                config.overview_clicks = clicks;
            } else {
                config.constant_overrides.push_back(presentation::camera::ConstantOverride{
                    std::string(tag), std::string(value)});
            }
        }
        // Scalar camera tags are checked by camera::apply_map_overrides;
        // the space overview count is validated above. A present block must
        // change at least one of the two constant sets.
        if (config.constant_overrides.empty() && !config.overview_clicks) {
            return Result::failure(invalid("map camera constant_overrides names no field"));
        }
        // With no tactical scalar to apply, resolve_map_constants has no
        // override layer to validate this source's identity and authority.
        if (config.constant_overrides.empty()
            && (config.overrides_source_id.empty() || config.overrides_authority != "project-authored")) {
            return Result::failure(invalid("map camera overview override needs project-authored provenance"));
        }
    }
    return Result::success(std::move(config));
}

core::Result<MapCameraConfig> skirmish_camera_config(
    const assets::Map& map, const skirmish::SkirmishStart& start, const sim::tactical::PlayerId local_player) {
    using Result = core::Result<MapCameraConfig>;
    if (!map.declared_extents || !std::isfinite(map.declared_extents->first)
        || !std::isfinite(map.declared_extents->second)
        || map.declared_extents->first <= 0 || map.declared_extents->second <= 0) {
        return Result::failure(invalid("live skirmish camera needs positive declared map extents or a camera XML"));
    }
    const auto marker = std::find_if(start.markers.begin(), start.markers.end(), [&](const auto& entry) {
        return entry.use == skirmish::MarkerUse::spawn && entry.player == local_player;
    });
    if (marker == start.markers.end()) return Result::failure(invalid("live skirmish camera has no local spawn marker"));
    const auto number = [](const skirmish::Fixed value) {
        return static_cast<double>(value.raw()) / static_cast<double>(skirmish::Fixed::scale);
    };
    double x = 0, y = 0;
    std::size_t count = 0;
    for (const auto& unit : start.units) {
        if (unit.state.owner != local_player || unit.record != marker->record
            || (unit.role != skirmish::UnitRole::fleet && unit.role != skirmish::UnitRole::free_unit)) continue;
        x += number(unit.state.position.x);
        y += number(unit.state.position.y);
        ++count;
    }
    MapCameraConfig config;
    config.mode = presentation::camera::Mode::space;
    config.map_path = start.map;
    config.map_sha256 = start.map_sha256;
    config.bindings_path = "space-live-camera-bindings.json";
    const float half_x = map.declared_extents->first * 0.5F;
    const float half_y = map.declared_extents->second * 0.5F;
    config.bounds = {-half_x, half_x, -half_y, half_y, start.map, "skirmish-declared-extents", "project-authored"};
    config.target_x = std::clamp(static_cast<float>(count ? x / static_cast<double>(count) : number(marker->position.x)), -half_x, half_x);
    config.target_y = std::clamp(static_cast<float>(count ? y / static_cast<double>(count) : number(marker->position.y)), -half_y, half_y);
    config.overrides_source_id = "space-camera-owner-deviations";
    config.overrides_authority = "project-authored";
    config.constant_overrides = {{"Distance_Min", "100"}, {"Tactical_Min_Scroll_Speed", "823.529412"}, {"Pitch_Min", "-60"}};
    config.overview_clicks = 5;
    return Result::success(std::move(config));
}

core::Result<presentation::camera::LoadedConstants> resolve_map_constants(
    const MapCameraConfig& config, presentation::camera::LoadedConstants xml,
    const std::string_view config_name, const std::string_view config_sha256) {
    using Result = core::Result<presentation::camera::LoadedConstants>;
    if (config.constant_overrides.empty()) return Result::success(std::move(xml));
    auto layered = presentation::camera::apply_map_overrides(xml,
        {config_name, config_sha256, config.overrides_source_id, config.overrides_authority},
        config.constant_overrides);
    if (!layered) return layered;
    // Dry run of the controller the bridge will build from these constants,
    // with the same bounds and initial pose. `create` only requires a nonempty
    // viewport, so 1x1 accepts and refuses exactly what the real one does.
    auto probe = presentation::camera::BoundedTacticalController::create(
        layered.value().constants, config.bounds,
        {config.target_x, config.target_height, -config.target_y},
        config.zoom, config.yaw_degrees, 1, 1);
    if (!probe) {
        core::Diagnostic refused;
        refused.code = std::string(presentation::camera::invalid_override);
        refused.severity = core::Severity::error;
        refused.message = "map camera override: the tactical controller refuses the overridden "
            "constants for this config's bounds and initial pose (" + probe.error().code + ": "
            + probe.error().message + ")";
        return Result::failure(std::move(refused));
    }
    return layered;
}

std::string_view map_override_inert_reason(const std::string_view tag, const bool use_splines) {
    if (tag == "Distance_Default") return "the map config's initial zoom sets the starting distance";
    if (tag == "Yaw_Default") return "the map config's initial yaw_degrees sets the starting and reset yaw";
    if (tag == "Fov_Per_Mouse_Unit") return "no map camera action changes the field of view";
    if (use_splines && (tag == "Pitch_Min" || tag == "Pitch_Max" || tag == "Pitch_Default"
            || tag == "Pitch_Per_Zoom_Unit" || tag == "Pitch_When_Zoomed_In"
            || tag == "Pitch_Zoom_Begin_Fraction")) {
        return "Use_Splines is set, so Pitch_Spline supersedes this pitch field";
    }
    return {};
}

std::string map_overview_source_members(const MapCameraSource& source) {
    if (!source.overview_base_clicks) return {};
    using camera_input::json_string_literal;
    const auto base = *source.overview_base_clicks;
    const auto effective = source.config.overview_clicks.value_or(base);
    std::ostringstream out;
    out << ", \"overview_clicks\": {\"value\": " << effective
        << ", \"replaced_value\": " << base
        << ", \"override\": ";
    if (source.config.overview_clicks) {
        out << "{\"file\": " << json_string_literal(source.config_file)
            << ", \"source_sha256\": " << json_string_literal(source.config_sha256)
            << ", \"source_id\": " << json_string_literal(source.config.overrides_source_id)
            << ", \"authority\": " << json_string_literal(source.config.overrides_authority) << '}';
    } else {
        out << "null";
    }
    out << ", \"replaced\": {\"file\": \"data/xml/tacticalcameras.xml\""
        << ", \"source_sha256\": " << json_string_literal(source.tactical_xml_sha256)
        << ", \"definition\": \"Space_Mode\", \"tag\": \"Tactical_Overview_Clicks\"}}";
    return out.str();
}

std::string map_camera_source_members(
    const std::span<const presentation::camera::AppliedOverride> overrides,
    const presentation::camera::Constants& constants,
    const camera_input::BindingTable* table) {
    using camera_input::json_string_literal;
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    const auto status = [](const presentation::camera::FieldStatus value) {
        return value == presentation::camera::FieldStatus::supplied ? "\"supplied\"" : "\"absent\"";
    };
    // Tags read only through an input action (the ledger's rates), as opposed
    // to Distance_Min/Max, which the solver reads on every frame.
    constexpr std::array<std::string_view, 8> action_only_rates{
        "Tactical_Min_Scroll_Speed", "Tactical_Max_Scroll_Speed", "Push_Scroll_Speed_Modifier",
        "Distance_Per_Mouse_Unit", "Yaw_Per_Mouse_Unit", "Pitch_Per_Mouse_Unit",
        "Tactical_Edge_Scroll_Region", "Tactical_Offscreen_Scroll_Region"};
    const auto bound_rate = [table](const std::string_view tag) {
        if (!table) return false;
        const auto names = [tag](const std::span<const std::string_view> tags) {
            return std::find(tags.begin(), tags.end(), tag) != tags.end();
        };
        if (table->edge_scroll && names(camera_input::edge_scroll_rate_tags())) return true;
        for (const auto& binding : table->bindings) {
            if (names(camera_input::rate_source_tags(binding.action))) return true;
        }
        return false;
    };
    const auto effect = [&](const std::string_view tag) -> std::pair<std::string_view, std::string_view> {
        if (const auto reason = map_override_inert_reason(tag, constants.use_splines); !reason.empty()) {
            return {"inert", reason};
        }
        if (std::find(action_only_rates.begin(), action_only_rates.end(), tag) != action_only_rates.end()
            && !bound_rate(tag)) {
            return {"unbound", "only an input the active binding table does not bind reads this rate"};
        }
        return {"consumed", "read by the bounded tactical controller"};
    };
    out << ", \"constant_precedence\": [\"effective-vfs-xml\", \"project-authored-map-override\", "
           "\"fixed-capture-frame\"]"
        << ", \"constant_overrides\": [";
    for (std::size_t index = 0; index < overrides.size(); ++index) {
        const auto& entry = overrides[index];
        out << (index == 0 ? "" : ", ") << "{\"tag\": " << json_string_literal(entry.tag)
            << ", \"value\": " << entry.value << ", \"replaced_value\": " << entry.base_value
            << ", \"file\": " << json_string_literal(entry.file)
            << ", \"source_sha256\": " << json_string_literal(entry.source_sha256)
            << ", \"source_id\": " << json_string_literal(entry.source_id)
            << ", \"authority\": " << json_string_literal(entry.authority)
            << ", \"effect\": " << json_string_literal(effect(entry.tag).first)
            << ", \"effect_reason\": " << json_string_literal(effect(entry.tag).second)
            << ", \"replaced\": {\"file\": " << json_string_literal(entry.base.file)
            << ", \"source_sha256\": " << json_string_literal(entry.base.source_sha256)
            << ", \"definition\": " << json_string_literal(entry.base.definition)
            << ", \"status\": " << status(entry.base.status) << "}}";
    }
    out << "], \"input_defaults\": {\"control_provenance\": "
        << json_string_literal(table ? table->provenance : "none")
        << ", \"original_binding_source\": "
        << json_string_literal(camera_input::original_binding_source)
        << ", \"edge_scroll_rate_tags\": [";
    if (table && table->edge_scroll) {
        const auto tags = camera_input::edge_scroll_rate_tags();
        for (std::size_t index = 0; index < tags.size(); ++index) {
            out << (index == 0 ? "" : ", ") << json_string_literal(tags[index]);
        }
    }
    out << "], \"actions\": [";
    if (table) {
        // One row per distinct bound action, in first-binding order.
        std::vector<camera_input::Action> seen;
        for (const auto& binding : table->bindings) {
            if (std::find(seen.begin(), seen.end(), binding.action) != seen.end()) continue;
            seen.push_back(binding.action);
            out << (seen.size() == 1 ? "" : ", ") << "{\"action\": "
                << json_string_literal(camera_input::to_string(binding.action))
                << ", \"rate_tags\": [";
            const auto tags = camera_input::rate_source_tags(binding.action);
            for (std::size_t index = 0; index < tags.size(); ++index) {
                out << (index == 0 ? "" : ", ") << json_string_literal(tags[index]);
            }
            out << "]}";
        }
    }
    out << "], \"pan_speed_scale\": " << (table ? table->pan_speed_scale : 1.0F) << "}";
    return out.str();
}

core::Result<void> MapCameraBridge::activate(
    MapCameraConfig config, presentation::camera::Constants constants,
    const std::string_view bindings_json, const std::uint32_t width,
    const std::uint32_t height,
    const std::optional<presentation::camera::TacticalFrame> fixed_capture) {
    const bool space = context_ == camera_input::Context::space;
    if (context_ == camera_input::Context::free || config.mode
            != (space ? presentation::camera::Mode::space : presentation::camera::Mode::land)) {
        return core::Result<void>::failure(invalid(space
            ? "space map camera requires a space map camera config"
            : "land map camera requires a land map camera config"));
    }
    auto table = camera_input::parse_binding_table(bindings_json);
    if (!table) return core::Result<void>::failure(table.error());
    if (space) {
        // No free-flight policy is defined for a space map: a free context,
        // free action, entry toggle or free settings block is refused rather
        // than silently ignored.
        if (table.value().free_camera) {
            return core::Result<void>::failure(invalid(
                "space map camera rejects unsupported free-camera bindings"));
        }
        for (const auto& binding : table.value().bindings) {
            if (binding.context == camera_input::Context::free
                || binding.action == camera_input::Action::free_toggle) {
                return core::Result<void>::failure(invalid(
                    "space map camera rejects unsupported free-camera bindings"));
            }
            if (binding.context != camera_input::Context::space) {
                return core::Result<void>::failure(invalid(
                    "space map camera rejects incompatible land bindings"));
            }
        }
        if (table.value().bindings.empty()) {
            return core::Result<void>::failure(invalid("space map camera binds no space control"));
        }
    } else {
        for (const auto& binding : table.value().bindings) {
            if (binding.context == camera_input::Context::space
                || (config.version == 1U && (binding.action == camera_input::Action::free_toggle
                    || binding.context != camera_input::Context::land))) {
                return core::Result<void>::failure(invalid(
                    "land map camera does not support space bindings or v1 free-toggle"));
            }
        }
    }
    if (table.value().version != config.version) {
        return core::Result<void>::failure(invalid("map camera config and bindings versions differ"));
    }
    auto created = presentation::camera::BoundedTacticalController::create(
        constants, config.bounds,
        {config.target_x, config.target_height, -config.target_y},
        config.zoom, config.yaw_degrees, width, height);
    if (!created) return core::Result<void>::failure(created.error());
    if (auto grounded = follow_ground(created.value(), constants, ground_, 0.0F, GroundHeight::snap); !grounded) {
        return grounded;
    }
    if (fixed_capture) {
        if (auto locked = created.value().lock_capture(*fixed_capture); !locked) {
            return core::Result<void>::failure(locked.error());
        }
    }
    // Stage the adapter so a late failure leaves the active one untouched.
    camera_input::Adapter adapter = adapter_;
    if (auto activated = adapter.activate(bindings_json); !activated) {
        return core::Result<void>::failure(activated.error());
    }
    if (auto viewport = adapter.set_viewport(static_cast<float>(width),
            static_cast<float>(height)); !viewport) {
        return core::Result<void>::failure(viewport.error());
    }
    if (fixed_capture) adapter.set_capture_locked(true);
    adapter_ = std::move(adapter);
    config_ = std::move(config);
    constants_ = std::move(constants);
    controller_ = std::move(created.value());
    focus_held_ = false;
    free_settings_ = table.value().free_camera;
    return core::Result<void>::success();
}

core::Result<void> MapCameraBridge::toggle_free() {
    if (free_controller_) {
        free_controller_.reset();
        adapter_.set_context(context_);
        controller_->stop_pan();
        ++free_exits_;
        free_transitions_.emplace_back("exit");
        return core::Result<void>::success();
    }
    if (!free_settings_) {
        ++free_rejections_;
        free_rejection_ = "free-camera settings absent";
        free_transitions_.emplace_back("rejected");
        return core::Result<void>::success();
    }
    auto created = presentation::camera::FreeCameraController::create(*free_settings_,
        std::span<const float, 3>{controller_->frame().eye}, controller_->yaw_degrees(),
        controller_->state().pitch_degrees);
    if (!created) {
        ++free_rejections_;
        free_rejection_ = core::format_diagnostic(created.error());
        free_transitions_.emplace_back("rejected");
        return core::Result<void>::success();
    }
    saved_tactical_frame_ = controller_->frame();
    controller_->stop_pan();
    free_frame_ = saved_tactical_frame_;
    free_controller_.emplace(std::move(created.value()));
    free_pose_ = free_controller_->pose();
    adapter_.set_context(camera_input::Context::free);
    ++free_entries_;
    free_transitions_.emplace_back("enter");
    return core::Result<void>::success();
}

core::Result<void> MapCameraBridge::step(const float seconds) {
    if (!controller_) return core::Result<void>::failure(invalid("map camera is not active"));
    if (controller_->capture_locked()) return core::Result<void>::success();
    // Rejected before any pending input is consumed.
    if (!std::isfinite(seconds) || seconds < 0.0F) {
        return core::Result<void>::failure(invalid("map camera step duration is invalid"));
    }
    auto intent = adapter_.take_step(constants_);
    if (!intent) return core::Result<void>::failure(intent.error());
    if (intent.value().free_toggle_requests) {
        auto toggled = toggle_free();
        if (toggled) ++steps_;
        return toggled;
    }
    if (free_controller_) {
        const auto before = free_controller_->pose();
        const presentation::camera::FreeCameraIntent movement{
            intent.value().free_move_x, intent.value().free_move_z, intent.value().free_move_y,
            intent.value().free_look_yaw_units, intent.value().free_look_pitch_units};
        if (auto advanced = free_controller_->advance(movement, seconds); !advanced) return advanced;
        const auto& pose = free_controller_->pose();
        free_pose_ = pose;
        ++free_steps_;
        ++steps_;
        if (pose == before) return core::Result<void>::success();
        auto forward = presentation::camera::free_camera_forward(pose);
        if (!forward) return core::Result<void>::failure(forward.error());
        const float reach = std::max(controller_->state().distance, 1.0F);
        free_frame_.eye = pose.eye;
        free_frame_.target = {pose.eye[0] + forward.value()[0] * reach,
            pose.eye[1] + forward.value()[1] * reach,
            pose.eye[2] + forward.value()[2] * reach};
        return core::Result<void>::success();
    }
    camera_input::StepIntent consumed = intent.value();
    if (overview_view_yaw_) {
        consumed.rotate_units = 0.0F;
        consumed.orbit_pitch_units = 0.0F;
    }
    MapCameraTrace entry{
        .step = steps_ + 1U,
        .seconds = seconds,
        .pan_x = consumed.pan_x,
        .pan_y = consumed.pan_y,
        .drag_x = consumed.drag_x,
        .drag_y = consumed.drag_y,
        .push_scroll = consumed.push_scroll,
        .zoom_detents = consumed.zoom_detents,
        .rotate_units = consumed.rotate_units,
        .orbit_pitch_units = consumed.orbit_pitch_units,
        .translate_x = consumed.translate_x,
        .translate_y = consumed.translate_y,
        .resets = consumed.reset_requests,
        .view_resets = consumed.view_reset_requests,
        .target_before = controller_->frame().target,
        .target_after = {},
        .zoom_before = controller_->state().zoom,
        .zoom_after = {},
        .yaw_before = controller_->yaw_degrees(),
        .yaw_after = {},
        .pitch_before = controller_->state().pitch_degrees,
        .pitch_after = {},
    };
    // Only steps that consumed tactical input are traced; idle frames are not.
    const auto record = [&] {
        if (consumed.pan_x == 0.0F && consumed.pan_y == 0.0F && consumed.drag_x == 0.0F
            && consumed.drag_y == 0.0F && consumed.zoom_detents == 0.0F
            && consumed.rotate_units == 0.0F && consumed.orbit_pitch_units == 0.0F
            && consumed.translate_x == 0.0F && consumed.translate_y == 0.0F
            && consumed.reset_requests == 0U && consumed.view_reset_requests == 0U) return;
        entry.target_after = controller_->frame().target;
        entry.zoom_after = controller_->state().zoom;
        entry.yaw_after = controller_->yaw_degrees();
        entry.pitch_after = controller_->state().pitch_degrees;
        if (trace_.size() < max_trace_entries) trace_.push_back(entry);
        else ++trace_dropped_;
    };
    if (consumed.reset_requests || consumed.view_reset_requests) {
        // Home restores the authored pose and snaps it onto the ground. A click
        // reset (FoC's middle click) restores zoom, yaw and pitch and keeps the
        // current target, height included: FoC's reset leaves the camera
        // location and its smoothed height alone. Only the eye floor, which FoC
        // applies every frame, may still raise it.
        const bool full = consumed.reset_requests != 0U;
        const auto& current = controller_->tactical_frame();
        const std::array<float, 3> target = full
            ? std::array<float, 3>{config_.target_x, config_.target_height, -config_.target_y}
            : current.target;
        auto reset = presentation::camera::BoundedTacticalController::create(
            constants_, config_.bounds, target,
            config_.zoom, config_.yaw_degrees, current.width, current.height);
        if (!reset) return core::Result<void>::failure(reset.error());
        if (auto grounded = follow_ground(reset.value(), constants_, ground_, 0.0F,
                full ? GroundHeight::snap : GroundHeight::keep); !grounded) {
            return grounded;
        }
        controller_ = std::move(reset.value());
        focus_held_ = false;
        ++(full ? resets_ : view_resets_);
        ++steps_;
        record();
        return core::Result<void>::success();
    }
    const presentation::camera::TacticalStep step{
        .pan = {consumed.pan_x, consumed.pan_y, consumed.push_scroll},
        .wheel_detents = consumed.zoom_detents,
        .yaw_mouse_units = consumed.rotate_units,
        .delta_seconds = seconds,
        .drag = {consumed.drag_x, consumed.drag_y},
        .orbit_pitch_units = consumed.orbit_pitch_units,
        // The controller tilts at the XML rate; the land project rate (#348)
        // adds the difference as a host tilt, clamped like the orbit.
        .pitch_adjust_degrees = (orbit_pitch_per_mouse_unit() - constants_.pitch_per_mouse_unit)
            * consumed.orbit_pitch_units,
        .translate_units = {consumed.translate_x, consumed.translate_y},
        .pan_speed_scale = adapter_.table()->pan_speed_scale,
        .orbit_pitch_range = orbit_pitch_range(),
        .view_yaw_degrees = overview_view_yaw_,
    };
    // Advance and follow on a copy, so a failure leaves the controller as it was.
    auto next = *controller_;
    if (auto advanced = next.advance(step); !advanced) return advanced;
    const bool focus_preserving_step = (consumed.rotate_units != 0.0F
        || consumed.orbit_pitch_units != 0.0F || consumed.zoom_detents != 0.0F
        || next.state().distance != controller_->state().distance)
        && consumed.pan_x == 0.0F && consumed.pan_y == 0.0F
        && consumed.drag_x == 0.0F && consumed.drag_y == 0.0F
        && consumed.translate_x == 0.0F && consumed.translate_y == 0.0F;
    const bool target_moved = next.frame().target[0] != controller_->frame().target[0]
        || next.frame().target[2] != controller_->frame().target[2];
    bool focus_held = target_moved ? false : focus_held_;
    if (focus_preserving_step && !target_moved && follows_ground()) {
        // Keep the existing focus through orbit and zoom. Raising the target
        // for eye clearance would slide it across the screen; limit elevation instead.
        if (!eye_clears_ground(next, constants_, ground_)) {
            const auto at_pitch = [&](const float pitch)
                -> std::optional<presentation::camera::BoundedTacticalController> {
                auto trial = *controller_;
                auto adjusted = step;
                adjusted.pitch_adjust_degrees += pitch - next.state().pitch_degrees;
                if (!trial.advance(adjusted)) return std::nullopt;
                return trial;
            };
            float low = next.state().pitch_degrees;
            float high = orbit_pitch_range().max_degrees;
            auto clear = at_pitch(high);
            if (!clear || !eye_clears_ground(*clear, constants_, ground_)) {
                return core::Result<void>::failure(invalid("orbit cannot clear terrain at its pitch limit"));
            }
            for (int iteration = 0; iteration < 20; ++iteration) {
                const float middle = (low + high) * 0.5F;
                auto trial = at_pitch(middle);
                if (trial && eye_clears_ground(*trial, constants_, ground_)) {
                    high = middle;
                    clear = std::move(trial);
                } else {
                    low = middle;
                }
            }
            next = std::move(*clear);
        }
    } else if (!focus_held || target_moved) {
        if (auto grounded = follow_ground(next, constants_, ground_, seconds, GroundHeight::ease); !grounded) {
            return grounded;
        }
    }
    if (focus_preserving_step && !target_moved && follows_ground()) focus_held = true;
    controller_ = std::move(next);
    focus_held_ = focus_held;
    ++steps_;
    record();
    return core::Result<void>::success();
}

core::Result<void> MapCameraBridge::set_viewport(const float width, const float height) {
    if (!controller_) return core::Result<void>::failure(invalid("map camera is not active"));
    if (controller_->capture_locked()) return core::Result<void>::success();
    if (!std::isfinite(width) || !std::isfinite(height)
        || width < 0.0F || height < 0.0F
        || static_cast<double>(width) > std::numeric_limits<std::uint32_t>::max()
        || static_cast<double>(height) > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<void>::failure(invalid("map camera viewport is invalid"));
    }
    if (width == 0.0F || height == 0.0F) {
        auto suspended = adapter_.set_viewport(0.0F, 0.0F);
        if (suspended) controller_->stop_pan();
        return suspended;
    }
    if (width < 1.0F || height < 1.0F
        || std::floor(width) != width || std::floor(height) != height) {
        return core::Result<void>::failure(invalid("map camera viewport needs whole, positive pixels"));
    }
    if (auto resized = controller_->set_viewport(static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height)); !resized) return resized;
    controller_->stop_pan();
    if (free_controller_) {
        free_frame_.width = static_cast<std::uint32_t>(width);
        free_frame_.height = static_cast<std::uint32_t>(height);
        saved_tactical_frame_ = controller_->frame();
    }
    return adapter_.set_viewport(width, height);
}

core::Result<void> MapCameraBridge::handle(const camera_input::RawEvent& event) {
    ++input_callbacks_;
    auto handled = adapter_.handle(event);
    if (!handled) rejected_event_ = core::format_diagnostic(handled.error());
    return handled;
}

} // namespace eawr::viewer
