// Contracts of the battle hint lookups and radar blip calls (BHR rules; docs/behaviour/battle-hint-radar-lua.md):
// Find_Hint, Find_All_Objects_With_Hint, Add_Radar_Blip and Remove_Radar_Blip of a tactical script. The world is the
// synthetic contract fixture; the production bindings run on it.
#include "../../support/env.hpp"
#include "foc_ai_contract_fixtures.hpp"
#include "tactical_ai_world.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace foc_contract {

namespace {

constexpr tactical::TypeId marker_type = 5;
constexpr tactical::TypeId other_marker_type = 6;
constexpr tactical::TypeId catalog_only_type = 7;

constexpr eawr::sim::EntityId marker_a = 40;
constexpr eawr::sim::EntityId marker_b = 41;
constexpr eawr::sim::EntityId marker_c = 42;
constexpr eawr::sim::EntityId other_marker = 43;
constexpr eawr::sim::EntityId gone_marker = 44; // listed as a hint object, not live in the view

struct HintWorld {
    World world;
    std::unique_ptr<auth::ScriptScheduler> scripts;
    std::uint64_t sequence{};

    HintWorld() {
        for (const auto& [id, name] : {std::pair{marker_type, "MARKER_GENERIC"}, std::pair{other_marker_type, "MARKER_OTHER"}}) {
            foc::AiType type;
            type.type_id = id;
            type.name = name; // upper case, as the host keys its names
            world.host->setup.content.types.push_back(type);
        }
        world.host->types.clear();
        world.host->types_by_name.clear();
        for (const foc::AiType& entry : world.host->setup.content.types) {
            world.host->types.emplace(entry.type_id, &entry);
            world.host->types_by_name.emplace(entry.name, &entry);
        }
        using Names = std::vector<std::pair<std::string, tactical::TypeId>>;
        world.host->setup.content.catalog_types = std::make_shared<const Names>(Names{{"CATALOG_ONLY", catalog_only_type}});
        for (const auto& [id, type] : {std::pair{marker_a, marker_type}, std::pair{marker_b, marker_type},
                 std::pair{marker_c, marker_type}, std::pair{other_marker, other_marker_type}}) {
            ai::ViewUnit row;
            row.id = id;
            row.type = type;
            row.owner = 1;
            row.visible_to = 3;
            world.view->units.push_back(row);
        }
        world.host->setup.hint_objects_hosted = true;
        // Creation order: A (dest0), B (dest1), C (dest0 again), the other type (dest0), a dead one (dest0).
        world.host->setup.hint_objects = {
            {marker_type, "dest0", marker_a}, {marker_type, "Dest1", marker_b}, {marker_type, "DEST0", marker_c},
            {other_marker_type, "dest0", other_marker}, {marker_type, "dest0", gone_marker}};
    }
    bool start() {
        auth::ModuleManifest manifest;
        const auto added = manifest.add("HINT.LUA", R"LUA(
            function Hint(a, b) found = Find_Hint(a, b) end
            function HintType(name) found = Find_Hint(Find_Object_Type(name), 'dest1') end
            function HintNone() Find_Hint() end
            function HintNumber() Find_Hint(5) end
            function All(hint) list = Find_All_Objects_With_Hint(hint) count = table.getn(list) first = list[1] second = list[2] third = list[3] end
            function AllNone() Find_All_Objects_With_Hint() end
            function AllTwo() Find_All_Objects_With_Hint('a', 'b') end
            function AllNumber() Find_All_Objects_With_Hint(5) end
            function Blip(a, b) Add_Radar_Blip(a, b) done = true end
            function BlipOne(a) Add_Radar_Blip(a) end
            function Unblip(a) Remove_Radar_Blip(a) done = true end
            function UnblipNone() Remove_Radar_Blip() end
            function UnblipTwo() Remove_Radar_Blip('a', 'b') end
        )LUA");
        expect(added.has_value(), "BHR: script fixture loads");
        auto created = auth::ScriptScheduler::create({}, std::move(manifest));
        expect(created.has_value(), "BHR: scheduler starts");
        if (!added || !created) return false;
        scripts = std::make_unique<auth::ScriptScheduler>(std::move(created.value()));
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(*scripts, world.host, errors);
        expect(errors.empty(), "BHR: production bindings register");
        expect(scripts->create_instance(1, "HINT.LUA").has_value(), "BHR: a script instance");
        return errors.empty();
    }
    std::optional<auth::ServiceReport> call(const std::string& name, auth::ValueList arguments) {
        auth::ScriptEvent event;
        event.key = {scripts->completed_tick() + 1, foc::producer_foc_engine, 1, sequence++};
        event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call;
        event.name = name;
        event.arguments = std::move(arguments);
        expect(event.name == name && scripts->submit_event(std::move(event)).has_value(), "BHR: call queues");
        const eawr::sim::InlineExecutor executor;
        auto report = scripts->service(executor);
        expect(report.has_value(), "BHR: call services");
        if (!report) return std::nullopt;
        return std::move(report.value());
    }
    std::optional<auth::Value> global(const std::string& name) {
        const auto value = scripts->read_global(1, name);
        return value && value.value() ? std::optional<auth::Value>(*value.value()) : std::nullopt;
    }
};

auth::Value text(const std::string& value) { return auth::Value::text(value); }
auth::Value number(const int value) { return foc::detail::number(eawr::script::numeric::LuaNumber(value)); }
auth::Value type_handle(const tactical::TypeId id) { return auth::Value{auth::Handle{foc::handle_type, id}}; }
auth::Value object_handle(const eawr::sim::EntityId id) { return auth::Value{auth::Handle{foc::handle_game_object, id}}; }

bool is_nil(const std::optional<auth::Value>& value) { return value && std::holds_alternative<std::monostate>(value->data); }
bool is_object(const std::optional<auth::Value>& value, const eawr::sim::EntityId id) {
    if (!value) return false;
    const auto* handle = std::get_if<auth::Handle>(&value->data);
    return handle != nullptr && handle->kind == foc::handle_game_object && handle->id == id;
}
bool is_count(const std::optional<auth::Value>& value, const int expected) {
    if (!value) return false;
    const auto* count = std::get_if<eawr::script::numeric::LuaNumber>(&value->data);
    return count != nullptr && *count == eawr::script::numeric::LuaNumber(expected);
}
bool one_fault(const std::optional<auth::ServiceReport>& report) { return report && report->diagnostics.size() == 1; }

void find_hint_cases() {
    HintWorld fixture;
    if (!fixture.start()) return;
    // A type and a hint: the first live object of that original type whose hint matches without case.
    auto report = fixture.call("Hint", {text("Marker_Generic"), text("DEST0")});
    expect(report && report->diagnostics.empty() && is_object(fixture.global("found"), marker_a),
        "BHR: Find_Hint answers the first object of the type with the hint, in creation order, without case");
    report = fixture.call("Hint", {text("MARKER_GENERIC"), text("dest1")});
    expect(report && is_object(fixture.global("found"), marker_b), "BHR: the hint picks the matching object, not the first");
    report = fixture.call("Hint", {type_handle(other_marker_type), text("dest0")});
    expect(report && is_object(fixture.global("found"), other_marker), "BHR: a type handle names the type as well");
    report = fixture.call("HintType", {text("MARKER_GENERIC")});
    expect(report && report->diagnostics.empty() && is_object(fixture.global("found"), marker_b),
        "BHR: Find_Object_Type feeds Find_Hint");
    // Without a hint string (or an empty one) the first object of the type answers.
    report = fixture.call("Hint", {text("MARKER_GENERIC")});
    expect(report && is_object(fixture.global("found"), marker_a), "BHR: no hint string answers the first object of the type");
    report = fixture.call("Hint", {text("MARKER_GENERIC"), text("")});
    expect(report && is_object(fixture.global("found"), marker_a), "BHR: an empty hint string matches any hint");
    // Nothing matches: nil, no error.
    report = fixture.call("Hint", {text("MARKER_GENERIC"), text("nowhere")});
    expect(report && report->diagnostics.empty() && is_nil(fixture.global("found")), "BHR: an unmatched hint answers nil");
    report = fixture.call("Hint", {text("CATALOG_ONLY")});
    expect(report && report->diagnostics.empty() && is_nil(fixture.global("found")),
        "BHR: a registered type with no hint object answers nil");
    report = fixture.call("Hint", {text("MARKER_OTHER"), text("dest1")});
    expect(report && is_nil(fixture.global("found")), "BHR: the hint must belong to an object of the asked type");
    // A non-string second argument counts as no hint string.
    report = fixture.call("Hint", {text("MARKER_GENERIC"), number(3)});
    expect(report && report->diagnostics.empty() && is_object(fixture.global("found"), marker_a),
        "BHR: a non-string second argument is ignored");
    // Errors: no argument, a wrong first argument, an unresolved name.
    expect(one_fault(fixture.call("HintNone", {})), "BHR: Find_Hint with no argument is a script error");
    expect(one_fault(fixture.call("HintNumber", {})), "BHR: Find_Hint with a number is a script error");
    expect(one_fault(fixture.call("Hint", {text("NO_SUCH_TYPE"), text("dest0")})),
        "BHR: an unknown type name is a script error");
    // The lookup follows the live objects.
    auto& units = fixture.world.view->units;
    units.erase(std::remove_if(units.begin(), units.end(),
        [](const foc::detail::ViewUnit& row) { return row.id == marker_a; }), units.end());
    report = fixture.call("Hint", {text("MARKER_GENERIC"), text("dest0")});
    expect(report && is_object(fixture.global("found"), marker_c), "BHR: a destroyed object no longer answers");
}

void find_all_cases() {
    HintWorld fixture;
    if (!fixture.start()) return;
    // Every live object with the hint, whatever its type, without case; the dead one is skipped.
    auto report = fixture.call("All", {text("Dest0")});
    expect(report && report->diagnostics.empty() && is_count(fixture.global("count"), 3),
        "BHR: Find_All_Objects_With_Hint lists every live object with the hint, any type");
    expect(is_object(fixture.global("first"), marker_a) && is_object(fixture.global("second"), marker_c)
            && is_object(fixture.global("third"), other_marker),
        "BHR: the table keeps creation order");
    report = fixture.call("All", {text("dest1")});
    expect(report && is_count(fixture.global("count"), 1), "BHR: a single match");
    report = fixture.call("All", {text("nowhere")});
    expect(report && report->diagnostics.empty() && is_count(fixture.global("count"), 0), "BHR: no match is an empty table");
    report = fixture.call("All", {text("")});
    expect(report && is_count(fixture.global("count"), 0), "BHR: the empty hint matches nothing here");
    expect(one_fault(fixture.call("AllNone", {})), "BHR: no argument is a script error");
    expect(one_fault(fixture.call("AllTwo", {})), "BHR: two arguments are a script error");
    expect(one_fault(fixture.call("AllNumber", {})), "BHR: a non-string argument is a script error");
    // A battle without hint objects: nil and an empty table.
    fixture.world.host->setup.hint_objects.clear();
    // Map hint objects not hosted: the same answers, but never silent.
    fixture.world.host->setup.hint_objects_hosted = false;
    report = fixture.call("All", {text("dest0")});
    expect(report && report->diagnostics.size() == 1 && report->diagnostics.front().code == auth::codes::unsupported_api
            && is_count(fixture.global("count"), 0), "BHR: unhosted map hints note per call (Find_All_Objects_With_Hint)");
    report = fixture.call("Hint", {text("MARKER_GENERIC")});
    expect(report && report->diagnostics.size() == 1 && report->diagnostics.front().code == auth::codes::unsupported_api
            && is_nil(fixture.global("found")), "BHR: unhosted map hints note per call (Find_Hint)");
    fixture.world.host->setup.hint_objects_hosted = true;
    report = fixture.call("All", {text("dest0")});
    expect(report && report->diagnostics.empty() && is_count(fixture.global("count"), 0), "BHR: no hint objects, empty table");
    report = fixture.call("Hint", {text("MARKER_GENERIC"), text("dest0")});
    expect(report && report->diagnostics.empty() && is_nil(fixture.global("found")), "BHR: no hint objects, nil");
}

void radar_blip_cases() {
    HintWorld fixture;
    if (!fixture.start()) return;
    const auto noted = [](const std::optional<auth::ServiceReport>& report) {
        return report && report->diagnostics.size() == 1
            && report->diagnostics.front().code == auth::codes::unsupported_api;
    };
    // The accepted forms note that no blip is drawn and the script goes on.
    auto report = fixture.call("Blip", {object_handle(marker_b), text("beacon")});
    expect(noted(report) && report->commands.empty(), "BHR-04/05: Add_Radar_Blip on an object is noted and stages nothing");
    expect(fixture.global("done").has_value() && !is_nil(fixture.global("done")), "BHR-04/05: the script continues after Add_Radar_Blip");
    report = fixture.call("Blip", {foc::detail::position_handle(eawr::script::numeric::LuaNumber(3), eawr::script::numeric::LuaNumber(4)), text("beacon")});
    expect(noted(report) && report->commands.empty(), "BHR-04/05: Add_Radar_Blip on a position is noted and stages nothing");
    expect(one_fault(fixture.call("Blip", {auth::Value{auth::Handle{foc::handle_type, 1}}, text("beacon")})),
        "BHR-04/05: Add_Radar_Blip with a planet-style argument (galactic form, not covered) is a script error");
    report = fixture.call("Unblip", {text("beacon")});
    expect(noted(report) && report->commands.empty(), "BHR-04/05: Remove_Radar_Blip by name is noted and stages nothing");
    // Errors the original reports.
    expect(one_fault(fixture.call("BlipOne", {object_handle(marker_b)})), "BHR-04/05: Add_Radar_Blip with one argument is a script error");
    expect(one_fault(fixture.call("Blip", {text("nowhere"), text("beacon")})),
        "BHR-04/05: Add_Radar_Blip without a deducible position is a script error");
    expect(one_fault(fixture.call("Blip", {object_handle(marker_b), number(4)})),
        "BHR-04/05: Add_Radar_Blip with a non-string name is a script error");
    expect(one_fault(fixture.call("UnblipNone", {})), "BHR-04/05: Remove_Radar_Blip with no argument is a script error");
    expect(one_fault(fixture.call("UnblipTwo", {})), "BHR-04/05: Remove_Radar_Blip with two arguments is a script error");
    expect(one_fault(fixture.call("Unblip", {number(4)})), "BHR-04/05: Remove_Radar_Blip with a non-string name is a script error");
}

} // namespace

void hint_radar_bindings() {
    find_hint_cases();
    find_all_cases();
    radar_blip_cases();
}

} // namespace foc_contract
