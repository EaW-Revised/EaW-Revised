#include "camera_binding_config_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <set>
#include <tuple>

namespace eawr::viewer::camera_input {
namespace {

[[nodiscard]] core::Result<BindingTable> reject(
    const std::string_view code, std::string message) {
    return core::Result<BindingTable>::failure(failure(code, std::move(message)));
}

[[nodiscard]] bool only_keys(const JsonValue& object,
                             const std::initializer_list<std::string_view> allowed,
                             std::string& unknown) {
    for (const auto& [name, value] : object.members) {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            unknown = name;
            return false;
        }
    }
    return true;
}

// The v2 free_camera block: exactly the five project-authored settings, each a
// JSON number, then the controller's own validation. No field is defaulted.
[[nodiscard]] core::Result<camera::FreeCameraSettings> parse_free_camera(
    const JsonValue* block) {
    using Parsed = core::Result<camera::FreeCameraSettings>;
    if (!block || block->kind != JsonValue::Kind::object) {
        return Parsed::failure(failure(diagnostic_codes::invalid_bindings,
            "schema v2 requires a free_camera settings object"));
    }
    std::string unknown;
    if (!only_keys(*block, {"move_speed", "vertical_speed", "look_degrees_per_unit",
                            "pitch_min_degrees", "pitch_max_degrees"}, unknown)) {
        return Parsed::failure(failure(diagnostic_codes::invalid_bindings,
            "free_camera has unknown key \"" + unknown + "\""));
    }
    camera::FreeCameraSettings settings;
    const std::array<std::pair<std::string_view, float*>, 5> fields{{
        {"move_speed", &settings.move_speed},
        {"vertical_speed", &settings.vertical_speed},
        {"look_degrees_per_unit", &settings.look_degrees_per_unit},
        {"pitch_min_degrees", &settings.pitch_min_degrees},
        {"pitch_max_degrees", &settings.pitch_max_degrees},
    }};
    for (const auto& [name, target] : fields) {
        const JsonValue* value = block->find(name);
        if (!value || value->kind != JsonValue::Kind::number) {
            return Parsed::failure(failure(diagnostic_codes::invalid_bindings,
                "free_camera." + std::string(name) + " must be a number"));
        }
        *target = static_cast<float>(value->number);
    }
    if (auto valid = camera::validate(settings); !valid) {
        return Parsed::failure(failure(diagnostic_codes::invalid_bindings,
            "free_camera settings rejected: " + valid.error().code + ": "
                + valid.error().message));
    }
    return Parsed::success(settings);
}

} // namespace

core::Result<BindingTable> parse_binding_table(const std::string_view json) {
    if (json.size() > max_binding_document_bytes) {
        return reject(diagnostic_codes::invalid_bindings, "binding document exceeds 1 MiB");
    }
    JsonValue document;
    JsonReader reader(json);
    if (!reader.read_document(document)) {
        return reject(diagnostic_codes::invalid_bindings, reader.error());
    }
    if (document.kind != JsonValue::Kind::object) {
        return reject(diagnostic_codes::invalid_bindings, "binding document must be an object");
    }

    // Schema identity and version first, so a future file gets a version
    // diagnostic instead of a misleading field complaint.
    const JsonValue* schema = document.find("schema");
    if (!schema || schema->kind != JsonValue::Kind::string || schema->text != bindings_schema) {
        return reject(diagnostic_codes::unsupported_bindings_version,
            "binding document schema must be \"" + std::string(bindings_schema) + "\"");
    }
    const JsonValue* version = document.find("version");
    const bool is_v1 = version && version->kind == JsonValue::Kind::number && version->integral
        && version->number == static_cast<double>(bindings_version);
    const bool is_v2 = version && version->kind == JsonValue::Kind::number && version->integral
        && version->number == static_cast<double>(bindings_version_v2);
    if (!is_v1 && !is_v2) {
        return reject(diagnostic_codes::unsupported_bindings_version,
            "binding document version must be the integer " + std::to_string(bindings_version)
                + " or " + std::to_string(bindings_version_v2));
    }
    const std::int64_t document_version = is_v1 ? bindings_version : bindings_version_v2;
    std::string unknown;
    const bool known_keys = is_v1
        ? only_keys(document, {"schema", "version", "provenance", "notice", "edge_scroll",
                               "pan_speed_scale", "click_reset", "screen_mouse_units", "bindings"},
                    unknown)
        : only_keys(document, {"schema", "version", "provenance", "notice", "edge_scroll",
                               "pan_speed_scale", "click_reset", "screen_mouse_units", "free_camera",
                               "bindings"},
                    unknown);
    if (!known_keys) {
        return reject(diagnostic_codes::invalid_bindings,
            "unknown top-level key \"" + unknown + "\"");
    }

    BindingTable table;
    table.version = document_version;
    const JsonValue* provenance = document.find("provenance");
    if (!provenance || provenance->kind != JsonValue::Kind::string
        || provenance->text != project_authored_provenance) {
        return reject(diagnostic_codes::invalid_bindings,
            "provenance must be \"project-authored\"; schema v"
                + std::to_string(document_version) + " cannot carry retail evidence");
    }
    table.provenance = provenance->text;
    const JsonValue* notice = document.find("notice");
    if (!notice || notice->kind != JsonValue::Kind::string || notice->text.empty()) {
        return reject(diagnostic_codes::invalid_bindings, "notice must be a nonempty string");
    }
    table.notice = notice->text;
    const JsonValue* edge = document.find("edge_scroll");
    if (!edge || edge->kind != JsonValue::Kind::boolean) {
        return reject(diagnostic_codes::invalid_bindings, "edge_scroll must be a boolean");
    }
    table.edge_scroll = edge->boolean;
    if (const JsonValue* pan_scale = document.find("pan_speed_scale")) {
        // A project gain on the XML pan speed; zero or negative would stop or
        // reverse panning, so only a finite positive number is accepted.
        const float value = pan_scale->kind == JsonValue::Kind::number
            ? static_cast<float>(pan_scale->number) : 0.0F;
        if (!finite(value) || !(value > 0.0F)) {
            return reject(diagnostic_codes::invalid_bindings,
                "pan_speed_scale must be a finite positive number");
        }
        table.pan_speed_scale = value;
    }
    if (const JsonValue* click = document.find("click_reset")) {
        if (click->kind != JsonValue::Kind::boolean) {
            return reject(diagnostic_codes::invalid_bindings, "click_reset must be a boolean");
        }
        table.click_reset = click->boolean;
    }
    if (const JsonValue* units = document.find("screen_mouse_units")) {
        if (units->kind != JsonValue::Kind::boolean) {
            return reject(diagnostic_codes::invalid_bindings, "screen_mouse_units must be a boolean");
        }
        table.screen_mouse_units = units->boolean;
    }
    if (is_v2) {
        auto settings = parse_free_camera(document.find("free_camera"));
        if (!settings) return core::Result<BindingTable>::failure(settings.error());
        table.free_camera = settings.value();
    }
    const JsonValue* bindings = document.find("bindings");
    if (!bindings || bindings->kind != JsonValue::Kind::array || bindings->items.empty()) {
        return reject(diagnostic_codes::invalid_bindings, "bindings must be a nonempty array");
    }

    std::set<std::string> ids;
    using Chord = std::tuple<Context, Device, std::uint32_t, std::uint8_t>;
    std::map<Chord, std::string> chords;
    for (std::size_t index = 0; index < bindings->items.size(); ++index) {
        const JsonValue& entry = bindings->items[index];
        std::string where = "bindings[" + std::to_string(index) + "]";
        if (entry.kind != JsonValue::Kind::object) {
            return reject(diagnostic_codes::invalid_bindings, where + " must be an object");
        }
        if (!only_keys(entry, {"id", "action", "context", "device", "control", "modifiers",
                               "trigger", "scale"}, unknown)) {
            return reject(diagnostic_codes::invalid_bindings,
                where + " has unknown key \"" + unknown + "\"");
        }
        const auto text_field = [&](const std::string_view key) -> const std::string* {
            const JsonValue* value = entry.find(key);
            if (!value || value->kind != JsonValue::Kind::string) return nullptr;
            return &value->text;
        };
        const std::string* id = text_field("id");
        if (!id || !valid_id(*id)) {
            return reject(diagnostic_codes::invalid_bindings,
                where + ".id must match [a-z0-9][a-z0-9._-]{0,63}");
        }
        where += " (" + *id + ")";
        if (!ids.insert(*id).second) {
            return reject(diagnostic_codes::invalid_bindings, where + " duplicates an id");
        }

        Binding binding;
        binding.id = *id;
        const std::string* action = text_field("action");
        const auto parsed_action =
            action ? parse_action(*action, document_version) : std::nullopt;
        if (!parsed_action) {
            return reject(diagnostic_codes::invalid_bindings,
                where + ".action is unknown in schema v" + std::to_string(document_version));
        }
        binding.action = *parsed_action;
        const std::string* context = text_field("context");
        const auto parsed_context =
            context ? parse_context(*context, document_version) : std::nullopt;
        if (!parsed_context) {
            return reject(diagnostic_codes::invalid_bindings, is_v1
                ? where + ".context must be land or space in schema v1"
                : where + ".context must be land, space or free in schema v2");
        }
        binding.context = *parsed_context;
        if (!context_allows(binding.action, binding.context)) {
            return reject(diagnostic_codes::incompatible_binding,
                where + ": action " + std::string(to_string(binding.action))
                    + " cannot be bound in context " + std::string(to_string(binding.context)));
        }
        const std::string* device = text_field("device");
        const auto parsed_device = device ? parse_device(*device) : std::nullopt;
        if (!parsed_device) {
            return reject(diagnostic_codes::invalid_bindings, where + ".device is unknown");
        }
        binding.device = *parsed_device;
        const std::string* control = text_field("control");
        const auto parsed_control =
            control ? parse_control(binding.device, *control) : std::nullopt;
        if (!parsed_control) {
            return reject(diagnostic_codes::invalid_bindings,
                where + ".control is not a supported " + std::string(to_string(binding.device))
                    + " control");
        }
        binding.code = *parsed_control;
        const std::string* trigger = text_field("trigger");
        const auto parsed_trigger = trigger ? parse_trigger(*trigger) : std::nullopt;
        if (!parsed_trigger) {
            return reject(diagnostic_codes::invalid_bindings, where + ".trigger is unknown");
        }
        binding.trigger = *parsed_trigger;
        if (!compatible(binding.action, binding.trigger, binding.device)) {
            return reject(diagnostic_codes::incompatible_binding,
                where + ": action " + std::string(to_string(binding.action))
                    + " cannot use trigger " + std::string(to_string(binding.trigger))
                    + " on device " + std::string(to_string(binding.device)));
        }

        if (const JsonValue* modifiers = entry.find("modifiers")) {
            if (modifiers->kind != JsonValue::Kind::array) {
                return reject(diagnostic_codes::invalid_bindings,
                    where + ".modifiers must be an array");
            }
            for (const JsonValue& name : modifiers->items) {
                const auto bit = name.kind == JsonValue::Kind::string
                    ? parse_modifier(name.text) : std::nullopt;
                if (!bit) {
                    return reject(diagnostic_codes::invalid_bindings,
                        where + ".modifiers has an unknown modifier");
                }
                if ((binding.modifiers & *bit) != 0U) {
                    return reject(diagnostic_codes::invalid_bindings,
                        where + ".modifiers repeats a modifier");
                }
                binding.modifiers = static_cast<std::uint8_t>(binding.modifiers | *bit);
            }
        }

        const JsonValue* scale = entry.find("scale");
        if (!scale || scale->kind != JsonValue::Kind::number) {
            return reject(diagnostic_codes::invalid_bindings, where + ".scale must be a number");
        }
        binding.scale = static_cast<float>(scale->number);
        if (!finite(binding.scale) || binding.scale == 0.0F) {
            return reject(diagnostic_codes::invalid_bindings,
                where + ".scale must be finite and nonzero");
        }
        // Held, reset and toggle bindings carry no gain: a held pan's
        // magnitude comes from the XML pan speed (free flight: the authored
        // free_camera speed), and inventing a per-key gain would be an
        // unsupported movement law.
        const bool unit_only = binding.trigger == Trigger::held
            || binding.action == Action::reset_view || binding.action == Action::free_toggle;
        if (unit_only && binding.scale != 1.0F) {
            return reject(diagnostic_codes::invalid_bindings, is_v1
                ? where + ".scale must be exactly 1 for held and reset_view bindings"
                : where + ".scale must be exactly 1 for held, reset_view and free_toggle bindings");
        }

        const Chord chord{binding.context, binding.device, binding.code, binding.modifiers};
        if (const auto existing = chords.find(chord); existing != chords.end()) {
            return reject(diagnostic_codes::ambiguous_binding_chord,
                where + " repeats the chord already bound by " + existing->second);
        }
        chords.emplace(chord, binding.id);
        table.bindings.push_back(std::move(binding));
    }
    if (is_v2) {
        // A way into free flight, or anything bound inside it, needs a way out.
        bool enters_or_flies = false;
        bool exits = false;
        for (const Binding& binding : table.bindings) {
            const bool free_context = binding.context == Context::free;
            if (binding.action == Action::free_toggle && free_context) exits = true;
            if (binding.action == Action::free_toggle || free_context) enters_or_flies = true;
        }
        if (enters_or_flies && !exits) {
            return reject(diagnostic_codes::invalid_bindings,
                "schema v2 table binds free flight but no free_toggle in the free context");
        }
    }
    return core::Result<BindingTable>::success(std::move(table));
}


} // namespace eawr::viewer::camera_input
