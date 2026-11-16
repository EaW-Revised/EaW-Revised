#include "eawr/campaign/galactic_story_scripts.hpp"
#include "eawr/campaign/galactic_world_immediate_host.hpp"
#include "galactic_object_perception.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include "galactic_story_random.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/text.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/script/authoritative/letter_box.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"
#include "eawr/sim/world.hpp"
#include "../script/foc/story_plot_bindings.hpp"
#include "../script/foc/mode_time.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <system_error>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace eawr::campaign {
namespace {

namespace auth = script::authoritative;
namespace story = sim::galactic::story;

constexpr std::string_view library_prefix = "data/scripts/library/";
constexpr std::string_view miscellaneous_prefix = "data/scripts/miscellaneous/";
constexpr std::string_view miscellaneous_directory = "Data/Scripts/Miscellaneous/";
constexpr std::string_view story_prefix = "data/scripts/story/";
constexpr std::string_view library_prefix_directory = "Data/Scripts/Library/";
constexpr std::string_view story_directory = "Data/Scripts/Story/";

// Staged by the bindings below, committed in command order by GalacticStoryLink::commit.
constexpr std::string_view verb_story_event = "galactic.story_event";            // name, has object ("1"/"0"), planet name
constexpr std::string_view verb_story_flag_clear = "galactic.story_flag_clear";  // player, planet, name (decimal text)

constexpr std::string_view player_module = "EAWR/Story/Players.lua";
// WST-90: validate in Lua before crossing the host value boundary. Non-string graphs and functions never need conversion.
constexpr std::string_view player_source = R"LUA(
local lookup = EAWR_Story_Player.Lookup
local kind = type
function Find_Player(...)
    if arg.n ~= 1 or kind(arg[1]) ~= "string" then return lookup() end
    return lookup(arg[1])
end
)LUA";

core::Diagnostic problem(std::string message) {
    core::Diagnostic out;
    out.code = std::string(auth::codes::invalid_request);
    out.message = std::move(message);
    return out;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& character : out) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    return out;
}

std::string trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    return std::string(text);
}

std::string upper_text(std::string_view text) {
    std::string out(text);
    for (char& character : out) if (character >= 'a' && character <= 'z') character = static_cast<char>(character - ('a' - 'A'));
    return out;
}

std::optional<std::uint64_t> number_text(const auth::Value& value) {
    const auto* text = std::get_if<std::string>(&value.data);
    if (!text) return std::nullopt;
    std::uint64_t out{};
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), out);
    if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size()) return std::nullopt;
    return out;
}

} // namespace

struct GalacticStoryLink::Impl {
    script::foc::StoryPlotLinkHolder plots; // Get_Story_Plot's names and the plot-edit sink
    std::unique_ptr<GalacticStorySink> sink;
};

GalacticStoryLink::GalacticStoryLink() : impl_(std::make_unique<Impl>()) {}
GalacticStoryLink::~GalacticStoryLink() = default;

void GalacticStoryLink::bind(std::span<const story::SubPlotDef> definitions, std::unique_ptr<GalacticStorySink> sink) {
    impl_->sink = std::move(sink);
    auto* target = impl_->sink.get();
    impl_->plots.link = script::foc::StoryPlotLink::from(definitions,
        [target](const story::PlotCommand& command) { return target->plot(command); });
}

core::Result<void> GalacticStoryLink::commit(auth::ServiceReport& report) const {
    namespace foc = script::foc;
    auto* sink = impl_->sink.get();
    core::Result<void> failure = core::Result<void>::success();
    const auto note = [&](std::string object, std::string message) {
        core::LoadNote row;
        row.code = "EAWR-LOAD-1400";
        row.object = std::move(object);
        row.action = core::LoadAction::runtime_degraded;
        row.phase = core::LoadPhase::runtime;
        row.diagnostic_code = std::string(auth::codes::unsupported_api);
        row.message = std::move(message);
        report.load_notes.push_back(std::move(row));
    };
    std::erase_if(report.commands, [&](const auth::ScriptCommand& command) {
        const auto& args = command.arguments;
        if (command.verb == foc::verb_story_plot) {
            if (auto why = foc::detail::commit_story_plot(impl_->plots.link.get(), command); !why.empty())
                note("Get_Story_Plot:" + (args.size() > 1 && std::holds_alternative<std::string>(args[1].data)
                    ? std::get<std::string>(args[1].data) : std::string{}), "story plot request not applied: " + why);
            return true;
        }
        if (command.verb == verb_story_event) {
            const auto* name = args.size() == 3 ? std::get_if<std::string>(&args[0].data) : nullptr;
            const auto* has_object = args.size() == 3 ? std::get_if<std::string>(&args[1].data) : nullptr;
            const auto* planet = args.size() == 3 ? std::get_if<std::string>(&args[2].data) : nullptr;
            if (!name || !has_object || !planet) { note("Story_Event", "malformed story event request"); return true; }
            if (!sink) { note("Story_Event:" + *name, "story event not delivered: no plot engine is hosted"); return true; }
            story::TextNotice notice;
            notice.kind = story::Text::ai_notification;
            notice.text = *name;
            if (*has_object == "1") notice.planet = *planet;
            if (auto done = sink->notify(notice); !done && failure) failure = core::Result<void>::failure(done.error());
            return true;
        }
        if (command.verb == verb_story_flag_clear) {
            const auto player = args.size() == 3 ? number_text(args[0]) : std::nullopt;
            const auto planet = args.size() == 3 ? number_text(args[1]) : std::nullopt;
            const auto* name = args.size() == 3 ? std::get_if<std::string>(&args[2].data) : nullptr;
            if (!player || !planet || !name) { note("Check_Story_Flag", "malformed flag request"); return true; }
            if (sink) sink->clear_flag(static_cast<story::Player>(*player), *planet, *name);
            return true;
        }
        return false;
    });
    return failure;
}

core::Result<GalacticObjectQueryCatalog> make_galactic_object_query_catalog(
    const units::UnitTables& tables, std::span<const std::pair<std::string, sim::galactic::TypeId>> bindings) {
    using Result = core::Result<GalacticObjectQueryCatalog>;
    GalacticObjectQueryCatalog result;
    for (const auto& entry : tables.categories)
        if (!result.categories.emplace(upper_text(entry.name), static_cast<std::uint64_t>(entry.value)).second)
            return Result::failure(problem("object query category vocabulary has duplicate names"));
    for (const auto& entry : tables.properties)
        if (!result.properties.emplace(upper_text(entry.name), static_cast<std::uint64_t>(entry.value)).second)
            return Result::failure(problem("object query property vocabulary has duplicate names"));
    for (const auto& [name, id] : bindings) {
        if (!id || name.empty()) return Result::failure(problem("object query requires names and nonzero type identities"));
        const auto [entry, inserted] = result.names.emplace(upper_text(name), id);
        if (!inserted && id < entry->second) entry->second = id;
        const auto* loaded = tables.find(name);
        GalacticObjectQueryType type{id, name, 0, 0, true};
        if (loaded) { type.categories = loaded->category_bits; type.properties = loaded->property_bits; }
        else type.qualified = false;
        if (!result.types.emplace(id, std::move(type)).second)
            return Result::failure(problem("object query requires distinct type identities"));
    }
    return Result::success(std::move(result));
}

core::Result<GalacticObjectFilter> parse_galactic_object_filter(
    const auth::ValueList& arguments, const GalacticObjectQueryCatalog& catalog) {
    using Result = core::Result<GalacticObjectFilter>;
    GalacticObjectFilter filter;
    const auto unrestricted = [](const std::optional<std::uint64_t> bits) {
        return !bits || *bits == std::numeric_limits<std::uint64_t>::max();
    };
    const auto mask = [](const auto& names, std::string_view text) -> std::optional<std::uint64_t> {
        std::uint64_t bits{}; std::string name; bool valid = true;
        const auto flush = [&] {
            const auto found = names.find(upper_text(name));
            if (name.empty() || found == names.end()) valid = false;
            else bits |= found->second;
            name.clear();
        };
        for (const char ch : text) {
            if (ch == '|') flush();
            else if (ch != ' ' && ch != '\t') name.push_back(ch);
        }
        flush();
        return valid ? std::optional<std::uint64_t>{bits} : std::nullopt;
    };
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (const auto* handle = std::get_if<auth::Handle>(&argument.data)) {
            if (handle->kind == script::foc::handle_player && handle->id <= std::numeric_limits<sim::galactic::PlayerId>::max()) {
                filter.player = static_cast<sim::galactic::PlayerId>(handle->id); continue;
            }
            if (handle->kind == script::foc::handle_type && !filter.type && catalog.types.contains(handle->id)) {
                filter.type = handle->id; continue;
            }
        }
        if (const auto* name = std::get_if<std::string>(&argument.data)) {
            if (unrestricted(filter.properties)) {
                if (const auto bits = mask(catalog.properties, *name)) { filter.properties = *bits; continue; }
            }
            if (unrestricted(filter.categories)) {
                if (const auto bits = mask(catalog.categories, *name)) { filter.categories = *bits; continue; }
            }
            if (!filter.type) {
                const auto found = catalog.names.find(upper_text(*name));
                if (found != catalog.names.end()) { filter.type = found->second; continue; }
            }
        }
        return Result::failure(problem("Find_All_Objects_Of_Type: invalid filter " + std::to_string(index + 1)));
    }
    if (!filter.player && !filter.type && unrestricted(filter.categories) && unrestricted(filter.properties))
        return Result::failure(problem("Find_All_Objects_Of_Type requires a filter"));
    return Result::success(std::move(filter));
}

core::Result<bool> matches_galactic_object_filter(const GalacticObjectFilter& filter, const sim::galactic::CensusObject& actor,
    const GalacticObjectQueryType* qualified_flags) {
    using Result = core::Result<bool>;
    if (actor.dead || actor.delete_pending || (filter.player && actor.owner != *filter.player)
        || (filter.type && actor.original_type != *filter.type)) return Result::success(false);
    const auto unrestricted = [](const std::optional<std::uint64_t> bits) {
        return !bits || *bits == std::numeric_limits<std::uint64_t>::max();
    };
    if (unrestricted(filter.categories) && unrestricted(filter.properties)) return Result::success(true);
    if (!qualified_flags || !qualified_flags->qualified)
        return Result::failure(problem("object query requires qualified category and property flags"));
    return Result::success((unrestricted(filter.categories) || (qualified_flags->categories & *filter.categories) != 0)
        && (unrestricted(filter.properties) || (qualified_flags->properties & *filter.properties) != 0));
}

core::Result<auth::ValueList> query_galactic_object_view(std::string_view name, const auth::ValueList& arguments,
    std::span<const sim::galactic::CensusObject> actors, const GalacticObjectQueryCatalog& catalog,
    const sim::PartitionExecutor& executor) {
    using Result = core::Result<auth::ValueList>;
    if (name == "galactic.object_find_all") {
        auto filter = parse_galactic_object_filter(arguments, catalog);
        if (!filter) return Result::failure(filter.error());
        std::vector<auth::ValueList> partitions(sim::tick_partition_count);
        std::vector<std::optional<core::Diagnostic>> errors(sim::tick_partition_count);
        auto scanned = executor.execute_phase("galactic-object-query", sim::tick_partition_count, [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, actors.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& actor = actors[index];
                const auto type = catalog.types.find(actor.current_type);
                auto match = matches_galactic_object_filter(filter.value(), actor,
                    type == catalog.types.end() ? nullptr : &type->second);
                if (!match) { errors[partition] = match.error(); break; }
                if (match.value()) partitions[partition].push_back(auth::Value{auth::Handle{script::foc::handle_game_object, actor.id}});
            }
        });
        if (!scanned) return Result::failure(scanned.error());
        auth::ValueList found;
        for (std::size_t partition = 0; partition < partitions.size(); ++partition) {
            if (errors[partition]) return Result::failure(*errors[partition]);
            auto& rows = partitions[partition];
            found.insert(found.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
        }
        return Result::success({auth::Value{std::move(found)}});
    }
    if (name != "galactic.object_valid" && name != "galactic.object_type"
        && name != "galactic.object_owner" && name != "galactic.object_planet")
        return Result::failure(problem("unknown object query"));
    const auto* handle = arguments.size() == 1 ? std::get_if<auth::Handle>(&arguments.front().data) : nullptr;
    if (!handle || handle->kind != script::foc::handle_game_object)
        return Result::failure(problem("object query requires a game object"));
    const auto at = std::lower_bound(actors.begin(), actors.end(), handle->id,
        [](const auto& actor, auto id) { return actor.id < id; });
    const auto* actor = at != actors.end() && at->id == handle->id && !at->dead && !at->delete_pending ? &*at : nullptr;
    if (name == "galactic.object_valid") return Result::success({auth::Value{actor != nullptr}});
    if (!actor) return Result::success({});
    if (name == "galactic.object_type")
        return actor->original_type ? Result::success({auth::Value{auth::Handle{script::foc::handle_type, actor->original_type}}}) : Result::success({});
    if (name == "galactic.object_owner")
        return actor->owner ? Result::success({auth::Value{auth::Handle{script::foc::handle_player, actor->owner}}}) : Result::success({});
    if (name == "galactic.object_planet")
        return actor->planet ? Result::success({auth::Value{auth::Handle{script::foc::handle_game_object, actor->planet}}}) : Result::success({});
    return Result::failure(problem("unknown object query"));
}

void add_galactic_object_world_rules(GalacticWorldImmediateRules& rules, GalacticObjectQueryCatalog catalog,
    const sim::PartitionExecutor& executor, std::shared_ptr<const GalacticObjectPerception> perception) {
    using Values = core::Result<auth::ValueList>;
    if (perception) {
        // GOL-17: the equation's value from the completed world. A name, player or lookup it cannot answer is a script error
        // of the asking thread, never a made-up number.
        rules.queries.emplace("galactic.perception", [perception = std::move(perception)](const GalacticWorldBatch& batch,
                                                                                           const auth::ValueList& args) -> Values {
            const auto* name = args.empty() ? nullptr : std::get_if<std::string>(&args[0].data);
            const auto* player = args.size() < 2 ? nullptr : std::get_if<auth::Handle>(&args[1].data);
            if (!name || !player || player->kind != script::foc::handle_player)
                return Values::failure(problem("EvaluatePerception expects a name and a player"));
            std::optional<sim::galactic::PlanetId> target;
            if (args.size() > 2) {
                const auto* object = std::get_if<auth::Handle>(&args[2].data);
                if (!object || object->kind != script::foc::handle_game_object)
                    return Values::failure(problem("EvaluatePerception: parameter 3 is not a game object"));
                // A planet is itself; any other object stands for the planet it is on.
                const auto snapshot = batch.world().snapshot();
                const auto planets = snapshot->planets();
                if (std::any_of(planets.begin(), planets.end(), [&](const auto& row) { return row.id == object->id; })) target = object->id;
                else for (const auto& actor : snapshot->actor_records())
                    if (actor.id == object->id) { target = actor.planet; break; }
                if (!target) return Values::failure(problem("EvaluatePerception: parameter 3 is not an object of the galaxy"));
            }
            const auto snapshot = batch.world().snapshot();
            std::string why;
            std::uint64_t visited = 0;
            const auto value = perception->evaluate(*name, static_cast<sim::galactic::PlayerId>(player->id), target, *snapshot,
                batch.world().setup().seed, why, visited);
            batch.note_query_work(visited * auth::host_record_visit_units);
            if (!value) {
                auto error = problem("EvaluatePerception(" + *name + "): " + why);
                error.code = std::string(auth::codes::unsupported_api);
                return Values::failure(std::move(error));
            }
            return Values::success({auth::Value::number(*value)});
        });
    }
    const auto shared_catalog = std::make_shared<const GalacticObjectQueryCatalog>(std::move(catalog));
    rules.verbs.emplace("galactic.object_despawn", [](GalacticWorldBatch& batch, const auth::ValueList& args) -> Values {
        const auto* handle = args.size() == 1 ? std::get_if<auth::Handle>(&args.front().data) : nullptr;
        if (!handle || handle->kind != script::foc::handle_game_object)
            return Values::failure(problem("Despawn requires a game object"));
        const auto id = handle->id;
        if (auto staged = batch.shadow().stage_script_removal(id); !staged) return Values::failure(staged.error());
        return Values::success({});
    });
    for (const std::string name : {"galactic.object_find_all", "galactic.object_valid", "galactic.object_type",
                                   "galactic.object_owner", "galactic.object_planet"}) {
        rules.queries.emplace(name, [shared_catalog, &executor, name](const GalacticWorldBatch& batch, const auth::ValueList& args) {
            const auto* handle = args.size() == 1 ? std::get_if<auth::Handle>(&args.front().data) : nullptr;
            if (handle && handle->kind == script::foc::handle_game_object) {
                if (const auto* planet = batch.shadow().planet(handle->id)) {
                    if (name == "galactic.object_valid") return Values::success({auth::Value{true}});
                    if (name == "galactic.object_owner")
                        return planet->owner ? Values::success({auth::Value{auth::Handle{script::foc::handle_player, planet->owner}}})
                                             : Values::success({});
                }
                if (name != "galactic.object_find_all") {
                    const auto* actor = batch.shadow().script_removed(handle->id) ? nullptr : batch.shadow().actor(handle->id);
                    if (!actor && !batch.shadow().script_removed(handle->id)) actor = batch.shadow().created_object(handle->id);
                    const std::span<const sim::galactic::CensusObject> one = actor
                        ? std::span<const sim::galactic::CensusObject>(actor, 1) : std::span<const sim::galactic::CensusObject>{};
                    return query_galactic_object_view(name, args, one, *shared_catalog, executor);
                }
            }
            std::map<sim::EntityId, sim::galactic::CensusObject> projected;
            for (const auto& actor : batch.world().snapshot()->actor_records())
                if (!batch.shadow().script_removed(actor.id)) projected.emplace(actor.id, actor);
            for (const auto& change : batch.shadow().actor_changes())
                if (!batch.shadow().script_removed(change.actor))
                    if (const auto* actor = batch.shadow().created_object(change.actor)) projected.insert_or_assign(actor->id, *actor);
            std::vector<sim::galactic::CensusObject> actors;
            actors.reserve(projected.size());
            for (const auto& [id, actor] : projected) { (void)id; actors.push_back(actor); }
            // The projection visited every world record and the query then filtered every projected one.
            batch.note_query_work((batch.world().snapshot()->actor_records().size() + actors.size()) * auth::host_record_visit_units);
            return query_galactic_object_view(name, args, actors, *shared_catalog, executor);
        });
    }
}

core::Result<std::shared_ptr<const GalacticObjectScripts>> GalacticObjectScripts::load(
    const vfs::Vfs& files, std::span<const std::pair<sim::galactic::TypeId, std::string>> scripts) {
    using Result = core::Result<std::shared_ptr<const GalacticObjectScripts>>;
    auto out = std::make_shared<GalacticObjectScripts>();
    if (std::none_of(scripts.begin(), scripts.end(), [](const auto& row) { return !trimmed(row.second).empty(); }))
        return Result::success(std::move(out));
    auto assets = files.enumerate("data/scripts", ".lua");
    if (!assets) return Result::failure(assets.error());
    for (const auto& asset : assets.value()) {
        const auto& path = asset.canonical_path;
        if (!path.starts_with(library_prefix) && !path.starts_with("data/scripts/gameobject/")) continue;
        if (out->modules_.contains(path)) continue;
        auto bytes = files.open(path);
        if (!bytes) return Result::failure(bytes.error());
        std::string text(bytes.value().size(), '\0');
        if (!text.empty()) std::memcpy(text.data(), bytes.value().data(), bytes.value().size());
        out->modules_.emplace(path, std::move(text));
    }
    std::set<sim::galactic::TypeId> seen_types;
    for (const auto& [type, authored] : scripts) {
        auto name = trimmed(authored);
        if (name.empty()) continue;
        auto path = lower("data/scripts/gameobject/" + name);
        if (!path.ends_with(".lua")) path += ".lua";
        if (!type || !seen_types.insert(type).second) return Result::failure(problem("object scripts require unique nonzero type identities"));
        if (!out->modules_.contains(path)) {
            out->notes_.push_back("Lua_Script " + name + " has no mounted GameObject module");
            continue;
        }
        out->types_.emplace(type, std::move(path));
    }
    if (!out->types_.empty()) {
        auto perception = GalacticObjectPerception::load(files);
        if (!perception) return Result::failure(perception.error());
        for (const auto& note : perception.value()->notes()) out->notes_.push_back("perception equations: " + note);
        out->perception_ = std::move(perception).value();
    }
    return Result::success(std::move(out));
}

core::Result<std::shared_ptr<const GalacticObjectScripts>> load_world_object_scripts(
    const vfs::Vfs& files, const data::Catalog& catalog, const CampaignSetupPlan& plan) {
    using Result = core::Result<std::shared_ptr<const GalacticObjectScripts>>;
    std::vector<FleetTypeBinding> bindings(plan.type_bindings().begin(), plan.type_bindings().end());
    auto next = bindings.empty() ? sim::galactic::TypeId{} : bindings.back().id;
    for (const auto& location : plan.locations()) if (location.planet) {
        if (next == std::numeric_limits<sim::galactic::TypeId>::max())
            return Result::failure(problem("planet object type identity exhausted"));
        bindings.push_back({++next, location.original_type});
    }
    return load_world_object_scripts(files, catalog, bindings);
}

core::Result<std::shared_ptr<const GalacticObjectScripts>> load_world_object_scripts(
    const vfs::Vfs& files, const data::Catalog& catalog, std::span<const FleetTypeBinding> bindings) {
    using Result = core::Result<std::shared_ptr<const GalacticObjectScripts>>;
    const auto types = units::galactic_type_table(catalog, true);
    std::vector<std::pair<sim::galactic::TypeId, std::string>> scripts;
    StockNameSkips skips;
    for (const auto& binding : bindings) {
        auto resolved = resolve_stock_name(catalog, binding.original_type, data::Category::game_object, "object script type", skips);
        if (!resolved) return Result::failure(resolved.error());
        if (!resolved.value()) continue;
        auto source = units::resolve_initial_galactic_type(types, binding.original_type);
        if (!source) return Result::failure(source.error());
        auto object = resolve_stock_name(catalog, source.value().name, data::Category::game_object, "object script type", skips);
        if (!object) return Result::failure(object.error());
        if (!object.value()) continue;
        if (const auto* value = object.value()->value("Lua_Script")) {
            data::tag_trace::used(value->value);
            if (!trimmed(value->value.raw_text).empty()) scripts.emplace_back(binding.id, value->value.raw_text);
        }
    }
    return GalacticObjectScripts::load(files, scripts);
}

std::string GalacticObjectScripts::fingerprint() const {
    std::string text = "eawr-galactic-object-scripts-v2";
    const auto field = [&](std::string_view value) { text += std::to_string(value.size()) + ":"; text += value; };
    for (const auto& [path, bytes] : modules_) {
        field(path);
        const auto digest = core::sha256(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        field(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
    }
    for (const auto& [type, module] : types_) { field(std::to_string(type)); field(module); }
    field(perception_ ? perception_->digest() : std::string_view{});
    return core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::vector<std::string> GalacticStoryScripts::subplots_of(std::span<const std::uint64_t> instances) const {
    std::vector<std::string> out;
    for (const auto& entry : entries_)
        if (std::find(instances.begin(), instances.end(), entry.instance) != instances.end()) out.push_back(entry.subplot);
    return out;
}

core::Result<auth::ScriptScheduler> GalacticObjectScripts::create_scheduler(auth::SessionConfig config) const {
    // An uncaught error ends the failing thread or call of an object script, not the script.
    config.retire_on_lua_error = false;
    config.label = "object";
    config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/GameObject/"};
    auth::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules_) {
        if (auto added = manifest.add(path, bytes); !added)
            return core::Result<auth::ScriptScheduler>::failure(added.error());
    }
    return auth::ScriptScheduler::create(std::move(config), std::move(manifest));
}

core::Result<std::vector<auth::ScriptDiagnostic>> GalacticObjectScripts::reconcile(
    auth::ScriptScheduler& scheduler, std::span<const sim::galactic::CensusObject> actors,
    const sim::PartitionExecutor& executor) const {
    // Actor identities are session-unique. Reserve a separate RNG/instance
    // namespace from the AI and story hosts for each retained actor.
    constexpr std::uint64_t first_object_instance = std::uint64_t{1} << 40;
    std::vector<std::optional<auth::ObjectScript>> proposals(actors.size());
    const auto prepared = executor.execute_phase("galactic-object-script-attachments", sim::tick_partition_count,
        [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, actors.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& actor = actors[index];
                if (!actor.id || actor.dead || actor.delete_pending || !actor.current_type) continue;
                const auto script = types_.find(actor.current_type);
                if (script == types_.end()) continue;
                proposals[index] = auth::ObjectScript{{script::foc::handle_game_object, actor.id}, actor.id,
                    first_object_instance + actor.id, script->second};
            }
        });
    if (!prepared) return core::Result<std::vector<auth::ScriptDiagnostic>>::failure(prepared.error());
    std::vector<auth::ObjectScript> objects;
    for (auto& proposal : proposals) if (proposal) objects.push_back(std::move(*proposal));
    return scheduler.reconcile_objects(std::move(objects));
}

core::Result<auth::ServiceReport> GalacticObjectScripts::service(
    auth::ScriptScheduler& scheduler, const std::uint64_t frame, const sim::PartitionExecutor& executor) const {
    using Result = core::Result<auth::ServiceReport>;
    if (auto clock = scheduler.set_galactic_frame(frame); !clock) return Result::failure(clock.error());
    const auto attachments = scheduler.object_scripts();
    const auto active = scheduler.instances();
    struct Due { bool pump{}, first{}; std::optional<script::numeric::LuaNumber> rate; core::Diagnostic error; };
    std::vector<Due> due(attachments.size());
    const auto now = script::foc::tactical_ai_detail::mode_seconds(frame);
    const auto prepared = executor.execute_phase("galactic-object-script-admission", sim::tick_partition_count,
        [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, attachments.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto id = attachments[index].instance;
                if (!std::binary_search(active.begin(), active.end(), id)) continue;
                auto threads = scheduler.thread_slots(id);
                if (!threads) { due[index].error = threads.error(); continue; }
                if (std::none_of(threads.value().begin(), threads.value().end(), [](bool live) { return live; })) continue;
                auto rate = scheduler.read_global(id, "ServiceRate");
                auto last = scheduler.read_global(id, "LastService");
                if (!rate || !last) { due[index].error = !rate ? rate.error() : last.error(); continue; }
                const auto* period = rate.value() ? std::get_if<script::numeric::LuaNumber>(&rate.value()->data) : nullptr;
                const auto* previous = last.value() ? std::get_if<script::numeric::LuaNumber>(&last.value()->data) : nullptr;
                due[index].pump = period && (!previous || *period < now - *previous);
                if (due[index].pump) { due[index].first = !previous; due[index].rate = *period; }
            }
        });
    if (!prepared) return Result::failure(prepared.error());
    for (std::size_t index = 0; index < due.size(); ++index) {
        if (!due[index].error.code.empty()) return Result::failure(due[index].error);
        if (!due[index].pump) continue;
        const auto id = attachments[index].instance;
        auth::ScriptEvent event;
        event.key = {scheduler.completed_tick() + 1, auth::first_simulation_producer, id, 3};
        event.target = id; event.kind = auth::ScriptEvent::Kind::pump;
        if (auto submitted = scheduler.submit_event(event); !submitted) return Result::failure(submitted.error());
    }
    auth::ServiceOptions options;
    options.time_tick = frame;
    options.host_paced = [](std::uint64_t) { return true; };
    auto result = scheduler.service(executor, options);
    if (!result) return result;
    const auto remaining = scheduler.instances();
    for (std::size_t index = 0; index < due.size(); ++index) {
        const auto id = attachments[index].instance;
        if (!due[index].pump || !std::binary_search(remaining.begin(), remaining.end(), id)) continue;
        auto threads = scheduler.thread_slots(id);
        if (!threads) return Result::failure(threads.error());
        if (std::any_of(threads.value().begin(), threads.value().end(), [](bool live) { return live; }))
            if (auto stamped = scheduler.stamp_galactic_object_service(id, due[index].first, *due[index].rate, now); !stamped)
                return Result::failure(stamped.error());
    }
    return result;
}

core::Result<void> register_galactic_object_bindings(auth::ScriptScheduler& scheduler, const GalacticStoryPlayers& players,
    const std::shared_ptr<GalacticStoryLink>& story_link) {
    using Values = core::Result<auth::ValueList>;
    // GOL-17: the mounted equations, answered by the world host's perception query.
    if (auto added = scheduler.register_binding("EvaluatePerception", [](auth::BindingContext& context, const auth::ValueList& args) -> Values {
        auto result = context.query_immediate("galactic.perception", args);
        if (result || result.error().code != auth::codes::missing_api) return result;
        auto error = problem("EvaluatePerception is not available for galactic object scripts: this host has no perception equations");
        error.code = std::string(auth::codes::missing_api);
        return Values::failure(std::move(error));
    }); !added) return added;
    std::map<std::string, sim::galactic::TypeId> names;
    std::map<sim::galactic::TypeId, std::string> type_names;
    for (const auto& [name, id] : players.types) {
        const auto [at, inserted] = names.try_emplace(lower(name), id);
        if (!inserted && id < at->second) at->second = id;
        type_names.try_emplace(id, name);
    }
    if (auto added = scheduler.register_binding("Get_Game_Mode", [](auth::BindingContext&, const auth::ValueList&) {
        return Values::success({auth::Value::text("Galactic")});
    }); !added) return added;
    if (auto added = scheduler.register_binding("Find_Object_Type", [names = std::move(names)](
            auth::BindingContext&, const auth::ValueList& args) -> Values {
        const auto* name = args.empty() ? nullptr : std::get_if<std::string>(&args[0].data);
        if (!name) return Values::failure(problem("Find_Object_Type requires a type name"));
        const auto found = names.find(lower(*name));
        if (found == names.end()) return Values::success({});
        return Values::success({auth::Value{auth::Handle{script::foc::handle_type, found->second}}});
    }); !added) return added;
    if (auto added = scheduler.register_method(script::foc::handle_type, "Get_Name", [type_names = std::move(type_names)](
            auth::BindingContext&, const auth::ValueList& args) -> Values {
        const auto* type = args.empty() ? nullptr : std::get_if<auth::Handle>(&args[0].data);
        if (!type || type->kind != script::foc::handle_type) return Values::failure(problem("Get_Name requires a type"));
        const auto found = type_names.find(type->id);
        return found == type_names.end() ? Values::success({}) : Values::success({auth::Value::text(found->second)});
    }); !added) return added;
    if (auto added = scheduler.register_method(script::foc::handle_type, "Is_Valid", [types = players.types](
            auth::BindingContext&, const auth::ValueList& args) -> Values {
        const auto* type = args.size() == 1 ? std::get_if<auth::Handle>(&args[0].data) : nullptr;
        const bool found = type && type->kind == script::foc::handle_type
            && std::any_of(types.begin(), types.end(), [&](const auto& row) { return row.second == type->id; });
        return Values::success({auth::Value{found}});
    }); !added) return added;
    if (auto added = scheduler.register_method(script::foc::handle_type, "Get_Build_Cost", [costs = players.type_build_costs](
            auth::BindingContext&, const auth::ValueList& args) -> Values {
        const auto* type = args.size() == 1 ? std::get_if<auth::Handle>(&args[0].data) : nullptr;
        if (!type || type->kind != script::foc::handle_type) return Values::failure(problem("Get_Build_Cost requires a type"));
        const auto found = costs.find(type->id);
        if (found == costs.end()) return Values::failure(problem("Get_Build_Cost: galactic cost metadata unavailable"));
        return Values::success({auth::Value::number(script::numeric::from_fixed(found->second))});
    }); !added) return added;
    if (auto added = scheduler.register_method(script::foc::handle_player, "Give_Money",
            [](auth::BindingContext& context, const auth::ValueList& args) -> Values {
        auto result = context.apply_immediate("galactic.give_money", args);
        if (!result) return result;
        return Values::success({});
    }); !added) return added;
    if (auto added = scheduler.register_binding("Find_All_Objects_Of_Type", [](auth::BindingContext& context,
            const auth::ValueList& args) -> Values {
        return context.query_immediate("galactic.object_find_all", args);
    }); !added) return added;
    // `TestValid` is the data's own Lua function (the stock base library defines it over the wrapper's `Is_Valid`
    // method), so it is no native binding: a protected native of that name would make every such library fail to load.
    if (auto added = scheduler.register_method(script::foc::handle_player, "Is_Valid", [roster = players.players](
            auth::BindingContext&, const auth::ValueList& args) -> Values {
        const auto* player = args.size() == 1 ? std::get_if<auth::Handle>(&args[0].data) : nullptr;
        const bool found = player && player->kind == script::foc::handle_player
            && std::any_of(roster.begin(), roster.end(), [&](const auto& row) { return row.id == player->id; });
        return Values::success({auth::Value{found}});
    }); !added) return added;
    if (auto added = scheduler.register_binding("Spawn_Unit", [](auth::BindingContext& context,
            const auth::ValueList& args) -> Values {
        if (args.size() < 3) return Values::failure(problem("Spawn_Unit requires type, planet and player"));
        const auto* type = std::get_if<auth::Handle>(&args[0].data);
        const auto* planet = std::get_if<auth::Handle>(&args[1].data);
        const auto* player = std::get_if<auth::Handle>(&args[2].data);
        if (!type || type->kind != script::foc::handle_type || !planet || planet->kind != script::foc::handle_game_object
            || !player || player->kind != script::foc::handle_player)
            return Values::failure(problem("Spawn_Unit requires type, planet and player handles"));
        auto result = context.apply_immediate("galactic.spawn_unit", {args[2], args[1], args[0]});
        if (result && result.value().empty()) context.report(auth::codes::script_warning, "Spawn_Unit: the type created no object");
        return result;
    }); !added) return added;
    for (const auto& [method, query] : std::array<std::pair<std::string_view, std::string_view>, 4>{{
             {"Is_Valid", "galactic.object_valid"}, {"Get_Type", "galactic.object_type"},
             {"Get_Owner", "galactic.object_owner"}, {"Get_Planet_Location", "galactic.object_planet"}}}) {
        if (auto added = scheduler.register_method(script::foc::handle_game_object, method,
                [query = std::string(query)](auth::BindingContext& context, const auth::ValueList& args) {
                    return context.query_immediate(query, args);
                }); !added) return added;
    }
    if (auto added = scheduler.register_method(script::foc::handle_game_object, "Prevent_AI_Usage",
            [](auth::BindingContext& context, const auth::ValueList& args) -> Values {
        auto result = context.apply_immediate("galactic.story_prevent_ai_usage", args);
        if (!result) return result;
        const auto* applied = result.value().empty() ? nullptr : std::get_if<bool>(&result.value()[0].data);
        if (!applied) return Values::failure(problem("Prevent_AI_Usage native result is invalid"));
        if (!*applied) context.report(auth::codes::script_warning, "Prevent_AI_Usage: object is not in an AI pool");
        return Values::success({});
    }); !added) return added;
    if (auto added = scheduler.register_method(script::foc::handle_game_object, "Despawn",
            [](auth::BindingContext& context, const auth::ValueList& args) -> Values {
        if (args.size() != 1) return Values::failure(problem("Despawn requires an object"));
        auto result = context.apply_immediate("galactic.object_despawn", args);
        if (!result) return result;
        return Values::success({});
    }); !added) return added;
    if (auto added = scheduler.register_method(script::foc::handle_game_object, "Service_Wrapper",
            [](auth::BindingContext& context, const auth::ValueList& args) -> Values {
        const auto* object = args.size() == 1 ? std::get_if<auth::Handle>(&args[0].data) : nullptr;
        if (!object || object->kind != script::foc::handle_game_object)
            return Values::failure(problem("Service_Wrapper requires an object"));
        bool unavailable = false;
        auto serviced = context.service_proximities(*object, [&](const auth::ProximityQuery&) {
            unavailable = true;
            return std::vector<auth::Handle>{};
        });
        if (!serviced) return Values::failure(serviced.error());
        if (unavailable) {
            core::Diagnostic diagnostic; diagnostic.code = std::string(auth::codes::missing_api);
            diagnostic.message = "galactic object proximity service is unavailable";
            return Values::failure(std::move(diagnostic));
        }
        return Values::success({});
    }); !added) return added;
    for (const std::string_view sink : {"_OuputDebug", "_ScriptMessage", "_CustomScriptMessage", "_MessagePopup", "DumpCallStack"})
        if (auto added = scheduler.register_binding(sink, [](auth::BindingContext&, const auth::ValueList&) {
                return Values::success({}); }); !added) return added;
    if (auto added = scheduler.register_binding("Script.Debug_Should_Issue_Event_Alert",
            [](auth::BindingContext&, const auth::ValueList&) { return Values::success({auth::Value{false}}); }); !added) return added;
    std::vector<std::string_view> implemented{"Get_Game_Mode", "Find_Object_Type", "Find_All_Objects_Of_Type", "Spawn_Unit", "EvaluatePerception",
        "_OuputDebug", "_ScriptMessage", "_CustomScriptMessage", "_MessagePopup", "DumpCallStack",
        "Script.Debug_Should_Issue_Event_Alert"};
    if (story_link) {
        std::vector<core::Diagnostic> errors;
        script::foc::detail::register_story_plot_bindings(scheduler,
            script::foc::StoryPlotAccess([story_link] { return story_link->impl().plots.link.get(); }), errors);
        if (!errors.empty()) return core::Result<void>::failure(errors.front());
        implemented.push_back("Get_Story_Plot");
        std::map<std::uint64_t, std::string> planets;
        for (const auto& [name, id] : players.planets) planets.try_emplace(id, upper_text(name));
        if (auto added = scheduler.register_binding("Story_Event", [planets = std::move(planets)](
                auth::BindingContext& context, const auth::ValueList& args) -> Values {
            if (args.empty() || args.size() > 2 || !std::holds_alternative<std::string>(args[0].data))
                return Values::failure(problem("Story_Event expects a name and an optional object"));
            std::string planet;
            bool has_object = false;
            if (args.size() == 2 && !std::holds_alternative<std::monostate>(args[1].data)) {
                const auto* object = std::get_if<auth::Handle>(&args[1].data);
                if (!object || object->kind != script::foc::handle_game_object)
                    return Values::failure(problem("Story_Event: the second argument must be a game object"));
                has_object = true;
                if (const auto found = planets.find(object->id); found != planets.end()) planet = found->second;
            }
            context.issue_command(std::string(verb_story_event), {auth::Value::text(std::get<std::string>(args[0].data)),
                auth::Value::text(has_object ? "1" : "0"), auth::Value::text(planet)});
            return Values::success({});
        }); !added) return added;
        implemented.push_back("Story_Event");
        if (auto added = scheduler.register_binding("Check_Story_Flag", [story_link](
                auth::BindingContext& context, const auth::ValueList& args) -> Values {
            if (args.size() < 2 || args.size() > 4 || !std::holds_alternative<std::string>(args[1].data))
                return Values::failure(problem("Check_Story_Flag expects a player, a flag name, an optional planet and an optional reset"));
            const auto* player = std::get_if<auth::Handle>(&args[0].data);
            if (!player && !std::holds_alternative<std::monostate>(args[0].data))
                return Values::failure(problem("Check_Story_Flag: the first argument must be a player"));
            std::uint64_t planet{};
            if (args.size() > 2 && !std::holds_alternative<std::monostate>(args[2].data)) {
                const auto* object = std::get_if<auth::Handle>(&args[2].data);
                if (!object || object->kind != script::foc::handle_game_object)
                    return Values::failure(problem("Check_Story_Flag: the planet must be a game object or nil"));
                planet = object->id;
            }
            bool reset = false;
            if (args.size() > 3 && !std::holds_alternative<std::monostate>(args[3].data)) {
                const auto* value = std::get_if<bool>(&args[3].data);
                if (!value) return Values::failure(problem("Check_Story_Flag: reset must be a Boolean or nil"));
                reset = *value;
            }
            if (!player || player->kind != script::foc::handle_player) return Values::success({auth::Value{false}});
            const auto& name = std::get<std::string>(args[1].data);
            const auto* sink = story_link->impl().sink.get();
            const bool set = sink && sink->flag(static_cast<story::Player>(player->id), planet, name);
            if (set && reset)
                context.issue_command(std::string(verb_story_flag_clear), {auth::Value::text(std::to_string(player->id)),
                    auth::Value::text(std::to_string(planet)), auth::Value::text(name)});
            return Values::success({auth::Value{set}});
        }); !added) return added;
        implemented.push_back("Check_Story_Flag");
    }
    for (const std::string_view name : auth::missing_galactic_api_names()) {
        const std::string_view receiver = name.substr(0, name.find('.'));
        if (receiver != name && (receiver == "Object" || receiver == "Target" || receiver == "Budget"
            || receiver == "FreeStore" || receiver == "PlayerObject")) implemented.push_back(name);
    }
    return auth::register_missing_galactic_api(scheduler, implemented);
}

namespace {
class GalacticObjectEngine final : public auth::GalacticObjectScriptEngine {
public:
    GalacticObjectEngine(std::shared_ptr<const GalacticObjectScripts> content, GalacticObjectEffectsFactory effects,
        std::size_t max_instances) : content_(std::move(content)), effects_(std::move(effects)), max_instances_(max_instances) {}
    core::Result<auth::ServiceReport> service(const sim::PartitionExecutor& executor,
        sim::galactic::GalacticSession& world, auth::ScriptScheduler& scheduler) override {
        // The effect owner is scoped to this completed-world visit. Install it
        // before admission because a module's initial chunk may use bindings.
        if (effects_) {
            auto host = effects_(world, executor);
            if (!host) return core::Result<auth::ServiceReport>::failure(host.error());
            if (auto bound = scheduler.set_immediate_effect_host(std::move(host).value(), max_instances_); !bound)
                return core::Result<auth::ServiceReport>::failure(bound.error());
        }
        const auto snapshot = world.snapshot();
        if (!snapshot) return core::Result<auth::ServiceReport>::failure(problem("object scripts require a world snapshot"));
        auto attached = content_->reconcile(scheduler, snapshot->actor_records(), executor);
        if (!attached) return core::Result<auth::ServiceReport>::failure(attached.error());
        auto result = content_->service(scheduler, world.clock().galactic_frame, executor);
        if (!result) return result;
        auto diagnostics = std::move(attached).value();
        result.value().diagnostics.insert(result.value().diagnostics.begin(),
            std::make_move_iterator(diagnostics.begin()), std::make_move_iterator(diagnostics.end()));
        return result;
    }
private:
    std::shared_ptr<const GalacticObjectScripts> content_;
    GalacticObjectEffectsFactory effects_;
    std::size_t max_instances_{};
};
}

std::shared_ptr<auth::GalacticObjectScriptEngine> make_galactic_object_engine(
    std::shared_ptr<const GalacticObjectScripts> content, GalacticObjectEffectsFactory effects, std::size_t max_instances) {
    return content && max_instances ? std::make_shared<GalacticObjectEngine>(std::move(content), std::move(effects), max_instances) : nullptr;
}

core::Result<void> configure_world_object_scripts(GameSession& game, std::shared_ptr<const GalacticObjectScripts> content,
    const std::shared_ptr<GalacticStoryLink>& link, GalacticObjectEffectsFactory effects, std::size_t max_instances) {
    if (!content || !link || !effects || !game.galactic_scripts() || !game.galactic_scripts()->object_scheduler())
        return core::Result<void>::failure(problem("world object scripts require content, story link, effect owner and scheduler"));
    auto engine = make_galactic_object_engine(std::move(content), std::move(effects), max_instances);
    if (!engine) return core::Result<void>::failure(problem("world object script engine has invalid limits"));
    if (auto installed = game.galactic_scripts()->set_object_engine(std::move(engine)); !installed) return installed;
    if (game.story_engine()) return game.connect_story_link(link);
    return core::Result<void>::success();
}

core::Result<void> install_world_object_scripts(GameSession& game, std::shared_ptr<const GalacticObjectScripts> content,
    auth::SessionConfig config, const GalacticStoryPlayers& players, const std::shared_ptr<GalacticStoryLink>& link,
    GalacticObjectEffectsFactory effects, std::size_t max_instances) {
    if (!content || !link || !effects || !max_instances)
        return core::Result<void>::failure(problem("world object scripts require prepared content and services"));
    if (content->types().empty()) return core::Result<void>::success();
    auto objects = content->create_scheduler(config);
    if (!objects) return core::Result<void>::failure(objects.error());
    if (auto bound = register_galactic_object_bindings(objects.value(), players, link); !bound) return bound;
    if (auto* scripts = game.galactic_scripts()) {
        if (auto installed = scripts->set_object_scheduler(std::move(objects).value()); !installed) return installed;
    } else {
        auto ai = auth::ScriptScheduler::create(config, {});
        if (!ai) return core::Result<void>::failure(ai.error());
        auto story = auth::ScriptScheduler::create(config, {});
        if (!story) return core::Result<void>::failure(story.error());
        if (auto installed = game.install_galactic_scripts(std::move(ai).value(), std::move(story).value(), {},
                std::move(objects).value()); !installed) return installed;
    }
    return configure_world_object_scripts(game, std::move(content), link, std::move(effects), max_instances);
}

// The one scheduler builder: the public one searches Library and Story, the scoring-definitions run also Miscellaneous (the
// authored scoring module requires its siblings there). Same API set, same bindings.
static core::Result<auth::ScriptScheduler> build_story_scheduler(const GalacticStoryScripts& content, auth::SessionConfig base,
    bool multiplayer, const GalacticStoryPlayers& players, auth::GalacticSnapshotReader reader, auth::GlobalValueReader globals,
    const std::shared_ptr<GalacticStoryLink>& link_in, std::vector<std::string> directories);

core::Result<void> GalacticStoryScripts::pin_scoring_module(const vfs::Vfs& files, GalacticStoryScripts& into) {
    using Void = core::Result<void>;
    // WGP-111: the module is the one the authored constants name, in the effective layering.
    const auto constants = data::load_document(files, "data/xml/gameconstants.xml");
    if (!constants) return Void::success();
    std::string module;
    for (const auto& child : constants.value().root.children) {
        if (child.name != "Game_Scoring_Script_Name") continue;
        data::tag_trace::used(child);
        module = core::trim(child.raw_text);
    }
    if (module.empty()) return Void::success();
    if (module.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) {
        into.notes_.push_back("scoring definitions: Game_Scoring_Script_Name " + module + " is not a module name; its definitions did not run");
        return Void::success();
    }
    const std::string path = std::string(miscellaneous_prefix) + lower(module) + ".lua";
    auto assets = files.enumerate("data/scripts", ".lua");
    if (!assets) return Void::failure(assets.error());
    // The scoring module and every sibling it may require; Library and Story are pinned already.
    bool authored = false;
    std::map<std::string, std::string> siblings;
    for (const auto& asset : assets.value()) {
        if (!asset.canonical_path.starts_with(miscellaneous_prefix) || siblings.contains(asset.canonical_path)) continue;
        authored |= asset.canonical_path == path;
        auto bytes = files.open(asset.canonical_path);
        if (!bytes) return Void::failure(bytes.error());
        std::string text(bytes.value().size(), '\0');
        if (!text.empty()) std::memcpy(text.data(), bytes.value().data(), bytes.value().size());
        siblings.emplace(asset.canonical_path, std::move(text));
    }
    if (!authored) return Void::success(); // named module is not authored: nothing to run, as at the campaign end
    into.scoring_module_ = path;
    into.scoring_modules_ = std::move(siblings);
    return Void::success();
}

void GalacticStoryScripts::evaluate_scoring_definitions(const auth::SessionConfig& base, bool multiplayer, const GalacticStoryPlayers& players) const {
    initial_globals_.clear();
    scoring_notes_.clear();
    if (scoring_module_.empty()) return;
    GalacticStoryScripts sandbox;
    sandbox.modules_ = modules_;
    for (const auto& [path, text] : scoring_modules_) sandbox.modules_.emplace(path, text);
    Entry entry;
    entry.subplot = "scoring definitions";
    entry.script = scoring_module_;
    entry.module = scoring_module_;
    entry.instance = first_story_script_instance;
    sandbox.entries_.push_back(entry);
    auto scheduler = build_story_scheduler(sandbox, base, multiplayer, players, {}, {}, {},
        {std::string(library_prefix_directory), std::string(story_directory), std::string(miscellaneous_directory)});
    if (!scheduler) { scoring_notes_.push_back("scoring definitions: " + scoring_module_ + " did not run: " + scheduler.error().message); return; }
    if (auto created = scheduler.value().create_instance(entry.instance, entry.module); !created) {
        scoring_notes_.push_back("scoring definitions: " + scoring_module_ + " did not start: " + created.error().code + " " + created.error().message);
        return;
    }
    auth::ScriptEvent event;
    event.key = {scheduler.value().completed_tick() + 1, auth::first_simulation_producer, entry.instance, 0};
    event.target = entry.instance;
    event.kind = auth::ScriptEvent::Kind::call;
    event.name = "Base_Definitions";
    if (auto posted = scheduler.value().submit_event(std::move(event)); !posted) {
        scoring_notes_.push_back("scoring definitions: " + scoring_module_ + " did not run: " + posted.error().message);
        return;
    }
    auth::ServiceOptions options;
    options.time_tick = 1;
    const sim::InlineExecutor executor;
    auto report = scheduler.value().service(executor, options);
    if (!report) { scoring_notes_.push_back("scoring definitions: " + scoring_module_ + " did not run: " + report.error().message); return; }
    for (const auto& row : report.value().diagnostics)
        if (row.code != auth::codes::script_log) scoring_notes_.push_back("scoring definitions: " + scoring_module_ + ": " + row.code + " " + row.message);
    for (const auto& command : report.value().commands) {
        if (command.verb != auth::global_value_command || command.arguments.size() != 2) continue;
        const auto* name = std::get_if<std::string>(&command.arguments[0].data);
        if (name == nullptr || std::holds_alternative<std::monostate>(command.arguments[1].data)) continue; // a nil write starts nothing
        initial_globals_.insert_or_assign(*name, command.arguments[1]);
    }
}

core::Result<std::shared_ptr<const GalacticStoryScripts>> GalacticStoryScripts::load(
    const vfs::Vfs& files, std::span<const story::SubPlotDef> subplots) {
    using Result = core::Result<std::shared_ptr<const GalacticStoryScripts>>;
    auto out = std::make_shared<GalacticStoryScripts>();
    // Nothing to pin when no sub-plot names a script: the story scheduler stays empty, as before this host.
    if (std::none_of(subplots.begin(), subplots.end(), [](const auto& sub) { return !sub.script.empty(); }))
        return Result::success(std::move(out));
    auto assets = files.enumerate("data/scripts", ".lua");
    if (!assets) return Result::failure(assets.error());
    for (const auto& asset : assets.value()) {
        const auto& path = asset.canonical_path;
        if (!path.starts_with(library_prefix) && !path.starts_with(story_prefix)) continue;
        if (out->modules_.contains(path)) continue;
        auto bytes = files.open(path);
        if (!bytes) return Result::failure(bytes.error());
        std::string text(bytes.value().size(), '\0');
        if (!text.empty()) std::memcpy(text.data(), bytes.value().data(), bytes.value().size());
        out->modules_.emplace(path, std::move(text));
    }
    const auto names_symbol = [](const std::string& text, std::string_view symbol) {
        const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        for (auto at = text.find(symbol); at != std::string::npos; at = text.find(symbol, at + 1)) {
            const auto end = at + symbol.size();
            if ((at == 0 || !word(text[at - 1])) && (end >= text.size() || !word(text[end]))) return true;
        }
        return false;
    };
    for (const auto& [path, text] : out->modules_)
        if (names_symbol(text, "Spawn_Unit") || names_symbol(text, "Prevent_AI_Usage")) { out->uses_immediate_effects_ = true; break; }
    // #7073: the scoring module is pinned here; its definitions run once, first, in an instance of their own when the scheduler is made.
    if (auto pinned = pin_scoring_module(files, *out); !pinned) return Result::failure(pinned.error());
    std::uint64_t ordinal = 0;
    for (const auto& sub : subplots) {
        ++ordinal;
        if (sub.script.empty()) continue;
        Entry entry;
        entry.subplot = sub.name;
        entry.script = trimmed(sub.script);
        entry.instance = first_story_script_instance + ordinal;
        std::string path = std::string(story_directory) + entry.script;
        if (lower(path).ends_with(".lua") == false) path += ".lua";
        if (out->modules_.contains(lower(path))) entry.module = std::move(path);
        else out->notes_.push_back(sub.name + ": Lua_Script " + entry.script + " is not a Story module of the mounted data; the sub-plot runs no Lua (WST-53)");
        out->entries_.push_back(std::move(entry));
    }
    return Result::success(std::move(out));
}

const GalacticStoryScripts::Entry* GalacticStoryScripts::find(std::string_view subplot) const noexcept {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) { return entry.subplot == subplot; });
    return found == entries_.end() ? nullptr : &*found;
}

std::string GalacticStoryScripts::fingerprint() const {
    std::string text = "eawr-galactic-story-scripts-v2";
    const auto field = [&](std::string_view value) { text += std::to_string(value.size()) + ":"; text += value; };
    for (const auto& [path, bytes] : modules_) {
        field(path);
        const auto digest = core::sha256(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        field(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
    }
    for (const auto& entry : entries_) { field(entry.subplot); field(entry.script); field(entry.module); field(std::to_string(entry.instance)); }
    // The pinned scoring module runs at every scheduler start, so its bytes are part of the identity (no module: unchanged).
    if (!scoring_module_.empty()) {
        field("scoring-module"); field(scoring_module_);
        for (const auto& [path, bytes] : scoring_modules_) {
            field(path);
            const auto digest = core::sha256(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
            field(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
        }
    }
    return core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

core::Result<auth::ScriptScheduler> create_galactic_story_scheduler(const GalacticStoryScripts& content, auth::SessionConfig base,
    bool multiplayer, const GalacticStoryPlayers& players, auth::GalacticSnapshotReader reader,
    auth::GlobalValueReader globals, const std::shared_ptr<GalacticStoryLink>& link_in) {
    content.evaluate_scoring_definitions(base, multiplayer, players); // #7073: first, before any plot script exists
    return build_story_scheduler(content, std::move(base), multiplayer, players, std::move(reader), std::move(globals), link_in,
        {std::string(library_prefix_directory), std::string(story_directory)});
}

static core::Result<auth::ScriptScheduler> build_story_scheduler(const GalacticStoryScripts& content, auth::SessionConfig base,
    bool multiplayer, const GalacticStoryPlayers& players, auth::GalacticSnapshotReader reader,
    auth::GlobalValueReader globals, const std::shared_ptr<GalacticStoryLink>& link_in, std::vector<std::string> directories) {
    using Result = core::Result<auth::ScriptScheduler>;
    const auto link = link_in ? link_in : std::make_shared<GalacticStoryLink>();
    // An uncaught error ends the failing thread or call of a story script, not the script.
    base.retire_on_lua_error = false;
    base.label = "story";
    // Only scripted campaigns change anything: a story scheduler without scripts keeps its previous identity and hash.
    if (content.entries().empty()) return auth::ScriptScheduler::create(std::move(base), {});
    base.script_directories = std::move(directories);
    auth::ModuleManifest manifest;
    for (const auto& [path, bytes] : content.modules())
        if (auto added = manifest.add(path, bytes); !added) return Result::failure(added.error());
    if (auto added = manifest.add(story_random::distribution_module, std::string(story_random::distribution_source)); !added) return Result::failure(added.error());
    if (auto added = manifest.add(player_module, std::string(player_source)); !added) return Result::failure(added.error());
    auto scheduler = auth::ScriptScheduler::create(std::move(base), std::move(manifest));
    if (!scheduler) return scheduler;
    // #7073: the scoring definitions' values are the registry's starting values for the story scripts; any write replaces them.
    if (!content.initial_globals().empty())
        globals = [live = std::move(globals), start = content.initial_globals()](const std::string& key) -> std::optional<auth::Value> {
            if (live) if (auto found = live(key)) return found;
            const auto seeded = start.find(key);
            return seeded == start.end() ? std::nullopt : std::optional<auth::Value>(seeded->second);
        };
    if (auto added = auth::register_shared_global_values(scheduler.value(), std::move(globals)); !added)
        return Result::failure(added.error());
    // RGC-190..199: the weighted list and the two random calls. Everything else the stock and mod story starts need
    // that this host does not implement faults by name below.
    if (auto added = story_random::register_story_random(scheduler.value(), multiplayer); !added) return Result::failure(added.error());
    // WST-91/92: prepare the full faction database once; the first matching player is the lowest player-table ID.
    std::map<std::string, std::optional<sim::galactic::PlayerId>> factions;
    for (const auto& name : players.factions) factions.try_emplace(lower(name), std::nullopt);
    bool local_registered = false, shared_registered = false;
    for (const auto& player : players.players) {
        local_registered |= players.local_player && player.id == *players.local_player;
        shared_registered |= players.shared_local_player && player.id == *players.shared_local_player;
        const auto found = factions.find(lower(player.faction));
        if (found != factions.end() && (!found->second || player.id < *found->second)) found->second = player.id;
    }
    // Project deviation from WST-91: a per-peer local player cannot enter the multiplayer lockstep simulation.
    // The shared co-op faction is the one deterministic exception: every peer derives the same value.
    const auto local = multiplayer ? (shared_registered ? players.shared_local_player : std::nullopt)
                                   : (local_registered ? players.local_player : std::nullopt);
    if (auto added = scheduler.value().register_binding("EAWR_Story_Player.Lookup", [factions = std::move(factions), local](
            auth::BindingContext& context, const auth::ValueList& arguments) {
        using Lookup = core::Result<auth::ValueList>;
        const auto invalid = [&](std::string message) {
            context.report(auth::codes::invalid_request, std::move(message));
            context.request_exit();
            return Lookup::success({});
        };
        // WST-90: string-only, exact arity; diagnostics stop this script even without a log sink.
        if (arguments.size() != 1 || !std::holds_alternative<std::string>(arguments.front().data))
            return invalid("Find_Player requires exactly one string argument");
        const auto name = lower(std::get<std::string>(arguments.front().data));
        auto player = local;
        if (name != "local") {
            const auto found = factions.find(name);
            if (found == factions.end()) return invalid("Find_Player: unknown faction '" + std::get<std::string>(arguments.front().data) + "'");
            player = found->second;
        }
        // WST-93: the sandbox interns each player's handle independently in each script's wrapper registry.
        if (player) return Lookup::success({auth::Value{auth::Handle{script::foc::handle_player, *player}}});
        return Lookup::success({});
    }); !added) return Result::failure(added.error());
    std::map<std::uint64_t, std::string> difficulties;
    for (const auto& player : players.players) {
        const auto level = lower(player.difficulty);
        difficulties.emplace(player.id, level == "easy" ? "Easy" : level == "normal" ? "Normal" : level == "hard" ? "Hard" : player.difficulty);
    }
    // #7101 (L-51, GA-04): the active mode's name; the galactic story host only ever runs in the galactic mode.
    if (auto added = scheduler.value().register_binding("Get_Game_Mode", [](auth::BindingContext&, const auth::ValueList&) {
        return core::Result<auth::ValueList>::success({auth::Value::text("Galactic")});
    }); !added) return Result::failure(added.error());
    // #7102: Player.Is_Human for a registered player (its seat is human-controlled); anything else is a named fault, like Get_Difficulty.
    std::map<std::uint64_t, bool> humans;
    for (const auto& player : players.players) humans.emplace(player.id, player.human);
    if (auto added = scheduler.value().register_method(script::foc::handle_player, "Is_Human",
        [humans = std::move(humans)](auth::BindingContext&, const auth::ValueList& arguments) {
            using HumanResult = core::Result<auth::ValueList>;
            const auto* self = arguments.empty() ? nullptr : std::get_if<auth::Handle>(&arguments.front().data);
            if (!self || self->kind != script::foc::handle_player || arguments.size() != 1 || !humans.contains(self->id))
                return HumanResult::failure(problem("Is_Human requires a registered player"));
            return HumanResult::success({auth::Value{humans.at(self->id)}});
        }); !added) return Result::failure(added.error());
    if (auto added = scheduler.value().register_method(script::foc::handle_player, "Get_Difficulty",
        [difficulties = std::move(difficulties)](auth::BindingContext&, const auth::ValueList& arguments) {
            using DifficultyResult = core::Result<auth::ValueList>;
            const auto* self = arguments.empty() ? nullptr : std::get_if<auth::Handle>(&arguments.front().data);
            if (!self || self->kind != script::foc::handle_player || !difficulties.contains(self->id))
                return DifficultyResult::failure(problem("Get_Difficulty requires a registered player"));
            return DifficultyResult::success({auth::Value::text(difficulties.at(self->id))});
        }); !added) return Result::failure(added.error());
    if (auto added = auth::register_galactic_first_object(scheduler.value(), players.types, players.planets,
        std::move(reader)); !added) return Result::failure(added.error());
    std::set<std::uint64_t> advisor_players;
    for (const auto& player : players.players) advisor_players.insert(player.id);
    if (auto added = scheduler.value().register_method(script::foc::handle_player, "Enable_Advisor_Hints",
        [advisor_players = std::move(advisor_players)](auth::BindingContext& context, const auth::ValueList& arguments) {
            using HintsResult = core::Result<auth::ValueList>;
            const auto* self = arguments.empty() ? nullptr : std::get_if<auth::Handle>(&arguments.front().data);
            if (arguments.size() != 3 || !self || self->kind != script::foc::handle_player
                || !advisor_players.contains(self->id) || !std::get_if<std::string>(&arguments[1].data)
                || !std::get_if<bool>(&arguments[2].data))
                return HintsResult::failure(problem("Enable_Advisor_Hints requires a registered player, mode string and boolean"));
            // Retail changes a per-player advisor presentation preference and
            // returns nothing. The advisor presentation consumer is not hosted.
            context.report(auth::codes::unsupported_api,
                "Enable_Advisor_Hints presentation preference is unavailable; story script continues");
            return HintsResult::success({});
        }); !added) return Result::failure(added.error());
    if (auto added = scheduler.value().register_prelude(player_module); !added) return Result::failure(added.error());
    std::map<std::string, sim::galactic::PlanetId> planets;
    for (const auto& [name, id] : players.planets) {
        const auto key = lower(name);
        const auto found = planets.find(key);
        if (found == planets.end() || id < found->second) planets.insert_or_assign(key, id);
    }
    if (auto added = scheduler.value().register_binding("FindPlanet", [planets = std::move(planets)](
            auth::BindingContext& context, const auth::ValueList& arguments) {
        using Lookup = core::Result<auth::ValueList>;
        if (arguments.empty() || !std::holds_alternative<std::string>(arguments.front().data)) {
            context.report(auth::codes::invalid_request, "FindPlanet requires a string first argument");
            context.request_exit();
            return Lookup::success({});
        }
        const auto found = planets.find(lower(std::get<std::string>(arguments.front().data)));
        if (found == planets.end()) return Lookup::success({});
        return Lookup::success({auth::Value{auth::Handle{script::foc::handle_game_object, found->second}}});
    }); !added) return Result::failure(added.error());
    // RGC-176..178: type lookup, creation and the AI hand-off. Creation and the free-store write run in the ordered
    // native shadow of the story phase (the same journaled creation as the AI's forced production), so identities,
    // streams and the replay journal are the session's own on every peer and worker count.
    std::map<std::string, sim::galactic::TypeId> type_names;
    for (const auto& [name, id] : players.types) {
        const auto key = lower(name);
        const auto found = type_names.find(key);
        if (found == type_names.end() || id < found->second) type_names.insert_or_assign(key, id);
    }
    if (auto added = scheduler.value().register_binding("Find_Object_Type", [type_names = std::move(type_names)](
            auth::BindingContext& context, const auth::ValueList& arguments) {
        using Lookup = core::Result<auth::ValueList>;
        if (arguments.empty() || !std::holds_alternative<std::string>(arguments.front().data)) {
            context.report(auth::codes::invalid_request, "Find_Object_Type requires a string first argument");
            context.request_exit();
            return Lookup::success({});
        }
        const auto found = type_names.find(lower(std::get<std::string>(arguments.front().data)));
        if (found == type_names.end()) return Lookup::success({});
        return Lookup::success({auth::Value{auth::Handle{script::foc::handle_type, found->second}}});
    }); !added) return Result::failure(added.error());
    std::set<std::uint64_t> spawn_players, spawn_planets;
    for (const auto& player : players.players) spawn_players.insert(player.id);
    for (const auto& planet : players.planets) spawn_planets.insert(planet.second);
    if (auto added = scheduler.value().register_binding("Spawn_Unit", [spawn_players = std::move(spawn_players), spawn_planets = std::move(spawn_planets)](
            auth::BindingContext& context, const auth::ValueList& arguments) {
        using Spawn = core::Result<auth::ValueList>;
        const auto refuse = [&](std::string message) {
            context.report(auth::codes::invalid_request, std::move(message));
            context.request_exit();
            return Spawn::success({});
        };
        // RGC-176: three arguments (extra ones are ignored), each checked in order; nothing is created on a refusal.
        if (arguments.size() < 3) return refuse("Spawn_Unit requires a type, a planet and a player");
        const auto* type = std::get_if<auth::Handle>(&arguments[0].data);
        if (!type || type->kind != script::foc::handle_type || !type->id) return refuse("Spawn_Unit: parameter 1 is not an object type");
        const auto* planet = std::get_if<auth::Handle>(&arguments[1].data);
        if (!planet || planet->kind != script::foc::handle_game_object || !spawn_planets.contains(planet->id))
            return refuse("Spawn_Unit: parameter 2 is not a planet");
        const auto* player = std::get_if<auth::Handle>(&arguments[2].data);
        if (!player || player->kind != script::foc::handle_player || !spawn_players.contains(player->id))
            return refuse("Spawn_Unit: parameter 3 is not a player");
        auto made = context.apply_immediate("galactic.spawn_unit", {arguments[2], arguments[1], arguments[0]});
        if (!made) return made;
        if (made.value().empty()) {
            // RGC-178: a null creation is a script warning and no table.
            context.report(auth::codes::script_warning, "Spawn_Unit: the type created no object");
            return Spawn::success({});
        }
        return made;
    }); !added) return Result::failure(added.error());
    if (auto added = scheduler.value().register_method(script::foc::handle_game_object, "Prevent_AI_Usage",
        [](auth::BindingContext& context, const auth::ValueList& arguments) {
            using Prevent = core::Result<auth::ValueList>;
            const auto* actor = arguments.size() == 2 ? std::get_if<auth::Handle>(&arguments[0].data) : nullptr;
            const auto* flag = arguments.size() == 2 ? std::get_if<bool>(&arguments[1].data) : nullptr;
            if (!actor || actor->kind != script::foc::handle_game_object || !flag)
                return Prevent::failure(problem("Prevent_AI_Usage requires an object and a Boolean"));
            auto applied = context.apply_immediate("galactic.story_prevent_ai_usage", arguments);
            if (!applied) return applied;
            const auto* done = !applied.value().empty() && applied.value().size() <= 2 ? std::get_if<bool>(&applied.value()[0].data) : nullptr;
            if (!done) return Prevent::failure(problem("Prevent_AI_Usage native result is invalid"));
            // L-57: no AI pool for the owner (a human seat) is a warning, and the script goes on. An object the AI
            // free store does not track is a different warning.
            if (!*done) {
                const auto* reason = applied.value().size() == 2 ? std::get_if<std::string>(&applied.value()[1].data) : nullptr;
                context.report(auth::codes::script_warning, reason && *reason == "actor"
                    ? "Prevent_AI_Usage: the object is not known to the AI free store"
                    : "Prevent_AI_Usage: the owner has no initialized AI pool");
            }
            return Prevent::success({});
        }); !added) return Result::failure(added.error());
    // WST-56: the sub-plot scripts edit their plots (Lock_List/Unlock_List set the reward parameter, then send the event).
    {
        std::vector<core::Diagnostic> errors;
        script::foc::detail::register_story_plot_bindings(scheduler.value(),
            script::foc::StoryPlotAccess([link] { return link->impl().plots.link.get(); }), errors);
        if (!errors.empty()) return Result::failure(errors.front());
    }
    // WST-37/55: Story_Event(name[, object]) stages a STORY_AI_NOTIFICATION broadcast. The debug build's
    // The debug build dispatches the notification to the sub-plots at the call (WST-55), so the engine commits it at the call's place in
    // the script's command order, after the Set_Reward_Parameter calls before it (their values are captured per trigger).
    {
        std::map<std::uint64_t, std::string> names;
        for (const auto& [name, id] : players.planets) names.try_emplace(id, upper_text(name));
        if (auto added = scheduler.value().register_binding("Story_Event", [names = std::move(names)](
                auth::BindingContext& context, const auth::ValueList& arguments) {
            using Sent = core::Result<auth::ValueList>;
            if (arguments.empty() || arguments.size() > 2 || !std::holds_alternative<std::string>(arguments[0].data))
                return Sent::failure(problem("Story_Event expects a name and an optional object"));
            std::string planet;
            bool has_object = false;
            if (arguments.size() == 2 && !std::holds_alternative<std::monostate>(arguments[1].data)) {
                const auto* object = std::get_if<auth::Handle>(&arguments[1].data);
                if (!object || object->kind != script::foc::handle_game_object)
                    return Sent::failure(problem("Story_Event: the second argument must be a game object"));
                has_object = true;
                if (const auto found = names.find(object->id); found != names.end()) planet = found->second;
            }
            context.issue_command(std::string(verb_story_event), {auth::Value::text(std::get<std::string>(arguments[0].data)),
                auth::Value::text(has_object ? "1" : "0"), auth::Value::text(planet)});
            return Sent::success({});
        }); !added) return Result::failure(added.error());
    }
    // Check_Story_Flag(player, name[, planet[, reset]]): whether a TRIGGER_AI reward set (player, planet, name); `reset`
    // clears it. The read sees the flags as of the end of the last story step; the clear is committed in command order
    // after the service, so a flag consumed by one script is gone for every later service.
    if (auto added = scheduler.value().register_binding("Check_Story_Flag", [link](
            auth::BindingContext& context, const auth::ValueList& arguments) {
        using Checked = core::Result<auth::ValueList>;
        if (arguments.size() < 2 || arguments.size() > 4 || !std::holds_alternative<std::string>(arguments[1].data))
            return Checked::failure(problem("Check_Story_Flag expects a player, a flag name, an optional planet and an optional reset"));
        const auto* player = std::get_if<auth::Handle>(&arguments[0].data);
        if (!player && !std::holds_alternative<std::monostate>(arguments[0].data))
            return Checked::failure(problem("Check_Story_Flag: the first argument must be a player"));
        std::uint64_t planet = 0;
        if (arguments.size() > 2 && !std::holds_alternative<std::monostate>(arguments[2].data)) {
            const auto* object = std::get_if<auth::Handle>(&arguments[2].data);
            if (!object || object->kind != script::foc::handle_game_object)
                return Checked::failure(problem("Check_Story_Flag: the planet must be a game object or nil"));
            planet = object->id;
        }
        bool reset = false;
        if (arguments.size() > 3 && !std::holds_alternative<std::monostate>(arguments[3].data)) {
            const auto* flag = std::get_if<bool>(&arguments[3].data);
            if (!flag) return Checked::failure(problem("Check_Story_Flag: reset must be a Boolean or nil"));
            reset = *flag;
        }
        // No such player (a faction that is not in this game): nothing can have set a flag for it.
        if (!player || player->kind != script::foc::handle_player) return Checked::success({auth::Value{false}});
        const auto& name = std::get<std::string>(arguments[1].data);
        const auto* sink = link->impl().sink.get();
        const bool set = sink && sink->flag(static_cast<story::Player>(player->id), planet, name);
        if (set && reset)
            context.issue_command(std::string(verb_story_flag_clear), {auth::Value::text(std::to_string(player->id)),
                auth::Value::text(std::to_string(planet)), auth::Value::text(name)});
        return Checked::success({auth::Value{set}});
    }); !added) return Result::failure(added.error());
    // Letter_Box_In(seconds) / Letter_Box_Out(seconds) (legacy #6634): the cinematic bands slide in or out over `seconds`
    // (0 = at once; stock scripts pass 0 and 2). Staged like Story_Event and committed in the service's command order, where
    // the session makes a local presentation request for the plot owner's seat: nothing here reaches the simulation. A call
    // without exactly one finite, non-negative number is a named script fault for that thread, never a match abort.
    for (const auto& [name, direction] : std::array<std::pair<std::string_view, std::string_view>, 2>{{{"Letter_Box_In", "in"}, {"Letter_Box_Out", "out"}}}) {
        if (auto added = scheduler.value().register_binding(std::string(name),
                auth::letter_box_binding(std::string(name), std::string(direction), std::string(verb_letter_box))); !added)
            return Result::failure(added.error());
    }
    // Debug sinks (PGDebug, NEW-L22): the engine writes them to its logs; here they have no effect and write no file.
    for (const std::string_view sink : {"_OuputDebug", "_ScriptMessage", "_CustomScriptMessage", "_MessagePopup", "DumpCallStack"})
        if (auto added = scheduler.value().register_binding(sink, [](auth::BindingContext&, const auth::ValueList&) {
                return core::Result<auth::ValueList>::success(auth::ValueList{}); }); !added) return Result::failure(added.error());
    std::vector<std::string_view> implemented{"GlobalValue.Get", "GlobalValue.Set", "Get_Game_Mode", "Find_First_Object", "Find_Object_Type", "Spawn_Unit", "FindPlanet", "Find_Player", "DiscreteDistribution.Create", "GameRandom", "_OuputDebug", "_ScriptMessage",
        "_CustomScriptMessage", "_MessagePopup", "DumpCallStack", "Get_Story_Plot", "Story_Event", "Check_Story_Flag", "Letter_Box_In", "Letter_Box_Out"};
    // `Object`, `Target`, `Budget`, `FreeStore` and `PlayerObject` are variables of the scripts (Common_Base_Definitions
    // clears them), and the inventory's "Object.x" names are methods of the wrappers they hold; bound as global tables they
    // would be protected and fault every Base_Definitions. Calls on them fail as an index of nil until the wrappers exist.
    for (const std::string_view name : auth::missing_galactic_api_names()) {
        const std::string_view receiver = name.substr(0, name.find('.'));
        if (receiver != name && (receiver == "Object" || receiver == "Target" || receiver == "Budget" || receiver == "FreeStore" || receiver == "PlayerObject")) implemented.push_back(name);
    }
    if (auto missing = auth::register_missing_galactic_api(scheduler.value(), implemented); !missing) return Result::failure(missing.error());
    return scheduler;
}

core::Result<void> start_galactic_story_scripts(auth::ScriptScheduler& scripts, const GalacticStoryScripts& content,
    std::vector<std::string>& notes) {
    for (const auto& line : content.scoring_notes()) notes.push_back(line);
    for (const auto& entry : content.entries()) {
        if (entry.module.empty()) continue;
        if (auto created = scripts.create_instance(entry.instance, entry.module); !created) {
            // A chunk that fails to load leaves no instance (create_instance): the campaign goes on without it.
            notes.push_back(entry.subplot + ": Lua_Script " + entry.script + " did not start: " + created.error().code + " " + created.error().message);
            continue;
        }
        const std::uint64_t tick = scripts.completed_tick() + 1;
        // The initialization events the engine gives a fresh script before its first pump (L-20, L-27, WST-53).
        for (std::uint64_t step = 0; step < 2; ++step) {
            auth::ScriptEvent event;
            event.key = {tick, auth::first_simulation_producer, entry.instance, step};
            event.target = entry.instance;
            event.kind = step == 0 ? auth::ScriptEvent::Kind::call : auth::ScriptEvent::Kind::start_thread;
            event.name = step == 0 ? "Base_Definitions" : "main";
            if (auto posted = scripts.submit_event(std::move(event)); !posted) return posted;
        }
    }
    return core::Result<void>::success();
}

core::Result<void> forward_galactic_story_calls(auth::ScriptScheduler& scripts, const GalacticStoryScripts& content,
    std::span<const story::SubPlotDef> definitions, std::span<const story::ScriptCall> calls, std::uint64_t& sequence) {
    if (calls.empty()) return core::Result<void>::success();
    const auto running = scripts.instances();
    for (const auto& call : calls) {
        if (call.subplot >= definitions.size()) return core::Result<void>::failure(problem("story call names a sub-plot outside the plot table"));
        const auto* entry = content.find(definitions[call.subplot].name);
        if (entry == nullptr || !std::binary_search(running.begin(), running.end(), entry->instance)) continue;
        auth::ScriptEvent event;
        event.key = {scripts.completed_tick() + 1, auth::galactic_story_call_producer, entry->instance, ++sequence};
        event.target = entry->instance;
        event.kind = auth::ScriptEvent::Kind::call;
        event.name = "Story_Event_Trigger";
        event.arguments.push_back(auth::Value::text(call.event));
        if (auto posted = scripts.submit_event(std::move(event)); !posted) return posted;
    }
    return core::Result<void>::success();
}

} // namespace eawr::campaign
