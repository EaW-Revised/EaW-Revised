// FoC tactical space AI host (#79, docs/behaviour/foc-tactical-ai.md "#79 host").
// Engine rules cite the host rules FH-xx of that note; each names its evidence.

#include "eawr/script/foc/tactical_ai.hpp"

#include "ai_mode.hpp"
#include "tactical_ai_internal.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace eawr::script::foc {
namespace tactical_ai_detail {

namespace tactical = sim::tactical;
namespace math = sim::math;
using authoritative::Binding;
using authoritative::BindingContext;
using authoritative::Handle;
using authoritative::ScriptEvent;
using authoritative::ScriptScheduler;
using authoritative::Value;
using authoritative::ValueList;
using numeric::LuaNumber;

using namespace detail;

namespace {

// ---- Contrast weights (FH-30) -----------------------------------------------------------

// Runs PGAICommands' Base_Definitions as a plan definition load (PlanDefinitionLoad, which
// calls Set_Contrast_Values) and hands each EnemyContrastTypes entry and its
// FriendlyContrastTypes list to the host. Host glue, not a retail script.
constexpr char contrast_loader_path[] = "EAWR/FOC_CONTRAST_LOADER.LUA";
constexpr char contrast_loader[] = R"LUA(
require("PGAICommands")
PlanDefinitionLoad = true
Base_Definitions()
for index, enemy in pairs(EnemyContrastTypes) do
    _EAWR_Contrast(enemy, FriendlyContrastTypes[index])
end
)LUA";

} // namespace

core::Result<void> load_contrast(Host& host, const std::map<std::string, std::string>& modules) {
    authoritative::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules) {
        if (auto added = manifest.add(path, bytes); !added) return added;
    }
    if (auto added = manifest.add(contrast_loader_path, contrast_loader); !added) return added;
    authoritative::SessionConfig config;
    config.seed = host.setup.seed;
    config.tick_duration = authoritative::TickDuration{tactical::tick_numerator, tactical::tick_denominator};
    config.script_directories = {"Data/Scripts/Library/"};
    auto scripts = ScriptScheduler::create(std::move(config), std::move(manifest));
    if (!scripts) return core::Result<void>::failure(scripts.error());
    struct Lists {
        std::vector<std::pair<std::vector<std::string>, std::vector<LuaNumber>>> lists;
        std::vector<std::pair<std::string, std::uint64_t>> enemies;
    };
    auto lists = std::make_shared<Lists>();
    std::vector<core::Diagnostic> errors;
    const auto check = [&](core::Result<void> added) {
        if (!added) errors.push_back(added.error());
    };
    check(scripts.value().register_binding("WeightedTypeList.Create", [lists](BindingContext&, const ValueList&) {
        lists->lists.emplace_back();
        return one(handle(handle_type_list, lists->lists.size()));
    }));
    check(scripts.value().register_method(handle_type_list, "Parse", [lists](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 3) return fail("Parse expects names and weights");
        const auto* names = std::get_if<std::vector<Value>>(&arguments[1].data);
        const auto* weights = std::get_if<std::vector<Value>>(&arguments[2].data);
        if (names == nullptr || weights == nullptr || names->size() != weights->size()) return fail("Parse expects two lists of one length");
        auto& list = lists->lists.at(std::get<Handle>(arguments[0].data).id - 1);
        list = {};
        for (std::size_t index = 0; index < names->size(); ++index) {
            const std::string* name = as_text((*names)[index]);
            const auto* weight = std::get_if<LuaNumber>(&(*weights)[index].data);
            if (name == nullptr || weight == nullptr) return fail("Parse expects names and numbers");
            list.first.push_back(upper(*name));
            list.second.push_back(*weight);
        }
        return none();
    }));
    check(scripts.value().register_binding("_EAWR_Contrast", [lists](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const Handle* list = arguments.size() == 2 ? as_handle(arguments[1], handle_type_list) : nullptr;
        if (list == nullptr || as_text(arguments[0]) == nullptr) return fail("expects an enemy type and a list");
        lists->enemies.emplace_back(upper(*as_text(arguments[0])), list->id);
        return none();
    }));
    for (const char* name : {"_OuputDebug", "_ScriptMessage", "_MessagePopup", "DumpCallStack"}) {
        check(scripts.value().register_binding(name, noop()));
    }
    if (!errors.empty()) return core::Result<void>::failure(errors.front());
    if (auto created = scripts.value().create_instance(1, contrast_loader_path); !created) return created;
    for (const auto& [enemy, list_id] : lists->enemies) {
        const auto category = host.setup.content.categories.find(enemy);
        if (category == host.setup.content.categories.end() || list_id == 0 || list_id > lists->lists.size()) continue;
        if (std::find(host.contrast_order.begin(), host.contrast_order.end(), category->second) == host.contrast_order.end()) {
            host.contrast_order.push_back(category->second);
        }
        auto& entries = host.contrast[category->second];
        const auto& [names, weights] = lists->lists[list_id - 1];
        for (std::size_t index = 0; index < names.size(); ++index) {
            // FH-30: a name is a category, else an object type.
            ContrastEntry entry;
            entry.weight = weights[index];
            if (const auto bit = host.setup.content.categories.find(names[index]); bit != host.setup.content.categories.end()) {
                entry.category_bits = bit->second;
            } else if (const auto type = host.types_by_name.find(names[index]); type != host.types_by_name.end()) {
                entry.type = type->second->type_id;
            } else {
                continue;
            }
            entries.push_back(entry);
        }
    }
    return core::Result<void>::success();
}

// ---- The goal system (#449) --------------------------------------------------------------

namespace {

std::string plan_stem(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto extension = name.rfind('.');
    if (extension != std::string::npos) name.resize(extension);
    return name;
}

} // namespace

// PL-10: each selected plan's definition load (PlanDefinitionLoad, Base_Definitions) runs in its
// own instance; host glue hands the globals it leaves to the engine.
core::Result<std::vector<ai::PlanDef>> load_plans(const HostPtr& host, const std::map<std::string, std::string>& modules,
    std::vector<std::string>& notes, const std::set<std::string>* selected_goals) {
    using PlansResult = core::Result<std::vector<ai::PlanDef>>;
    authoritative::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules) {
        if (auto added = manifest.add(path, bytes); !added) return PlansResult::failure(added.error());
    }
    const ai::AiMode& mode = ai::ai_mode(host->setup.domain);
    const std::vector<std::string> paths = selected_plans(modules, host->setup.domain);
    for (const std::string& path : paths) {
        // L-03a / ML-33: require substitutes the request verbatim. Keep the
        // admitted logical directory so root-level AI plans resolve too.
        const std::string loader = "require(\"" + path.substr(0, path.rfind('.')) + "\")\n"
            "PlanDefinitionLoad = true\n"
            "Base_Definitions()\n"
            "_EAWR_Plan(Category, TaskForce, IgnoreTarget, MagicPlan, AllowFreeStoreUnits, AllowEngagedUnits,\n"
            "    PerFailureContrastAdjust, MinContrastScale, MaxContrastScale, RequiredCategories)\n";
        if (auto added = manifest.add("EAWR/PLAN_LOADER/" + upper(path) + ".LUA", loader); !added) {
            return PlansResult::failure(added.error());
        }
    }
    authoritative::SessionConfig config;
    config.seed = host->setup.seed;
    config.tick_duration = authoritative::TickDuration{tactical::tick_numerator, tactical::tick_denominator};
    config.script_directories = {"Data/Scripts/Library/", std::string(mode.script_directory)};
    auto scripts = ScriptScheduler::create(std::move(config), std::move(manifest));
    if (!scripts) return PlansResult::failure(scripts.error());
    auto loaded = std::make_shared<std::map<std::uint64_t, ValueList>>();
    std::vector<core::Diagnostic> errors;
    register_globals(scripts.value(), host, errors);
    register_methods(scripts.value(), host, errors);
    if (auto added = scripts.value().register_binding("_EAWR_Plan", [loaded](BindingContext& context, const ValueList& arguments) {
            (*loaded)[context.instance()] = arguments;
            return none();
        });
        !added) {
        errors.push_back(added.error());
    }
    if (!errors.empty()) return PlansResult::failure(errors.front());
    std::vector<ai::PlanDef> plans;
    std::size_t outside_selection{};
    std::uint64_t instance = 1;
    for (const std::string& path : paths) {
        const std::string name = plan_stem(path);
        if (auto created = scripts.value().create_instance(instance, "EAWR/PLAN_LOADER/" + upper(path) + ".LUA"); !created) {
            notes.push_back(name + ": " + created.error().code + " " + created.error().message);
            ++instance;
            continue;
        }
        const auto found = loaded->find(instance);
        ++instance;
        if (found == loaded->end()) {
            notes.push_back(name + ": the definition load left nothing");
            continue;
        }
        // AI-16 / PL-02: global registration has a wider corpus. This space
        // host validates forces only for its selected controllers' goals.
        const auto* category = found->second.empty() ? nullptr : as_text(found->second.front());
        if (selected_goals && category) {
            const auto goals = ai::split_names(*category, "|");
            if (std::none_of(goals.begin(), goals.end(), [&](const auto& goal) {
                    return selected_goals->contains(upper(goal));
                })) {
                ++outside_selection;
                continue;
            }
        }
        auto plan = ai::build_plan(*host, name, path, found->second, notes);
        if (!plan) {
            notes.push_back(plan.error().code + " " + plan.error().message);
            continue;
        }
        plans.push_back(std::move(plan).value());
    }
    if (selected_goals) notes.push_back(std::string("AI selected ") + (host->setup.domain == tactical::BattleDomain::land ? "land" : "space") + " plan definitions loaded=" + std::to_string(plans.size())
        + ", outside selection=" + std::to_string(outside_selection));
    return PlansResult::success(std::move(plans));
}

namespace {
constexpr std::string_view goal_category_converter = "AIGoalCategoryType";

std::string enum_path(const std::string_view name) {
    std::string path = "data/xml/enum/";
    for (const char character : name)
        path += character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
    return path + ".xml";
}
} // namespace

// PE-01 converter constants: category masks, difficulty levels, hardpoints and authored goal categories.
ai::ConverterFunction converters(const HostPtr& host) {
    std::map<std::string, ai::Real, std::less<>> goal_categories;
    const auto goal_path = upper(enum_path(goal_category_converter));
    for (const auto& [path, text] : host->setup.xml) {
        if (upper(path) != goal_path) continue;
        const auto document = ai::parse_xml(path, text);
        if (!document) break;
        for (const auto& entry : document.value().children) {
            const auto number = ai::trimmed(entry.text);
            std::int64_t value{};
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
            if (parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size())
                goal_categories.emplace(upper(entry.name), LuaNumber(value));
        }
        break;
    }
    return [host, goal_categories = std::move(goal_categories)](std::string_view converter,
        std::string_view value) -> std::optional<ai::Real> {
        const std::string name = upper(converter);
        if (name == upper(goal_category_converter)) {
            const auto found = goal_categories.find(upper(ai::trimmed(value)));
            return found == goal_categories.end() ? std::nullopt : std::optional<ai::Real>{found->second};
        }
        if (name == "GAMEOBJECTCATEGORYTYPE" || name == "GAMEOBJECTPROPERTIESTYPE") {
            const auto& entries = name == "GAMEOBJECTCATEGORYTYPE"
                ? host->setup.content.categories : host->setup.content.properties;
            std::uint64_t bits = 0;
            for (const std::string& part : ai::split_names(value, "|")) {
                const auto found = entries.find(upper(part));
                if (found == entries.end()) return std::nullopt;
                bits |= found->second;
            }
            return LuaNumber(static_cast<std::int64_t>(bits));
        }
        if (name == "DIFFICULTYLEVELTYPE") {
            const std::string level = upper(ai::trimmed(value));
            if (level == "EASY") return LuaNumber(0);
            if (level == "NORMAL") return LuaNumber(1);
            if (level == "HARD") return LuaNumber(2);
            return std::nullopt;
        }
        if (name == "HARDPOINTTYPE") return LuaNumber(0); // only HardPointHealth reads it (fidelity list)
        return std::nullopt;
    };
}

} // namespace tactical_ai_detail

using namespace detail;
using namespace tactical_ai_detail;

std::vector<std::string> required_xml() { return required_xml(tactical::BattleDomain::space); }

std::vector<std::string> required_xml(const tactical::BattleDomain domain) {
    auto paths = ai::ai_xml_files(domain);
    paths.push_back(enum_path(goal_category_converter));
    return paths;
}

std::vector<std::string> required_modules(const AiSetup& setup, const bool include_object_roots) {
    std::vector<std::string> paths{
        "Data/Scripts/Library/PGCommands.lua",
        "Data/Scripts/Library/PGBaseDefinitions.lua",
        "Data/Scripts/Library/PGBase.lua",
        "Data/Scripts/Library/PGDebug.lua",
        "Data/Scripts/Library/PGAICommands.lua"};
    for (const auto& player : setup.players) {
        if (!player.ai) continue;
        const auto path = freestore_module(setup, player);
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
    }
    if (setup.players.empty()) paths.push_back(setup.freestore_module);
    if (setup.bounds && !setup.xml.empty()) {
        const auto list = plan_library_modules();
        paths.insert(paths.end(), list.begin(), list.end());
    }
    // SOS-01/45: authored module names are data, never a stock admission list.
    std::set<std::string> objects;
    for (const auto& type : setup.content.types) {
        const auto script = upper(type.lua_script);
        if (admitted_object_script(script)) objects.insert(script);
    }
    if (!objects.empty()) paths.push_back("Data/Scripts/Library/PGStateMachine.lua");
    // SOS-17: the stock Sundered Heart script also requires its hero-plan library.
    if (objects.contains("SUNDERED_HEART")) paths.push_back("Data/Scripts/GameObject/HeroPlanAttach.lua");
    if (include_object_roots)
        for (const auto& script : objects) paths.push_back("Data/Scripts/GameObject/" + script + ".lua");
    return paths;
}

std::map<std::string, std::vector<std::string>> object_script_modules(const AiSetup& setup) {
    std::map<std::string, std::vector<std::string>> roots;
    for (const auto& type : setup.content.types) {
        const auto script = upper(type.lua_script);
        if (!admitted_object_script(script)) continue;
        const auto path = "Data/Scripts/GameObject/" + script + ".lua";
        // An invalid logical path remains a hard admission failure, not a missing attachment.
        if (!authoritative::ModuleManifest::normalise(path)) continue;
        roots[path].push_back(type.name.empty() ? "type " + std::to_string(type.type_id) : type.name);
    }
    for (auto& [path, types] : roots) {
        static_cast<void>(path);
        std::sort(types.begin(), types.end());
        types.erase(std::unique(types.begin(), types.end()), types.end());
    }
    return roots;
}

std::vector<core::Diagnostic> object_script_load_notes(
    const AiSetup& setup, const std::map<std::string, std::string>& modules) {
    std::set<std::string> present;
    for (const auto& [path, bytes] : modules) {
        static_cast<void>(bytes);
        if (const auto canonical = authoritative::ModuleManifest::normalise(path)) present.insert(*canonical);
    }
    std::vector<core::Diagnostic> notes;
    std::set<std::string> strict_roots;
    for (const auto& path : required_modules(setup, false)) {
        if (const auto canonical = authoritative::ModuleManifest::normalise(path)) strict_roots.insert(*canonical);
    }
    for (const auto& [path, types] : object_script_modules(setup)) {
        const auto canonical = authoritative::ModuleManifest::normalise(path);
        if (present.contains(*canonical)) continue;
        if (strict_roots.contains(*canonical)) continue;
        core::Diagnostic note;
        note.code = std::string(authoritative::codes::script_warning);
        note.severity = core::Severity::warning;
        note.logical_path = path;
        note.message = "missing object Lua root; omitting only its attachment for unit types: ";
        bool first = true;
        for (const auto& type : types) {
            if (!first) note.message += ", ";
            first = false;
            note.message += type;
        }
        notes.push_back(std::move(note));
    }
    return notes;
}

std::string freestore_module(const AiSetup& setup, const AiPlayer& player) {
    // A host may explicitly substitute a diagnostic freestore. Ordinary setup
    // keeps the default and follows the selected controller's XML instead.
    if (setup.freestore_module != AiSetup{}.freestore_module) return setup.freestore_module;
    if (player.player_type.empty()) return setup.freestore_module;
    const std::string tag = std::string(ai::ai_mode(setup.domain).freestore_tag) + "FreeStoreScript";
    for (const auto& [path, text] : setup.xml) {
        if (upper(path).find("/AI/PLAYERS/") == std::string::npos) continue;
        const auto document = ai::parse_xml(path, text);
        if (!document || upper(document.value().child_text("Name")) != upper(player.player_type)) continue;
        auto script = document.value().child_text(tag);
        if (script.empty()) return setup.freestore_module;
        if (!upper(script).ends_with(".LUA") && !upper(script).ends_with(".LC")) script += ".lua";
        return "Data/Scripts/FreeStore/" + script;
    }
    return setup.freestore_module;
}

std::vector<std::string> selected_plans(const std::map<std::string, std::string>& modules) {
    return selected_plans(modules, tactical::BattleDomain::space);
}

std::vector<std::string> selected_plans(const std::map<std::string, std::string>& modules, const tactical::BattleDomain domain) {
    const ai::AiMode& mode = ai::ai_mode(domain);
    std::vector<std::string> paths;
    for (const auto& [path, bytes] : modules) {
        static_cast<void>(bytes);
        const auto key = upper(path);
        if (!key.ends_with(".LUA") && !key.ends_with(".LC")) continue;
        if (!key.starts_with("DATA/SCRIPTS/AI/") && !key.starts_with("DATA/SCRIPTS/INTERVENTIONS/")) continue;
        // PL-02 enumerates each configured directory, rather than recursively
        // treating helper subdirectories as plan roots.
        const auto relative = key.substr(key.starts_with("DATA/SCRIPTS/AI/") ? 16 : 27);
        const auto slash = relative.find('/');
        if (slash != std::string::npos) {
            const auto directory = relative.substr(0, slash);
            if ((directory != mode.plan_directory && directory != "STORYARC")
                || relative.find('/', slash + 1) != std::string::npos) continue;
        }
        if (key.ends_with(".LC")) {
            const auto source = key.substr(0, key.size() - 3) + ".LUA";
            if (std::any_of(modules.begin(), modules.end(), [&](const auto& entry) { return upper(entry.first) == source; })) continue;
        }
        paths.push_back(path);
    }
    return paths;
}

std::vector<std::string> selected_plans() {
    std::vector<std::string> paths;
    for (const char* name : {"destroyunit", "flankplan", "destroyunitminimal", "areasweep", "bombingrun", "escortplan",
             "spacescout", "hidesurpriseunits", "hidetransports", "turboattack", "turboattacklocation", "spaceartillery",
             "movetolocation", "movetolocationrush", "retreatplan", "burnunits", "ai_plan_expansiongeneric_defendspacestation",
             "tacticalmultiplayerbuildspaceunitsgeneric", "ai_plan_expansiongeneric_skirmishupgradespacestation", "purchasespaceupgradesgeneric",
             "buildrefineryspace", "buildstructurespace", "groundtospacedamage", "groundtospacedisable"}) {
        paths.push_back(std::string("Data/Scripts/AI/SpaceMode/") + name + ".lua");
    }
    paths.push_back("Data/Scripts/AI/ai_plan_expansiongeneric_generatemagiccashdrop.lua");
    return paths;
}

std::vector<std::string> plan_library_modules() {
    return {"Data/Scripts/Library/PGEvents.lua", "Data/Scripts/Library/PGTaskForce.lua"};
}

core::Result<std::vector<PlanInspection>> inspect_plans(const AiSetup& setup, const std::map<std::string, std::string>& modules) {
    return inspect_plans(setup, modules, selected_plans(modules, setup.domain));
}

core::Result<std::vector<PlanInspection>> inspect_plans(const AiSetup& setup,
    const std::map<std::string, std::string>& modules, const std::vector<std::string>& paths) {
    using InspectResult = core::Result<std::vector<PlanInspection>>;
    auto host = std::make_shared<Host>();
    host->setup = setup;
    for (const AiType& type : host->setup.content.types) {
        host->types.emplace(type.type_id, &type);
        host->types_by_name.emplace(type.name, &type);
    }
    for (const AiPlayer& player : host->setup.players) host->players.emplace(player.player, &player);
    host->view = std::make_shared<WorldView>();
    authoritative::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules) {
        if (auto added = manifest.add(path, bytes); !added) return InspectResult::failure(added.error());
    }
    authoritative::SessionConfig config;
    config.seed = setup.seed;
    config.tick_duration = authoritative::TickDuration{tactical::tick_numerator, tactical::tick_denominator};
    config.script_directories = {"Data/Scripts/Library/", std::string(ai::ai_mode(setup.domain).script_directory)};
    auto scripts = ScriptScheduler::create(std::move(config), std::move(manifest));
    if (!scripts) return InspectResult::failure(scripts.error());
    std::vector<core::Diagnostic> errors;
    register_globals(scripts.value(), host, errors);
    register_methods(scripts.value(), host, errors);
    if (!errors.empty()) return InspectResult::failure(errors.front());
    std::vector<PlanInspection> plans;
    std::uint64_t instance = 1;
    for (const std::string& path : paths) {
        PlanInspection plan;
        plan.path = path;
        if (auto created = scripts.value().create_instance(instance, path); !created) {
            plan.diagnostics.push_back(created.error().code + " " + created.error().message);
            plans.push_back(std::move(plan));
            continue;
        }
        const std::uint64_t tick = scripts.value().completed_tick() + 1;
        ScriptEvent flag;
        flag.key = authoritative::EventKey{tick, producer_foc_engine, instance, 0};
        flag.target = instance;
        flag.kind = ScriptEvent::Kind::assign;
        flag.name = "PlanDefinitionLoad";
        flag.parameter = boolean(true);
        ScriptEvent load = flag;
        load.key.sequence = 1;
        load.kind = ScriptEvent::Kind::call;
        load.name = "Base_Definitions";
        load.parameter.reset();
        for (ScriptEvent* event : {&flag, &load}) {
            if (auto submitted = scripts.value().submit_event(*event); !submitted) return InspectResult::failure(submitted.error());
        }
        authoritative::ServiceOptions options;
        options.host_paced = [](std::uint64_t) { return true; };
        const sim::InlineExecutor executor;
        auto report = scripts.value().service(executor, options);
        if (!report) return InspectResult::failure(report.error());
        for (const auto& diagnostic : report.value().diagnostics) plan.diagnostics.push_back(diagnostic.code + " " + diagnostic.message);
        auto category = scripts.value().read_global(instance, "Category");
        if (category && category.value()) {
            if (const std::string* text = as_text(*category.value())) plan.category = *text;
        }
        plan.loaded = plan.diagnostics.empty();
        plans.push_back(std::move(plan));
        ++instance;
    }
    return InspectResult::success(std::move(plans));
}

} // namespace eawr::script::foc
