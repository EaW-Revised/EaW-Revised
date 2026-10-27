#include "galactic_ai_internal.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <unordered_set>
#include <limits>

namespace eawr::script::foc::galactic_ai {
namespace {
const std::string* file(const LifecycleSetup& setup, std::string_view name) {
    const auto key = ai::upper_case(name);
    for (const auto& [path, bytes] : setup.xml) if (ai::upper_case(path) == key) return &bytes;
    return nullptr;
}
template<class Value> core::Result<std::map<std::string, Value, std::less<>>> enumeration(std::string_view path, std::string_view bytes) {
    using Result = core::Result<std::map<std::string, Value, std::less<>>>;
    auto document = ai::parse_xml(path, bytes);
    if (!document) return Result::failure(document.error());
    std::map<std::string, Value, std::less<>> entries;
    for (const auto& child : document.value().children) {
        auto text = ai::trimmed(child.text);
        Value value{};
        const bool hex = text.starts_with("0x") || text.starts_with("0X");
        if (hex) text.erase(0, 2);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, hex ? 16 : 10);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            !entries.emplace(ai::upper_case(child.name), value).second) return Result::failure(lifecycle_error("invalid registered enum " + std::string(path)));
    }
    return Result::success(std::move(entries));
}
} // namespace

std::vector<std::string> required_xml() {
    // WGA-02: galactic closure only. Shared tactical required_xml is unchanged.
    std::vector<std::string> paths;
    const auto add = [&](std::string_view folder, std::initializer_list<const char*> names) {
        for (const auto name : names) paths.push_back("data/xml/ai/" + std::string(folder) + "/" + name + ".xml");
    };
    add("players", {"basicempireplayer", "basicrebelplayer", "ai_player_underworld", "basicempirenogalacticai", "basicrebelnogalacticai", "ai_player_underworldstoryopponent"});
    add("templates", {"basicgenerictemplates", "basicempiretemplates", "basicrebeltemplates", "ai_templates_underworld"});
    add("goalfunctions", {"systemfunctions", "basicoffensivegalacticset", "basicdefensivegalacticset", "basicinfrastructuregalacticset", "basicinformationgalacticset", "ai_goalset_underworld_galactic", "ai_goalset_expansiongeneric_galactic"});
    add("goals", {"systemgoals", "offensivegalacticgoals", "defensivegalacticgoals", "infrastructuregalacticgoals", "informationgalacticgoals", "ai_goals_underworld_galactic", "ai_goals_expansiongeneric_galactic"});
    add("perceptualequations", {"basicgalacticequations", "offensivegalaticequations", "defensivegalacticequations", "infrastructuregalacticequations", "informationgalacticequations", "budgetingequations", "heroequations", "ai_equations_underworld_galactic", "ai_equations_underworld_budgeting", "ai_equations_expansiongeneric_galactic"});
    for (const auto name : {"difficultyadjustments", "gameconstants", "enum/aigoalcategorytype", "enum/gameobjectcategorytype", "enum/gameobjectpropertiestype"}) paths.push_back("data/xml/" + std::string(name) + ".xml");
    return paths;
}

namespace {
struct Vocabulary {
    std::map<std::string, gc::funding::CategoryId, std::less<>> categories;
    std::map<std::string, std::uint64_t, std::less<>> object_categories;
    std::map<std::string, std::uint64_t, std::less<>> object_properties;
};
core::Result<std::shared_ptr<const Vocabulary>> vocabulary(const LifecycleSetup& setup) {
    using Result = core::Result<std::shared_ptr<const Vocabulary>>;
    auto out = std::make_shared<Vocabulary>();
    out->categories = setup.categories;
    if (const auto* bytes = file(setup, "data/xml/enum/aigoalcategorytype.xml")) {
        auto parsed = enumeration<gc::funding::CategoryId>("goal categories", *bytes);
        if (!parsed) return Result::failure(parsed.error());
        out->categories = std::move(parsed).value();
    }
    if (const auto* bytes = file(setup, "data/xml/enum/gameobjectcategorytype.xml")) {
        auto parsed = enumeration<std::uint64_t>("object categories", *bytes);
        if (!parsed) return Result::failure(parsed.error());
        out->object_categories = std::move(parsed).value();
    }
    if (const auto* bytes = file(setup, "data/xml/enum/gameobjectpropertiestype.xml")) {
        auto parsed = enumeration<std::uint64_t>("object properties", *bytes);
        if (!parsed) return Result::failure(parsed.error());
        out->object_properties = std::move(parsed).value();
    }
    return Result::success(std::move(out));
}
ai::ConverterFunction make_converter(const LifecycleSetup& setup, std::shared_ptr<const Vocabulary> words) {
    return [&setup, words = std::move(words)](std::string_view converter_name, std::string_view value) -> std::optional<Number> {
        if (setup.converters) return setup.converters(converter_name, value);
        const auto name = ai::upper_case(converter_name);
        const auto entry = ai::upper_case(ai::trimmed(value));
        if (name == "AIGOALCATEGORYTYPE") {
            const auto found = words->categories.find(entry);
            return found == words->categories.end() ? std::nullopt : std::optional<Number>{Number(found->second)};
        }
        if (name == "DIFFICULTYLEVELTYPE") {
            if (entry == "EASY") return Number(0);
            if (entry == "NORMAL") return Number(1);
            if (entry == "HARD") return Number(2);
        }
        // Space equation files (OffensiveSpaceEquations in stock FoC and RaW, the Remake space files) carry this converter.
        // Only HardPointHealth reads the value, as in the tactical loader; without it the whole file would be skipped.
        if (name == "HARDPOINTTYPE") return Number(0);
        if (name == "GAMEOBJECTCATEGORYTYPE" || name == "GAMEOBJECTPROPERTIESTYPE") {
            const auto& entries = name == "GAMEOBJECTCATEGORYTYPE" ? words->object_categories : words->object_properties;
            std::uint64_t bits{};
            for (const auto& part : ai::split_names(entry, "|")) {
                const auto found = entries.find(part);
                if (found == entries.end()) return std::nullopt;
                bits |= found->second;
            }
            return Number(std::bit_cast<std::int64_t>(bits));
        }
        return std::nullopt;
    };
}
} // namespace

std::vector<std::string> loadable_extra_xml(const LifecycleSetup& setup, const std::vector<std::string>& extras,
                                            std::vector<std::string>& skipped) {
    std::vector<std::string> accepted = setup.xml_paths.empty() ? required_xml() : setup.xml_paths;
    auto words = vocabulary(setup);
    if (!words) { for (const auto& path : extras) skipped.push_back(path + ": " + words.error().message); return {}; }
    const auto converter = make_converter(setup, std::move(words).value());
    std::erase_if(accepted, [](const std::string& path) { return path.find("/enum/") != std::string::npos; });
    if (extras.empty()) return {};
    auto initial = ai::load_ai_data_files(setup.xml, converter, accepted);
    if (!initial) { for (const auto& path : extras) skipped.push_back(path + ": " + initial.error().message); return {}; }
    std::vector<std::string> kept;
    for (const auto& path : extras) {
        // Record parsing has no cross-file dependencies. Equations need only prior names to retain
        // first-wins shadowing; transactional append prevents a rejected file leaking partial names.
        const auto checked = [&]() -> core::Result<void> {
            const auto at = setup.xml.find(path);
            if (at != setup.xml.end() && path.find("/perceptualequations/") != std::string::npos)
                return initial.value().equations.append({{path, at->second}}, converter);
            const std::map<std::string, std::string, std::less<>> single = at == setup.xml.end()
                ? std::map<std::string, std::string, std::less<>>{} : std::map<std::string, std::string, std::less<>>{{path, at->second}};
            auto loaded = ai::load_ai_data_files(single, converter, {path});
            return loaded ? core::Result<void>::success() : core::Result<void>::failure(loaded.error());
        }();
        if (checked) kept.push_back(path);
        else skipped.push_back(path + ": " + checked.error().message);
    }
    return kept;
}

std::vector<std::string> include_mounted_ai_files(LifecycleSetup& setup) {
    // AI-16: keep initial first-wins bindings, then existing directory order and trial/skip policy.
    if (setup.xml_paths.empty()) setup.xml_paths = required_xml();
    std::vector<std::string> extras;
    const std::unordered_set<std::string_view> initial_paths(setup.xml_paths.begin(), setup.xml_paths.end());
    for (const auto folder : {"players", "perceptualequations", "goalfunctions", "goals", "templates"}) {
        const auto prefix = "data/xml/ai/" + std::string(folder) + "/";
        for (const auto& [path, bytes] : setup.xml) {
            static_cast<void>(bytes);
            if (path.starts_with(prefix) && path.ends_with(".xml")
                && !initial_paths.contains(path)) extras.push_back(path);
        }
    }
    std::vector<std::string> skipped;
    const auto kept = loadable_extra_xml(setup, extras, skipped);
    const std::unordered_set<std::string_view> retained(kept.begin(), kept.end());
    for (const auto& path : extras)
        if (!retained.contains(path)) setup.xml.erase(path);
    setup.xml_paths.insert(setup.xml_paths.end(), kept.begin(), kept.end());
    return skipped;
}

std::vector<std::string> unresolved_goal_equations(const ai::AiData& data, std::span<const PlayerSelection> selected) {
    std::vector<std::string> lines;
    for (const auto& selection : selected) {
        const auto type = data.players.find(ai::upper_case(selection.player_type));
        if (type == data.players.end()) continue;
        for (const auto& set : type->second.function_sets) {
            const auto functions = data.function_sets.find(set);
            if (functions == data.function_sets.end()) continue;
            for (const auto& function : functions->second) {
                const auto goal = data.goals.find(function.goal);
                if (goal == data.goals.end() || goal->second.mode != "GALACTIC" || data.equations.find(function.function)) continue;
                lines.push_back("galactic AI selected goal equation is missing, function set skipped: player=" + std::to_string(selection.player)
                    + " player_type=" + selection.player_type + " function_set=" + set
                    + " goal=" + function.goal + " equation=" + function.function);
            }
        }
    }
    return lines;
}

core::Result<ai::AiData> load_data(const LifecycleSetup& setup) {
    using Result = core::Result<ai::AiData>;
    auto words = vocabulary(setup);
    if (!words) return Result::failure(words.error());
    auto categories = words.value()->categories;
    auto converter = make_converter(setup, std::move(words).value());
    auto paths = setup.xml_paths.empty() ? required_xml() : setup.xml_paths;
    // Enum records use the validated enum loader, rather than masquerading as AI records.
    std::erase_if(paths, [](const std::string& path) { return path.find("/enum/") != std::string::npos; });
    auto loaded = ai::load_ai_data_files(setup.xml, converter, paths);
    if (!loaded) return loaded;
    auto& data = loaded.value();
    data.goal_categories = std::move(categories);
    for (const auto& [name, entry] : data.templates) {
        for (const auto& [category, equation] : entry.budget) {
            if (!data.goal_categories.contains(category) || !data.equations.find(equation)) return Result::failure(lifecycle_error("template " + name + " has unknown category/equation " + category + "/" + equation));
        }
        for (const auto& [category, allowance] : entry.tactical_allowances) {
            static_cast<void>(allowance);
            if (!data.goal_categories.contains(category)) return Result::failure(lifecycle_error("template " + name + " has unknown allowance category " + category));
        }
        for (const auto& equation : {entry.trigger, entry.untrigger}) if (!equation.empty() && !data.equations.find(equation)) return Result::failure(lifecycle_error("template " + name + " has unknown trigger " + equation));
    }
    return loaded;
}
} // namespace eawr::script::foc::galactic_ai
