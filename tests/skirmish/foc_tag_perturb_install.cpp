#include "foc_tag_perturb_support.hpp"

namespace foc_tag_perturb_test_support {

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& letter : result) {
        if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter - 'A' + 'a');
    }
    return result;
}

[[nodiscard]] bool iequals(const std::string_view left, const std::string_view right) {
    return lower(left) == lower(right);
}

[[nodiscard]] std::string trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(begin, end - begin + 1));
}

[[nodiscard]] std::vector<std::string> split(const std::string_view text, const char separator) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (true) {
        const auto end = text.find(separator, begin);
        parts.emplace_back(text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return parts;
}

[[nodiscard]] std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

[[nodiscard]] std::optional<std::uint64_t> whole(const std::string_view text, const std::uint64_t low, const std::uint64_t high) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < low || value > high) return std::nullopt;
    return value;
}
// ---- The change a row makes to one value ----------------------------------------------------

[[nodiscard]] std::optional<double> decimal(const std::string& token) {
    if (token.empty()) return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}

[[nodiscard]] std::string format_decimal(const double value) {
    std::ostringstream text;
    text.precision(6);
    text << std::fixed << value;
    auto result = text.str();
    while (result.size() > 1 && result.back() == '0') result.pop_back();
    if (!result.empty() && result.back() == '.') result.pop_back();
    return result;
}

// Tokens of a list value: comma or whitespace separated, as FoC's list tags are.
[[nodiscard]] std::vector<std::string> tokens(const std::string& text) {
    std::vector<std::string> result;
    std::string current;
    for (const char letter : text) {
        if (letter == ',' || letter == ' ' || letter == '\t' || letter == '\r' || letter == '\n') {
            if (!current.empty()) result.push_back(std::move(current));
            current.clear();
        } else {
            current += letter;
        }
    }
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

[[nodiscard]] std::optional<std::string> flipped(const std::string& text) {
    const auto value = lower(trim(text));
    const bool upper = !text.empty() && std::isupper(static_cast<unsigned char>(trim(text).front()));
    const auto cased = [&](std::string word) {
        if (upper) word.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(word.front())));
        return word;
    };
    if (value == "yes") return cased("no");
    if (value == "no") return cased("yes");
    if (value == "true") return cased("false");
    if (value == "false") return cased("true");
    return std::nullopt;
}


// The new occurrences of a value, or why this change does not apply to it. `auto` scales every
// number of every occurrence by 2 (a zero to 1; names between them stay); flips a boolean; drops the
// first entry of a list of names, or the last occurrence of a repeated tag. A single name (an
// enum or a reference) needs a `set:` change: which other value is valid is the registry's call.
[[nodiscard]] std::variant<Changed, std::string> change_values(const std::string& change,
    const std::vector<std::string>& current) {
    if (change.rfind("set:", 0) == 0) return Changed{{change.substr(4)}, "set"};
    if (current.empty()) return std::string("the object has no value to change");
    if (change.rfind("drop:", 0) == 0) {
        const auto member = change.substr(5);
        if (member.empty()) return std::string("drop:<member> requires a named member");
        auto values = current;
        bool removed = false;
        for (auto& value : values) {
            std::string kept;
            for (const auto& word : tokens(value)) {
                if (iequals(word, member)) { removed = true; continue; }
                kept += (kept.empty() ? "" : ", ") + word;
            }
            value = kept;
        }
        if (!removed) return std::string("the named member is absent: ") + member;
        return Changed{std::move(values), "dropped named member"};
    }
    const auto& text = current.back();
    const auto words = tokens(text);
    std::string kind = change;
    double factor = 2.0;
    if (change.rfind("scale:", 0) == 0) {
        const auto parsed = decimal(change.substr(6));
        if (!parsed) return "bad scale factor in " + change;
        factor = *parsed;
        kind = "scale";
    }
    // A number anywhere (a numeric list, or name-and-number tuples such as Damage_To_Armor_Mod's)
    // is scaled in every occurrence; the names stay.
    const bool numeric = std::any_of(current.begin(), current.end(), [](const std::string& value) {
        const auto parts = tokens(value);
        return std::any_of(parts.begin(), parts.end(), [](const std::string& word) { return decimal(word).has_value(); });
    });
    if (kind == "auto") {
        if (numeric) kind = "scale";
        else if (flipped(text)) kind = "flip";
        else if (current.size() > 1 || words.size() > 1) kind = "drop";
        else return std::string("a single name: the row needs a set:<value> change");
    }
    auto values = current;
    if (kind == "scale") {
        if (!numeric) return std::string("not a number: ") + text;
        for (auto& value : values) {
            std::string scaled;
            for (const auto& word : tokens(value)) {
                if (!scaled.empty()) scaled += ", ";
                const auto number = decimal(word);
                scaled += number ? format_decimal(*number == 0.0 ? 1.0 : *number * factor) : word;
            }
            value = scaled;
        }
        return Changed{values, "scaled"};
    }
    if (kind == "flip") {
        const auto other = flipped(text);
        if (!other) return std::string("not a boolean: ") + text;
        values.back() = *other;
        return Changed{values, "flipped"};
    }
    if (kind == "drop") {
        if (current.size() > 1) {
            values.pop_back();
            return Changed{values, "dropped"};
        }
        if (words.size() < 2) return std::string("a list of one entry: dropping it removes the tag, use set:");
        std::string rest;
        for (std::size_t index = 1; index < words.size(); ++index) rest += (index > 1 ? ", " : "") + words[index];
        values.back() = rest;
        return Changed{values, "dropped"};
    }
    return "unknown change " + change;
}

// ---- The loaded installation and its scene objects ----------------------------------------
[[nodiscard]] std::string kind_name(const units::UnitKind kind) {
    switch (kind) {
    case units::UnitKind::station: return "station";
    case units::UnitKind::ship: return "ship";
    case units::UnitKind::squadron: return "squadron";
    case units::UnitKind::craft: return "craft";
    }
    return "other";
}

// The scene's object ids of a type, in table order.
[[nodiscard]] std::optional<std::vector<std::string>> objects_of(const units::UnitTables& tables,
    const std::vector<std::string>& factions, const std::string& type) {
    std::vector<std::string> ids;
    const auto add = [&](const std::string& id) {
        if (std::none_of(ids.begin(), ids.end(), [&](const std::string& seen) { return iequals(seen, id); })) {
            ids.push_back(id);
        }
    };
    if (type == "station" || type == "ship" || type == "squadron" || type == "craft") {
        for (const auto& unit : tables.units) if (kind_name(unit.kind) == type) add(unit.id);
    } else if (type == "hardpoint") {
        for (const auto& unit : tables.units) for (const auto& hardpoint : unit.hardpoints) add(hardpoint.id);
    } else if (type == "projectile") {
        for (const auto& projectile : tables.projectiles) add(projectile.id);
    } else if (type == "faction") {
        for (const auto& faction : factions) add(faction);
    } else {
        return std::nullopt;
    }
    return ids;
}


[[nodiscard]] Found find_values(const std::vector<const data::XmlNode*>& top, const std::string& tag) {
    Found found;
    const auto path = split(tag, '/');
    if (path.size() == 1) {
        for (const auto* value : top) {
            if (!iequals(value->name, path.front())) continue;
            found.values.push_back(value->raw_text);
            found.node = value;
        }
        return found;
    }
    const data::XmlNode* container = nullptr;
    for (const auto* value : top) if (iequals(value->name, path.front())) container = value;
    for (std::size_t index = 1; container != nullptr && index + 1 < path.size(); ++index) {
        const data::XmlNode* next = nullptr;
        for (const auto& child : container->children) if (iequals(child.name, path[index])) next = &child;
        container = next;
    }
    if (container == nullptr) return found;
    for (const auto& child : container->children) {
        if (!iequals(child.name, path.back())) continue;
        found.values.push_back(child.raw_text);
        found.node = &child;
    }
    return found;
}

[[nodiscard]] Found find_values(const data::EffectiveObject& object, const std::string& tag) {
    std::vector<const data::XmlNode*> top;
    for (const auto& value : object.values) top.push_back(&value.value);
    return find_values(top, tag);
}

[[nodiscard]] Found find_values(const data::XmlNode& root, const std::string& tag) {
    std::vector<const data::XmlNode*> top;
    for (const auto& child : root.children) top.push_back(&child);
    return find_values(top, tag);
}


[[nodiscard]] units::LoadInput Installation::input(const data::Catalog& source) {
        units::LoadInput result;
        result.catalog = &source;
        result.filesystem = &*filesystem;
        result.model = [this](const std::string_view path) {
            const std::scoped_lock lock(cache_mutex);
            return cache->model(path);
        };
        return result;
    }

} // namespace foc_tag_perturb_test_support
