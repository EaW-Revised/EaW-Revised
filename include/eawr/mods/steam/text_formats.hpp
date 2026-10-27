#pragma once
// The two small text formats of the launcher and the mod core: JSON (the launcher's saved choice, the viewer's
// failure report, the mod catalog) and Valve's KeyValues text (libraryfolders.vdf, appmanifest_*.acf,
// appworkshop_*.acf). Standard library only (see paths.hpp).
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::mods::steam {

struct JsonValue {
    enum class Kind { null, boolean, number, string, array, object };
    Kind kind = Kind::null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<JsonValue> array;
    // Object members in file order; keys[i] names values[i].
    std::vector<std::string> keys;
    std::vector<JsonValue> values;

    [[nodiscard]] const JsonValue* member(std::string_view key) const;
    [[nodiscard]] const std::string* string_member(std::string_view key) const;
};

// Strict enough for our own files: UTF-8 (BOM allowed), \u escapes incl. surrogate pairs, depth <= 64.
[[nodiscard]] std::optional<JsonValue> parse_json(std::string_view text);
// A JSON string literal (with quotes) for UTF-8 text.
[[nodiscard]] std::string json_quote(std::string_view text);

struct VdfNode {
    std::string key;
    std::string value;       // set when is_object is false
    bool is_object = false;
    std::vector<VdfNode> children;

    // First child whose key equals `name`, ignoring ASCII case.
    [[nodiscard]] const VdfNode* child(std::string_view name) const;
};

// Returns a synthetic root whose children are the file's top-level entries. Quoted strings with
// \\ \" \n \t escapes, unquoted tokens, // comments and { } nesting; nullopt on a malformed file.
[[nodiscard]] std::optional<VdfNode> parse_vdf(std::string_view text);

} // namespace eawr::mods::steam
