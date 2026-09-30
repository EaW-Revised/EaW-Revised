#include "eawr/presentation/camera/constants_source.hpp"

#include <pugixml.hpp>

#include <array>
#include <optional>
#include <string>
#include <utility>

namespace eawr::presentation::camera {
namespace {

constexpr std::string_view source_error = "EAWR-CAMERA-0004";

[[nodiscard]] bool ieq(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto fold = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        };
        if (fold(left[i]) != fold(right[i])) return false;
    }
    return true;
}

[[nodiscard]] bool whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && whitespace(text.front())) text.remove_prefix(1);
    while (!text.empty() && whitespace(text.back())) text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::optional<std::string> whole_text(const pugi::xml_node node) {
    std::string result;
    for (const pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_pcdata && child.type() != pugi::node_cdata) {
            return std::nullopt;
        }
        result += child.value();
    }
    return result;
}

[[nodiscard]] bool logical_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\'
        || path.find(':') != std::string_view::npos) return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t end = path.find_first_of("/\\", start);
        if (path.substr(start, end - start) == "..") return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

[[nodiscard]] core::Diagnostic diagnostic(std::string message) {
    core::Diagnostic result;
    result.code = std::string(source_error);
    result.severity = core::Severity::error;
    result.message = std::move(message);
    return result;
}

[[nodiscard]] bool parse_document(const XmlSource source, pugi::xml_document& document,
                                  std::string& failure) {
    constexpr unsigned int options =
        pugi::parse_default | pugi::parse_doctype | pugi::parse_declaration;
    const auto parsed = document.load_buffer(source.bytes.data(), source.bytes.size(),
                                              options, pugi::encoding_auto);
    if (!parsed) {
        failure = std::string(source.logical_path) + " could not be parsed: " + parsed.description();
        return false;
    }
    for (const pugi::xml_node child : document.children()) {
        if (child.type() == pugi::node_doctype) {
            failure = std::string(source.logical_path) + " contains a forbidden document type declaration";
            return false;
        }
    }
    return true;
}

struct Field final {
    std::string_view tag;
    float Constants::*member;
};

constexpr std::array camera_required{
    Field{"Distance_Min", &Constants::distance_min},
    Field{"Distance_Max", &Constants::distance_max},
    Field{"Distance_Default", &Constants::distance_default},
    Field{"Distance_Per_Mouse_Unit", &Constants::distance_per_mouse_unit},
    Field{"Pitch_Min", &Constants::pitch_min},
    Field{"Pitch_Max", &Constants::pitch_max},
    Field{"Pitch_Default", &Constants::pitch_default},
    Field{"Near_Clip", &Constants::near_clip},
    Field{"Far_Clip", &Constants::far_clip},
};

constexpr std::array camera_optional{
    Field{"Distance_Smooth_Time", &Constants::distance_smooth_time},
    Field{"Pitch_Per_Mouse_Unit", &Constants::pitch_per_mouse_unit},
    Field{"Pitch_Per_Zoom_Unit", &Constants::pitch_per_zoom_unit},
    Field{"Pitch_When_Zoomed_In", &Constants::pitch_when_zoomed_in},
    Field{"Pitch_Zoom_Begin_Fraction", &Constants::pitch_zoom_begin_fraction},
    Field{"Yaw_Min", &Constants::yaw_min},
    Field{"Yaw_Max", &Constants::yaw_max},
    Field{"Yaw_Default", &Constants::yaw_default},
    Field{"Yaw_Per_Mouse_Unit", &Constants::yaw_per_mouse_unit},
    Field{"Fov_Min", &Constants::fov_min},
    Field{"Fov_Max", &Constants::fov_max},
    Field{"Fov_Default", &Constants::fov_default},
    Field{"Fov_Per_Mouse_Unit", &Constants::fov_per_mouse_unit},
    Field{"Location_Follows_Terrain", &Constants::location_follows_terrain},
    Field{"Location_Height_Up_Smooth_Time", &Constants::location_height_up_smooth_time},
    Field{"Location_Height_Down_Smooth_Time", &Constants::location_height_down_smooth_time},
    Field{"Min_Height_Above_Terrain", &Constants::min_height_above_terrain},
};

constexpr std::array scroll_required{
    Field{"Tactical_Min_Scroll_Speed", &Constants::tactical_min_scroll_speed},
    Field{"Tactical_Max_Scroll_Speed", &Constants::tactical_max_scroll_speed},
    Field{"Tactical_Edge_Scroll_Region", &Constants::tactical_edge_scroll_region},
    Field{"Tactical_Offscreen_Scroll_Region", &Constants::tactical_offscreen_scroll_region},
    Field{"Push_Scroll_Speed_Modifier", &Constants::push_scroll_speed_modifier},
    Field{"Scroll_Acceleration_Factor", &Constants::scroll_acceleration_factor},
    Field{"Scroll_Deceleration_Factor", &Constants::scroll_deceleration_factor},
};

class Reader final {
public:
    LoadedConstants loaded;
    std::string failure;

    [[nodiscard]] bool tag(const pugi::xml_node parent, const XmlSource source,
                           const std::string_view definition, const std::string_view name,
                           const bool required, pugi::xml_node& found) {
        for (const pugi::xml_node child : parent.children()) {
            if (child.type() != pugi::node_element || !ieq(child.name(), name)) continue;
            if (found) {
                failure = context(source, definition, name) + " is supplied more than once";
                return false;
            }
            found = child;
        }
        loaded.provenance.push_back(FieldProvenance{std::string(source.logical_path),
            std::string(source.sha256), std::string(definition), std::string(name),
            found ? FieldStatus::supplied : FieldStatus::absent});
        if (required && !found) {
            failure = context(source, definition, name) + " is missing required tag";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool scalar(const pugi::xml_node parent, const XmlSource source,
                              const std::string_view definition, const Field field,
                              const bool required) {
        pugi::xml_node node;
        if (!tag(parent, source, definition, field.tag, required, node)) return false;
        if (!node) return true; // The zero-initialized optional value stays inert.
        const auto text = whole_text(node);
        if (!text) {
            failure = context(source, definition, field.tag) + " must contain only text";
            return false;
        }
        auto value = parse_scalar(*text);
        if (!value) {
            failure = context(source, definition, field.tag) + " is not a finite decimal number";
            return false;
        }
        loaded.constants.*(field.member) = value.value();
        return true;
    }

    [[nodiscard]] static std::string context(const XmlSource source,
                                              const std::string_view definition,
                                              const std::string_view tag_name) {
        return std::string(source.logical_path) + " <" + std::string(definition)
            + "> <" + std::string(tag_name) + ">";
    }
};

} // namespace

core::Result<LoadedConstants> load_constants(const XmlSource tactical_cameras,
                                              const XmlSource game_constants,
                                              const Mode mode) {
    using Result = core::Result<LoadedConstants>;
    if (!logical_path(tactical_cameras.logical_path) || !logical_path(game_constants.logical_path)) {
        return Result::failure(diagnostic("camera XML sources require relative logical paths"));
    }
    pugi::xml_document camera_document;
    pugi::xml_document global_document;
    Reader reader;
    if (!parse_document(tactical_cameras, camera_document, reader.failure)
        || !parse_document(game_constants, global_document, reader.failure)) {
        return Result::failure(diagnostic(std::move(reader.failure)));
    }
    const pugi::xml_node camera_root = camera_document.document_element();
    if (!camera_root || !ieq(camera_root.name(), "TacticalCameras")) {
        return Result::failure(diagnostic(std::string(tactical_cameras.logical_path)
            + " document element is not TacticalCameras"));
    }
    const pugi::xml_node global_root = global_document.document_element();
    if (!global_root) {
        return Result::failure(diagnostic(std::string(game_constants.logical_path)
            + " has no document element"));
    }
    const std::string_view wanted = definition_name(mode);
    if (wanted.empty()) return Result::failure(diagnostic("invalid tactical camera mode"));
    pugi::xml_node definition;
    for (const pugi::xml_node candidate : camera_root.children()) {
        if (candidate.type() != pugi::node_element || !ieq(candidate.name(), "TacticalCamera")
            || !ieq(candidate.attribute("Name").as_string(), wanted)) continue;
        if (definition) {
            return Result::failure(diagnostic(std::string(tactical_cameras.logical_path)
                + " has duplicate <TacticalCamera Name=\"" + std::string(wanted) + "\"> definitions"));
        }
        definition = candidate;
    }
    if (!definition) {
        return Result::failure(diagnostic(std::string(tactical_cameras.logical_path)
            + " has no <TacticalCamera Name=\"" + std::string(wanted) + "\">"));
    }
    for (const Field field : camera_required) {
        if (!reader.scalar(definition, tactical_cameras, wanted, field, true))
            return Result::failure(diagnostic(std::move(reader.failure)));
    }
    for (const Field field : camera_optional) {
        if (!reader.scalar(definition, tactical_cameras, wanted, field, false))
            return Result::failure(diagnostic(std::move(reader.failure)));
    }
    pugi::xml_node splines_flag;
    if (!reader.tag(definition, tactical_cameras, wanted, "Use_Splines", false, splines_flag))
        return Result::failure(diagnostic(std::move(reader.failure)));
    if (splines_flag) {
        const auto text = whole_text(splines_flag);
        const std::string_view value = text ? trim(*text) : std::string_view{};
        if (ieq(value, "yes") || ieq(value, "true") || value == "1") {
            reader.loaded.constants.use_splines = true;
        } else if (!ieq(value, "no") && !ieq(value, "false") && value != "0") {
            return Result::failure(diagnostic(Reader::context(tactical_cameras, wanted, "Use_Splines")
                + " must be yes/true/1 or no/false/0"));
        }
    }
    for (const std::string_view spline_name : {"Distance_Spline", "Pitch_Spline"}) {
        pugi::xml_node spline_node;
        if (!reader.tag(definition, tactical_cameras, wanted, spline_name,
                        reader.loaded.constants.use_splines, spline_node)) {
            return Result::failure(diagnostic(std::move(reader.failure)));
        }
        if (!reader.loaded.constants.use_splines) continue;
        const auto text = whole_text(spline_node);
        if (!text) {
            return Result::failure(diagnostic(Reader::context(tactical_cameras, wanted, spline_name)
                + " must contain only text"));
        }
        auto parsed = parse_spline(*text);
        if (!parsed) {
            return Result::failure(diagnostic(Reader::context(tactical_cameras, wanted, spline_name)
                + " " + parsed.error().message));
        }
        if (spline_name == "Distance_Spline") {
            reader.loaded.constants.distance_spline = std::move(parsed.value());
        } else {
            reader.loaded.constants.pitch_spline = std::move(parsed.value());
        }
    }
    for (const Field field : scroll_required) {
        if (!reader.scalar(global_root, game_constants, global_root.name(), field, true))
            return Result::failure(diagnostic(std::move(reader.failure)));
    }
    if (auto valid = validate(reader.loaded.constants); !valid) {
        return Result::failure(std::move(valid.error()));
    }
    return Result::success(std::move(reader.loaded));
}

namespace {

[[nodiscard]] core::Diagnostic override_diagnostic(std::string message) {
    core::Diagnostic result;
    result.code = std::string(invalid_override);
    result.severity = core::Severity::error;
    result.message = "map camera override: " + std::move(message);
    return result;
}

[[nodiscard]] bool lowercase_sha256(const std::string_view text) {
    if (text.size() != 64) return false;
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

[[nodiscard]] const Field* overridable_field(const std::string_view tag) {
    for (const Field& field : camera_required) if (field.tag == tag) return &field;
    for (const Field& field : camera_optional) if (field.tag == tag) return &field;
    for (const Field& field : scroll_required) if (field.tag == tag) return &field;
    return nullptr;
}

} // namespace

core::Result<LoadedConstants> apply_map_overrides(
    const LoadedConstants& base, const OverrideSource source,
    const std::span<const ConstantOverride> overrides) {
    using Result = core::Result<LoadedConstants>;
    if (source.authority != project_authored_override) {
        return Result::failure(override_diagnostic(
            "authority must be \"project-authored\"; no original map override source is established"));
    }
    if (!logical_path(source.logical_path) || !lowercase_sha256(source.sha256)
        || source.source_id.empty()) {
        return Result::failure(override_diagnostic(
            "source needs a relative logical path, a lowercase SHA-256 and a source ID"));
    }
    if (overrides.empty()) {
        return Result::failure(override_diagnostic("an override block must name at least one field"));
    }
    if (!base.overrides.empty()) {
        return Result::failure(override_diagnostic("constants already carry a map override layer"));
    }
    LoadedConstants result = base;
    bool overrides_scroll_speed{};
    for (std::size_t index = 0; index < overrides.size(); ++index) {
        const ConstantOverride& entry = overrides[index];
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (ieq(overrides[prior].tag, entry.tag)) {
                return Result::failure(override_diagnostic("<" + entry.tag + "> is overridden more than once"));
            }
        }
        if (ieq(entry.tag, "Use_Splines") || ieq(entry.tag, "Distance_Spline")
            || ieq(entry.tag, "Pitch_Spline")) {
            return Result::failure(override_diagnostic("<" + entry.tag
                + "> is not overridable: spline activation and curves stay XML-owned"));
        }
        if (ieq(entry.tag, "Land_Tactical_Camera_Locked")
            || ieq(entry.tag, "Space_Tactical_Camera_Locked")) {
            return Result::failure(override_diagnostic("<" + entry.tag
                + "> is not overridable: camera-lock semantics are unresolved"));
        }
        const Field* field = overridable_field(entry.tag);
        if (!field) {
            return Result::failure(override_diagnostic("<" + entry.tag
                + "> is not a scalar camera constant tag (exact spelling required)"));
        }
        auto value = parse_scalar(entry.value);
        if (!value) {
            return Result::failure(override_diagnostic("<" + entry.tag + "> is not a finite decimal number"));
        }
        // Project override policy, not an XML rule: `validate` keeps the XML
        // baseline's own domain, while an override may not reverse panning.
        const bool scroll_speed = field->member == &Constants::tactical_min_scroll_speed
            || field->member == &Constants::tactical_max_scroll_speed;
        if (scroll_speed && value.value() < 0.0F) {
            return Result::failure(override_diagnostic("<" + entry.tag
                + "> must not be negative: an override cannot reverse panning"));
        }
        overrides_scroll_speed = overrides_scroll_speed || scroll_speed;
        const FieldProvenance* xml_row = nullptr;
        for (const FieldProvenance& row : base.provenance) {
            if (row.tag == field->tag) {
                xml_row = &row;
                break;
            }
        }
        if (!xml_row) {
            return Result::failure(override_diagnostic("<" + entry.tag
                + "> has no XML provenance row to override"));
        }
        result.overrides.push_back(AppliedOverride{std::string(field->tag), *xml_row,
            result.constants.*(field->member), value.value(), std::string(source.logical_path),
            std::string(source.sha256), std::string(source.source_id),
            std::string(source.authority)});
        result.constants.*(field->member) = value.value();
    }
    // Checked against the layered result, so overriding one end of the range
    // below or above the other end's XML value is refused here too.
    if (overrides_scroll_speed
        && result.constants.tactical_max_scroll_speed < result.constants.tactical_min_scroll_speed) {
        return Result::failure(override_diagnostic(
            "overridden Tactical_Max_Scroll_Speed must not be below Tactical_Min_Scroll_Speed"));
    }
    if (auto valid = validate(result.constants); !valid) {
        return Result::failure(std::move(valid.error()));
    }
    return Result::success(std::move(result));
}

} // namespace eawr::presentation::camera
