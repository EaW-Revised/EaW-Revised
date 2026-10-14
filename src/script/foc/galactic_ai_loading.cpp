#include "galactic_ai_internal.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
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
    for (const auto name : {"difficultyadjustments", "gameconstants", "enum/aigoalcategorytype", "enum/gameobjectcategorytype"}) paths.push_back("data/xml/" + std::string(name) + ".xml");
    return paths;
}

core::Result<ai::AiData> load_data(const LifecycleSetup& setup) {
    using Result = core::Result<ai::AiData>;
    auto categories = setup.categories;
    if (const auto* bytes = file(setup, "data/xml/enum/aigoalcategorytype.xml")) {
        auto parsed = enumeration<gc::funding::CategoryId>("goal categories", *bytes);
        if (!parsed) return Result::failure(parsed.error());
        categories = std::move(parsed).value();
    }
    std::map<std::string, std::uint64_t, std::less<>> object_categories;
    if (const auto* bytes = file(setup, "data/xml/enum/gameobjectcategorytype.xml")) {
        auto parsed = enumeration<std::uint64_t>("object categories", *bytes);
        if (!parsed) return Result::failure(parsed.error());
        object_categories = std::move(parsed).value();
    }
    auto converter = [&](std::string_view converter_name, std::string_view value) -> std::optional<Number> {
        if (setup.converters) return setup.converters(converter_name, value);
        const auto name = ai::upper_case(converter_name);
        const auto entry = ai::upper_case(ai::trimmed(value));
        if (name == "AIGOALCATEGORYTYPE") {
            const auto found = categories.find(entry);
            return found == categories.end() ? std::nullopt : std::optional<Number>{Number(found->second)};
        }
        if (name == "DIFFICULTYLEVELTYPE") {
            if (entry == "EASY") return Number(0);
            if (entry == "NORMAL") return Number(1);
            if (entry == "HARD") return Number(2);
        }
        if (name == "GAMEOBJECTCATEGORYTYPE") {
            std::uint64_t bits{};
            for (const auto& part : ai::split_names(entry, "|")) {
                const auto found = object_categories.find(part);
                if (found == object_categories.end()) return std::nullopt;
                bits |= found->second;
            }
            return Number(std::bit_cast<std::int64_t>(bits));
        }
        return std::nullopt;
    };
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
