#include "camera_binding_test_support.hpp"

namespace eawr_camera_binding_test {

void test_committed_project_table_is_valid_and_labelled() {
    std::ifstream file(EAWR_CAMERA_BINDINGS_PATH, std::ios::binary);
    expect(static_cast<bool>(file), "committed camera-bindings.json is readable");
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto parsed = input::parse_binding_table(text);
    if (!parsed) {
        std::cerr << "FAILED: committed bindings: " << parsed.error().message << '\n';
        ++failures;
        return;
    }
    const input::BindingTable& table = parsed.value();
    expect(table.version == 2, "committed table is schema v2");
    expect(table.provenance == "project-authored", "committed table is project-authored");
    expect(table.notice.find("NON-RETAIL") != std::string::npos,
           "committed table notice says NON-RETAIL");
    expect(table.free_camera.has_value()
               && camera::validate(*table.free_camera).has_value(),
           "committed table carries valid free-camera settings");
    bool land = false;
    bool space = false;
    bool free = false;
    int toggles = 0;
    for (const input::Binding& binding : table.bindings) {
        land = land || binding.context == input::Context::land;
        space = space || binding.context == input::Context::space;
        free = free || binding.context == input::Context::free;
        if (binding.action == input::Action::free_toggle) ++toggles;
    }
    expect(land && space && free, "committed table binds land, space and free contexts");
    expect(toggles == 3, "committed table toggles free flight from land, space and free");
}

void test_schema_and_version_rejections() {
    const std::string body(pan_right_d);
    expect(input::parse_binding_table(table_with(body)).has_value(), "minimal table parses");
    expect_code(table_with(body, R"("schema": "eawr-camera-bindings", "version": 3,
        "provenance": "project-authored", "notice": "n", "edge_scroll": false)"),
        input::diagnostic_codes::unsupported_bindings_version, "unknown version is rejected");
    expect_code(table_with(body, R"("schema": "eawr-camera-bindings", "version": 0,
        "provenance": "project-authored", "notice": "n", "edge_scroll": false)"),
        input::diagnostic_codes::unsupported_bindings_version, "version 0 is rejected");
    expect_code(table_with(body, R"("schema": "eawr-camera-bindings", "version": 1.0,
        "provenance": "project-authored", "notice": "n", "edge_scroll": false)"),
        input::diagnostic_codes::unsupported_bindings_version, "non-integral version is rejected");
    expect_code(table_with(body, R"("schema": "other", "version": 1,
        "provenance": "project-authored", "notice": "n", "edge_scroll": false)"),
        input::diagnostic_codes::unsupported_bindings_version, "wrong schema is rejected");
    expect_code(table_with(body, R"("schema": "eawr-camera-bindings", "version": 1,
        "provenance": "retail", "notice": "n", "edge_scroll": false)"),
        input::diagnostic_codes::invalid_bindings, "retail provenance is refused in v1");
    expect_code(table_with(body, R"("schema": "eawr-camera-bindings", "version": 1,
        "provenance": "project-authored", "notice": "", "edge_scroll": false)"),
        input::diagnostic_codes::invalid_bindings, "empty notice is rejected");
    expect_code(table_with(body, R"("schema": "eawr-camera-bindings", "version": 1,
        "provenance": "project-authored", "notice": "n", "edge_scroll": false, "extra": 1)"),
        input::diagnostic_codes::invalid_bindings, "unknown top-level key is rejected");
    expect_code(table_with(""), input::diagnostic_codes::invalid_bindings,
                "empty binding array is rejected");
    expect_code(table_with(body) + " x", input::diagnostic_codes::invalid_bindings,
                "trailing data is rejected");
    expect_code(R"({"schema": "eawr-camera-bindings", "schema": "eawr-camera-bindings"})",
                input::diagnostic_codes::invalid_bindings, "duplicate JSON keys are rejected");
    expect_code("[", input::diagnostic_codes::invalid_bindings, "truncated JSON is rejected");
}

void test_json_parser_limits_and_escaped_duplicate_keys() {
    expect(input::max_binding_document_bytes == (std::size_t{1} << 20U),
           "the host read cap and the parser cap are the same 1 MiB");
    const std::string too_large(input::max_binding_document_bytes + 1U, ' ');
    expect_code(too_large + "{}", input::diagnostic_codes::invalid_bindings,
                "documents larger than 1 MiB are rejected");

    std::string too_deep = R"({"schema":"eawr-camera-bindings","version":1,"extra":)";
    for (int depth = 0; depth < 9; ++depth) too_deep.push_back('[');
    too_deep.push_back('0');
    for (int depth = 0; depth < 9; ++depth) too_deep.push_back(']');
    too_deep.push_back('}');
    const auto deep_result = input::parse_binding_table(too_deep);
    expect(!deep_result.has_value()
               && deep_result.error().message.find("nesting is too deep") != std::string::npos,
           "nesting deeper than the supported limit is rejected by the JSON reader");

    const auto escaped_duplicate = input::parse_binding_table(
        R"({"schema":"eawr-camera-bindings","\u0073chema":"eawr-camera-bindings"})");
    expect(!escaped_duplicate.has_value()
               && escaped_duplicate.error().message.find("duplicate object key") != std::string::npos,
           "escaped object keys are decoded before duplicate-key checks");
}

// A notice the parser accepts must survive the report encoder as valid JSON.
// The strict reader doubles as the validator for the encoded literal.
void test_report_encoder_escapes_accepted_control_characters() {
    const std::string head_prefix = R"("schema": "eawr-camera-bindings", "version": 1,
        "provenance": "project-authored", "edge_scroll": false, "notice": )";
    const auto parsed = input::parse_binding_table(table_with(
        pan_right_d, head_prefix + R"("line one\nline two\u0000nul\u001f\t\"q\"\\ end")"));
    expect(parsed.has_value(), "escaped newline, NUL and U+001F are accepted in a notice");
    if (!parsed) return;
    const std::string notice = parsed.value().notice;
    using namespace std::string_literals;
    expect(notice == "line one\nline two\0nul\x1f\t\"q\"\\ end"s,
           "notice escapes decode to the raw control bytes");

    const std::string literal = input::json_string_literal(notice);
    expect(literal == R"("line one\nline two\u0000nul\u001f\t\"q\"\\ end")",
           "report encoder escapes newline, NUL, U+001F, tab, quote and backslash");
    for (const char character : literal) {
        expect(static_cast<unsigned char>(character) >= 0x20U,
               "encoded report literal contains no raw control byte");
    }
    const auto reparsed = input::parse_binding_table(
        table_with(pan_right_d, head_prefix + literal));
    expect(reparsed.has_value() && reparsed.value().notice == notice,
           "encoded notice parses back to the identical bytes");

    std::string every_control;
    for (int value = 0; value < 0x20; ++value) every_control.push_back(static_cast<char>(value));
    const std::string all_encoded = input::json_string_literal(every_control);
    const auto all_reparsed = input::parse_binding_table(
        table_with(pan_right_d, head_prefix + all_encoded));
    expect(all_reparsed.has_value() && all_reparsed.value().notice == every_control,
           "every U+0000-U+001F byte round-trips through the report encoder");
    expect(input::json_string_literal("plain \xC3\xA9") == "\"plain \xC3\xA9\"",
           "non-control UTF-8 bytes are copied unchanged");
}

void test_malformed_utf8_is_rejected() {
    const std::string head_prefix = R"("schema": "eawr-camera-bindings", "version": 1,
        "provenance": "project-authored", "edge_scroll": false, "notice": )";
    expect(input::parse_binding_table(table_with(pan_right_d, head_prefix + "\"caf\xC3\xA9\""))
               .has_value(),
           "well-formed UTF-8 in a notice is accepted");
    for (const std::string_view bad : {std::string_view("\xC3\x28"), std::string_view("\xC0\xAF"),
                                       std::string_view("\xED\xA0\x80"),
                                       std::string_view("\xF4\x90\x80\x80"),
                                       std::string_view("\xE2\x82"), std::string_view("\xFF")}) {
        const auto result = input::parse_binding_table(table_with(
            pan_right_d, head_prefix + "\"x" + std::string(bad) + "\""));
        expect(!result.has_value()
                   && result.error().message.find("not well-formed UTF-8") != std::string::npos,
               "malformed UTF-8 (bad continuation, overlong, surrogate, >U+10FFFF, truncated, "
               "invalid lead) is rejected");
    }
}

// RFC 8259 allows any character to be spelled raw or as \u escapes (with a
// surrogate pair above U+FFFF). Both spellings must yield identical bytes.
// Expected bytes are written out literally, not computed.
void test_unicode_escapes_match_raw_utf8() {
    const std::string head_prefix = R"("schema": "eawr-camera-bindings", "version": 1,
        "provenance": "project-authored", "edge_scroll": false, "notice": )";
    const auto notice_of = [&](const std::string& literal) {
        return input::parse_binding_table(table_with(pan_right_d, head_prefix + literal));
    };

    const std::string cafe = "caf\xC3\xA9";
    const auto raw_cafe = notice_of("\"" + cafe + "\"");
    const auto escaped_cafe = notice_of(R"("caf\u00e9")");
    const auto upper_cafe = notice_of(R"("caf\u00E9")");
    expect(raw_cafe.has_value() && raw_cafe.value().notice == cafe, "raw UTF-8 cafe is accepted");
    expect(escaped_cafe.has_value() && escaped_cafe.value().notice == cafe,
           "escaped U+00E9 decodes to the same UTF-8 bytes as raw cafe");
    expect(upper_cafe.has_value() && upper_cafe.value().notice == cafe,
           "uppercase hex digits decode identically");

    // U+20AC (three bytes) and U+1F680 (four bytes, a surrogate pair).
    const std::string euro = "\xE2\x82\xAC";
    const std::string rocket = "\xF0\x9F\x9A\x80";
    const auto escaped_euro = notice_of(R"("\u20ac")");
    expect(escaped_euro.has_value() && escaped_euro.value().notice == euro,
           "a three-byte BMP escape decodes to UTF-8");
    const auto raw_rocket = notice_of("\"" + rocket + "\"");
    const auto escaped_rocket = notice_of(R"("\ud83d\ude80")");
    const auto upper_rocket = notice_of(R"("\uD83D\uDE80")");
    expect(raw_rocket.has_value() && raw_rocket.value().notice == rocket,
           "raw astral UTF-8 is accepted");
    expect(escaped_rocket.has_value() && escaped_rocket.value().notice == rocket,
           "a surrogate-pair escape decodes to the four-byte UTF-8 character");
    expect(upper_rocket.has_value() && upper_rocket.value().notice == rocket,
           "an uppercase surrogate-pair escape decodes identically");
    const auto edges = notice_of(R"("\u0080\u07ff\u0800\uffff\ud800\udc00\udbff\udfff")");
    expect(edges.has_value()
               && edges.value().notice
                      == "\xC2\x80\xDF\xBF\xE0\xA0\x80\xEF\xBF\xBF"
                         "\xF0\x90\x80\x80\xF4\x8F\xBF\xBF",
           "escapes at every UTF-8 length boundary and U+10000/U+10FFFF decode exactly");

    // The report encoder copies non-ASCII bytes, so an escaped notice is
    // reported as raw UTF-8 and parses back to the same bytes.
    const std::string mixed = cafe + " " + rocket + "\n";
    const auto parsed = notice_of(R"("caf\u00e9 \ud83d\ude80\n")");
    expect(parsed.has_value() && parsed.value().notice == mixed,
           "mixed escaped notice decodes to UTF-8 plus a newline");
    if (parsed) {
        const std::string literal = input::json_string_literal(parsed.value().notice);
        expect(literal == "\"caf\xC3\xA9 \xF0\x9F\x9A\x80\\n\"",
               "report literal carries raw UTF-8 and an escaped newline");
        const auto reparsed = notice_of(literal);
        expect(reparsed.has_value() && reparsed.value().notice == mixed,
               "decoded non-ASCII notice round-trips through the report encoder");
    }

    // Keys decode before the duplicate check, whichever spelling comes first.
    for (const std::string& json :
         {std::string(R"({"caf\u00e9": 1, ")") + cafe + "\": 2}",
          "{\"" + cafe + R"(": 1, "caf\u00E9": 2})",
          std::string(R"({"\ud83d\ude80": 1, ")") + rocket + "\": 2}"}) {
        const auto result = input::parse_binding_table(json);
        expect(!result.has_value()
                   && result.error().message.find("duplicate object key") != std::string::npos,
               "escaped and raw spellings of one key are rejected as duplicates");
    }
    const auto distinct = input::parse_binding_table(R"({"caf\u00e9": 1, "cafe": 2})");
    expect(!distinct.has_value()
               && distinct.error().message.find("duplicate object key") == std::string::npos,
           "distinct keys are not reported as duplicates");
    const auto escaped_schema = input::parse_binding_table(table_with(pan_right_d,
        R"("\u0073chema": "eawr-camera-bindings", "version": 1,
           "provenance": "project-\u0061uthored", "notice": "n", "edge_scroll": true)"));
    expect(escaped_schema.has_value(), "escaped ASCII in schema keys and values still matches");

    for (const std::string_view bad : {
             std::string_view(R"("\ud83d")"),          // lone high at string end
             std::string_view(R"("\ud83dx")"),         // high followed by a raw character
             std::string_view(R"("\ud83d\u0041")"),    // high followed by a non-surrogate
             std::string_view(R"("\ud83d\ud83d")"),    // two highs
             std::string_view(R"("\ude80")"),          // lone low
             std::string_view(R"("\ude80\ud83d")"),    // reversed pair
             std::string_view(R"("\ud83d\n\ude80")"), // pair split by another escape
         }) {
        const auto result = notice_of(std::string(bad));
        expect(!result.has_value()
                   && result.error().code == input::diagnostic_codes::invalid_bindings
                   && result.error().message.find("surrogate") != std::string::npos,
               "lone, reversed or split surrogate escapes are rejected");
    }
    for (const std::string_view bad : {
             std::string_view(R"("\u00g9")"), std::string_view(R"("\u00e")"),
             std::string_view(R"("\u+0e9")"), std::string_view(R"("\u-0e9")"),
             std::string_view(R"("\u 0e9")"), std::string_view(R"("\ud83d\ude8")"),
             std::string_view(R"("\ud83d\uzz80")"), std::string_view(R"("\U00e9")"),
         }) {
        const auto result = notice_of(std::string(bad));
        expect(!result.has_value()
                   && result.error().code == input::diagnostic_codes::invalid_bindings,
               "malformed hex in a backslash-u escape is rejected");
    }
    for (const std::string_view truncated :
         {std::string_view(R"({"notice": "\u00)"), std::string_view(R"({"notice": "\ud83d\ude)")}) {
        const auto result = input::parse_binding_table(truncated);
        expect(!result.has_value()
                   && result.error().message.find("truncated") != std::string::npos,
               "an escape cut off by the end of the document is rejected");
    }

    // A rejected escape must not disturb the active table.
    input::Adapter adapter = ready_adapter();
    const input::BindingTable before = *adapter.table();
    expect(!adapter.activate(table_with(pan_right_d, head_prefix + R"("\ude80")")).has_value(),
           "activation with a lone surrogate fails");
    expect(adapter.table() && *adapter.table() == before,
           "a lone-surrogate table leaves the prior table active");
}

void test_binding_field_rejections() {
    const auto binding = [](const std::string_view fields) {
        return table_with(std::string("{") + std::string(fields) + "}");
    };
    const std::string base = R"("action": "pan_right", "context": "land", "device": "keyboard",
        "control": "D", "trigger": "held", "scale": 1)";
    expect_code(binding(R"("id": "Bad", )" + base), input::diagnostic_codes::invalid_bindings,
                "uppercase id is rejected");
    expect_code(binding(R"("id": ".x", )" + base), input::diagnostic_codes::invalid_bindings,
                "id with a leading separator is rejected");
    expect_code(binding(R"("id": "ok", "extra": 1, )" + base),
                input::diagnostic_codes::invalid_bindings, "unknown binding key is rejected");
    expect_code(table_with(std::string(pan_right_d) + "," + std::string(pan_right_d)),
                input::diagnostic_codes::invalid_bindings, "duplicate id is rejected");
    expect_code(binding(R"("id": "x", "action": "fly", "context": "land", "device": "keyboard",
        "control": "D", "trigger": "held", "scale": 1)"),
        input::diagnostic_codes::invalid_bindings, "unknown action is rejected");
    expect_code(binding(R"("id": "x", "action": "pan_right", "context": "free",
        "device": "keyboard", "control": "D", "trigger": "held", "scale": 1)"),
        input::diagnostic_codes::invalid_bindings, "free context is reserved in v1");
    expect_code(binding(R"("id": "x", "action": "pan_right", "context": "land",
        "device": "keyboard", "control": "Shift", "trigger": "held", "scale": 1)"),
        input::diagnostic_codes::invalid_bindings, "modifier key as control is rejected");
    expect_code(binding(R"("id": "x", "action": "pan_right", "context": "land",
        "device": "mouse_button", "control": "up", "trigger": "held", "scale": 1)"),
        input::diagnostic_codes::invalid_bindings, "wheel name on a button device is rejected");
    expect_code(binding(R"("id": "x", "action": "rotate", "context": "land",
        "device": "keyboard", "control": "Q", "trigger": "held", "scale": 1)"),
        input::diagnostic_codes::incompatible_binding, "held keyboard rotate is incompatible");
    expect_code(binding(R"("id": "x", "action": "zoom", "context": "land",
        "device": "keyboard", "control": "Q", "trigger": "delta", "scale": 1)"),
        input::diagnostic_codes::incompatible_binding, "delta keyboard zoom is incompatible");
    expect_code(binding(R"("id": "x", "action": "pan_left", "context": "land",
        "device": "mouse_wheel", "control": "up", "trigger": "held", "scale": 1)"),
        input::diagnostic_codes::incompatible_binding, "held wheel pan is incompatible");
    expect_code(binding(R"("id": "x", "action": "zoom", "context": "land",
        "device": "mouse_wheel", "control": "up", "trigger": "delta", "scale": 0)"),
        input::diagnostic_codes::invalid_bindings, "zero scale is rejected");
    expect_code(binding(R"("id": "x", "action": "zoom", "context": "land",
        "device": "mouse_wheel", "control": "up", "trigger": "delta", "scale": 1e999)"),
        input::diagnostic_codes::invalid_bindings, "nonfinite scale is rejected");
    expect_code(binding(R"("id": "x", "action": "zoom", "context": "land",
        "device": "mouse_wheel", "control": "up", "trigger": "delta", "scale": "1")"),
        input::diagnostic_codes::invalid_bindings, "string scale is rejected");
    expect_code(binding(R"("id": "x", "action": "pan_right", "context": "land",
        "device": "keyboard", "control": "D", "trigger": "held", "scale": 2)"),
        input::diagnostic_codes::invalid_bindings, "held binding gain is rejected");
    expect_code(binding(R"("id": "x", "action": "pan_right", "context": "land",
        "device": "keyboard", "control": "D", "modifiers": ["shift", "shift"],
        "trigger": "held", "scale": 1)"),
        input::diagnostic_codes::invalid_bindings, "repeated modifier is rejected");
    expect_code(binding(R"("id": "x", "action": "pan_right", "context": "land",
        "device": "keyboard", "control": "D", "modifiers": ["hyper"],
        "trigger": "held", "scale": 1)"),
        input::diagnostic_codes::invalid_bindings, "unknown modifier is rejected");
}

void test_schema_v1_keeps_rejecting_free() {
    // The v1 parser is unchanged: no free context, no free action, no settings.
    const std::string body(pan_right_d);
    auto v1 = input::parse_binding_table(table_with(body));
    expect(v1.has_value() && v1.value().version == 1 && !v1.value().free_camera.has_value(),
           "a v1 table still parses as v1 without free-camera settings");
    const auto v1_binding = [](const std::string_view fields) {
        return table_with(std::string("{") + std::string(fields) + "}");
    };
    expect_code(v1_binding(R"("id": "x", "action": "free_toggle", "context": "land",
        "device": "keyboard", "control": "F", "trigger": "pressed", "scale": 1)"),
        input::diagnostic_codes::invalid_bindings, "free_toggle is unknown in v1");
    expect_code(v1_binding(R"("id": "x", "action": "free_move_forward", "context": "free",
        "device": "keyboard", "control": "W", "trigger": "held", "scale": 1)"),
        input::diagnostic_codes::invalid_bindings, "free actions are unknown in v1");
    expect_code(v1_binding(R"("id": "x", "action": "pan_right", "context": "free",
        "device": "keyboard", "control": "D", "trigger": "held", "scale": 1)"),
        input::diagnostic_codes::invalid_bindings, "free context stays rejected in v1");
    const auto v1_free = input::parse_binding_table(v1_binding(R"("id": "x",
        "action": "pan_right", "context": "free", "device": "keyboard", "control": "D",
        "trigger": "held", "scale": 1)"));
    expect(!v1_free.has_value()
               && v1_free.error().message.find("land or space in schema v1") != std::string::npos,
           "the v1 free-context diagnostic is unchanged");
    expect_code(table_with(body, R"("schema": "eawr-camera-bindings", "version": 1,
        "provenance": "project-authored", "notice": "n", "edge_scroll": false,
        "free_camera": {"move_speed": 1, "vertical_speed": 1, "look_degrees_per_unit": 1,
                        "pitch_min_degrees": -10, "pitch_max_degrees": 10})"),
        input::diagnostic_codes::invalid_bindings, "free_camera block is an unknown key in v1");
}

void test_schema_v2_accepts_free_flight() {
    auto parsed = input::parse_binding_table(v2_table());
    if (!parsed) {
        std::cerr << "FAILED: v2 table: " << parsed.error().message << '\n';
        ++failures;
        return;
    }
    const input::BindingTable& table = parsed.value();
    expect(table.version == 2, "v2 table reports version 2");
    expect(table.free_camera.has_value() && table.free_camera->move_speed == 100.0F
               && table.free_camera->vertical_speed == 50.0F
               && table.free_camera->look_degrees_per_unit == 0.5F
               && table.free_camera->pitch_min_degrees == -80.0F
               && table.free_camera->pitch_max_degrees == 80.0F,
           "v2 free_camera settings are carried exactly");
    expect(input::to_string(input::Context::free) == "free", "free context has a name");
    expect(input::to_string(input::Action::free_look_pitch) == "free_look_pitch",
           "free actions have names");
    // A v2 table need not bind free flight at all.
    expect(input::parse_binding_table(table_with(pan_right_d, v2_head)).has_value(),
           "a v2 table without free bindings parses");
    // The same chord is independent across contexts: D pans in land, strafes in free.
    expect(table.bindings.size() == 12U, "every v2 binding is kept");
}

void test_schema_v2_rejections() {
    const auto v2 = [](const std::string_view bindings) {
        return table_with(bindings, v2_head);
    };
    const auto with_settings = [](const std::string_view settings) {
        return table_with(pan_right_d, std::string(R"("schema": "eawr-camera-bindings",
            "version": 2, "provenance": "project-authored", "notice": "n", "edge_scroll": true)")
            + std::string(settings));
    };
    expect_code(with_settings(""), input::diagnostic_codes::invalid_bindings,
                "v2 requires free_camera settings");
    expect_code(with_settings(R"(, "free_camera": 5)"), input::diagnostic_codes::invalid_bindings,
                "free_camera must be an object");
    expect_code(with_settings(R"(, "free_camera": {"move_speed": 1, "vertical_speed": 1,
        "look_degrees_per_unit": 1, "pitch_min_degrees": -10})"),
        input::diagnostic_codes::invalid_bindings, "a missing setting is rejected");
    expect_code(with_settings(R"(, "free_camera": {"move_speed": 1, "vertical_speed": 1,
        "look_degrees_per_unit": 1, "pitch_min_degrees": -10, "pitch_max_degrees": 10,
        "fov": 60})"),
        input::diagnostic_codes::invalid_bindings, "an unknown setting is rejected");
    expect_code(with_settings(R"(, "free_camera": {"move_speed": "1", "vertical_speed": 1,
        "look_degrees_per_unit": 1, "pitch_min_degrees": -10, "pitch_max_degrees": 10})"),
        input::diagnostic_codes::invalid_bindings, "a string setting is rejected");
    expect_code(with_settings(R"(, "free_camera": {"move_speed": 0, "vertical_speed": 1,
        "look_degrees_per_unit": 1, "pitch_min_degrees": -10, "pitch_max_degrees": 10})"),
        input::diagnostic_codes::invalid_bindings, "a zero speed is rejected");
    expect_code(with_settings(R"(, "free_camera": {"move_speed": 1e39, "vertical_speed": 1,
        "look_degrees_per_unit": 1, "pitch_min_degrees": -10, "pitch_max_degrees": 10})"),
        input::diagnostic_codes::invalid_bindings, "a speed that overflows float is rejected");
    expect_code(with_settings(R"(, "free_camera": {"move_speed": 1, "vertical_speed": 1,
        "look_degrees_per_unit": 1, "pitch_min_degrees": -10, "pitch_max_degrees": 90})"),
        input::diagnostic_codes::invalid_bindings, "a vertical pitch bound is rejected");
    const auto rejected = input::parse_binding_table(with_settings(R"(, "free_camera": {
        "move_speed": 1, "vertical_speed": 1, "look_degrees_per_unit": 1,
        "pitch_min_degrees": 10, "pitch_max_degrees": -10})"));
    expect(!rejected.has_value()
               && rejected.error().message.find("EAWR-CAMERA-0301") != std::string::npos,
           "settings rejections carry the controller diagnostic");

    const std::string toggles = std::string(free_toggle_land) + "," + std::string(free_toggle_free);
    expect_code(v2(std::string(free_toggle_land)), input::diagnostic_codes::invalid_bindings,
                "an entry toggle without a free-context exit is rejected");
    expect_code(v2(R"({"id": "x", "action": "free_move_forward", "context": "free",
        "device": "keyboard", "control": "W", "trigger": "held", "scale": 1})"),
        input::diagnostic_codes::invalid_bindings, "free bindings without an exit are rejected");
    expect_code(v2(toggles + R"(, {"id": "x", "action": "pan_right", "context": "free",
        "device": "keyboard", "control": "D", "trigger": "held", "scale": 1})"),
        input::diagnostic_codes::incompatible_binding, "tactical pan is not a free action");
    expect_code(v2(toggles + R"(, {"id": "x", "action": "free_move_forward", "context": "land",
        "device": "keyboard", "control": "W", "trigger": "held", "scale": 1})"),
        input::diagnostic_codes::incompatible_binding, "free flight is not a land action");
    expect_code(v2(toggles + R"(, {"id": "x", "action": "free_look_yaw", "context": "free",
        "device": "keyboard", "control": "J", "trigger": "held", "scale": 1})"),
        input::diagnostic_codes::incompatible_binding, "keyboard look is incompatible");
    expect_code(v2(toggles + R"(, {"id": "x", "action": "free_move_forward", "context": "free",
        "device": "mouse_wheel", "control": "up", "trigger": "delta", "scale": 1})"),
        input::diagnostic_codes::incompatible_binding, "wheel flight is incompatible");
    expect_code(v2(toggles + R"(, {"id": "x", "action": "free_toggle", "context": "space",
        "device": "mouse_motion", "control": "x", "trigger": "delta", "scale": 1})"),
        input::diagnostic_codes::incompatible_binding, "motion toggle is incompatible");
    expect_code(v2(std::string(free_toggle_free) + R"(, {"id": "land.free.f2",
        "action": "free_toggle", "context": "land", "device": "keyboard", "control": "F",
        "trigger": "pressed", "scale": 2})"),
        input::diagnostic_codes::invalid_bindings, "a toggle gain is rejected");
    expect_code(v2(toggles + R"(, {"id": "x", "action": "free_rise", "context": "free",
        "device": "keyboard", "control": "E", "trigger": "held", "scale": -1})"),
        input::diagnostic_codes::invalid_bindings, "a held free gain is rejected");
    expect_code(v2(toggles + R"(, {"id": "x", "action": "free_look_pitch", "context": "free",
        "device": "mouse_motion", "control": "y", "trigger": "delta", "scale": 0})"),
        input::diagnostic_codes::invalid_bindings, "a zero look scale is rejected");
    expect_code(v2(toggles + R"(, {"id": "x", "action": "free_move_back", "context": "free",
        "device": "keyboard", "control": "f", "trigger": "held", "scale": 1})"),
        input::diagnostic_codes::ambiguous_binding_chord,
        "a free chord that repeats the free toggle is ambiguous");
    expect_code(v2(toggles + R"(, {"id": "x", "action": "zoom", "context": "orbit",
        "device": "mouse_wheel", "control": "up", "trigger": "delta", "scale": 1})"),
        input::diagnostic_codes::invalid_bindings, "an unknown v2 context is rejected");

    // Atomic activation still holds for v2: a bad replacement keeps the table.
    input::Adapter adapter(input::Context::land, true);
    expect(adapter.activate(v2_table()).has_value(), "v2 table activates");
    const input::BindingTable before = *adapter.table();
    expect(!adapter.activate(v2(std::string(free_toggle_land))).has_value(),
           "invalid v2 replacement is refused");
    expect(*adapter.table() == before, "refused v2 replacement keeps the prior table");
}

// `pan_speed_scale` is an optional, finite, positive table key in both schemas.
void test_pan_speed_scale_key() {
    auto plain = input::parse_binding_table(test_table());
    expect(plain.has_value() && plain.value().pan_speed_scale == 1.0F,
           "a table without pan_speed_scale keeps the XML pan speed");
    const auto with_scale = [](const std::string_view value, const std::string_view version = "1") {
        return table_with(pan_right_d, std::string(R"("schema": "eawr-camera-bindings", "version": )")
            + std::string(version) + R"(, "provenance": "project-authored", "notice": "n",
            "edge_scroll": true, "pan_speed_scale": )" + std::string(value));
    };
    auto half = input::parse_binding_table(with_scale("0.5"));
    expect(half.has_value() && half.value().pan_speed_scale == 0.5F, "a v1 table carries pan_speed_scale");
    for (const std::string_view bad : {"0", "-0.5", "\"0.5\"", "null", "true", "1e39", "[0.5]"}) {
        expect_code(with_scale(bad), input::diagnostic_codes::invalid_bindings,
                    "pan_speed_scale must be a finite positive number");
    }
    const std::string v2 = std::string(v2_head) + R"(, "pan_speed_scale": 0.5)";
    auto v2_half = input::parse_binding_table(table_with(pan_right_d, v2));
    expect(v2_half.has_value() && v2_half.value().pan_speed_scale == 0.5F,
           "a v2 table carries pan_speed_scale");
}

void test_input_default_ledger() {
    // Only rates are XML-sourced; no original control assignment exists.
    const auto tags = [](const input::Action action) {
        std::vector<std::string_view> out;
        for (const auto tag : input::rate_source_tags(action)) out.push_back(tag);
        return out;
    };
    using Tags = std::vector<std::string_view>;
    const Tags pan{"Tactical_Min_Scroll_Speed", "Tactical_Max_Scroll_Speed"};
    for (const auto action : {input::Action::pan_left, input::Action::pan_right,
                              input::Action::pan_forward, input::Action::pan_back}) {
        expect(tags(action) == pan, "pan actions name both tactical scroll speeds");
    }
    expect(tags(input::Action::push_scroll) == Tags{"Push_Scroll_Speed_Modifier"}, "push rate tag");
    expect(tags(input::Action::zoom) == Tags{"Distance_Per_Mouse_Unit", "Distance_Min", "Distance_Max"},
           "zoom rate tags include the distance span a detent is converted over");
    expect(tags(input::Action::rotate) == Tags{"Yaw_Per_Mouse_Unit"}, "rotate rate tag");
    expect(tags(input::Action::orbit_pitch) == Tags{"Pitch_Per_Mouse_Unit"},
           "orbit pitch tilts at the FoC pitch rate");
    for (const auto action : {input::Action::translate_x, input::Action::translate_y}) {
        expect(tags(action) == Tags{"Distance_Min", "Distance_Max"},
               "a translate unit is a share of the live distance");
    }
    for (const auto action : {input::Action::rotate_grab, input::Action::reset_view,
                              input::Action::free_toggle, input::Action::free_move_left,
                              input::Action::free_move_right, input::Action::free_move_forward,
                              input::Action::free_move_back, input::Action::free_rise,
                              input::Action::free_descend, input::Action::free_look_grab,
                              input::Action::free_look_yaw, input::Action::free_look_pitch}) {
        expect(tags(action).empty(), "actions without an XML rate report none");
    }
    const auto edge = input::edge_scroll_rate_tags();
    expect(edge.size() == 4 && edge[0] == "Tactical_Edge_Scroll_Region"
           && edge[1] == "Tactical_Offscreen_Scroll_Region", "edge scroll rate tags");
    expect(input::original_binding_source.find("unresolved") == 0,
           "original binding source is stated as unresolved");
    // A table cannot claim an original provenance class it has no evidence for.
    for (const std::string_view claim : {"original", "original-xml", "retail"}) {
        std::ifstream file(EAWR_CAMERA_BINDINGS_PATH, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const auto at = text.find("\"project-authored\"");
        expect(at != std::string::npos, "provenance anchor exists");
        if (at == std::string::npos) continue;
        text.replace(at, std::string_view("\"project-authored\"").size(), "\"" + std::string(claim) + "\"");
        auto parsed = input::parse_binding_table(text);
        expect(!parsed && parsed.error().code == input::diagnostic_codes::invalid_bindings,
               "a non-project binding provenance is refused");
    }
}

} // namespace eawr_camera_binding_test
