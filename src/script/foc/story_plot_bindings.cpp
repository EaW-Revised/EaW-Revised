#include "story_plot_bindings.hpp"

#include "host.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace eawr::script::foc {

std::shared_ptr<StoryPlotLink> StoryPlotLink::from(std::span<const sim::galactic::story::SubPlotDef> definitions,
    std::function<core::Result<void>(const sim::galactic::story::PlotCommand&)> apply) {
    auto link = std::make_shared<StoryPlotLink>();
    link->apply = std::move(apply);
    for (const auto& definition : definitions) {
        Plot plot;
        plot.name = definition.name;
        for (std::uint32_t index = 0; index < definition.events.size(); ++index)
            plot.events.emplace(definition.events[index].name, index);
        // The first sub-plot of a repeated file name wins, as in the engine's own lookup.
        link->by_name.emplace(plot.name, static_cast<std::uint32_t>(link->plots.size()));
        link->plots.push_back(std::move(plot));
    }
    return link;
}

namespace detail {
namespace {
using Op = sim::galactic::story::PlotCommand::Op;

constexpr std::string_view op_names[] = {"activate", "suspend", "reset", "event_param", "reward_param", "reward_type",
    "dialog", "add_dialog_text", "clear_dialog_text"};

// A Lua number is safe to narrow to 32 bits only when it is finite and inside the range; NaN, infinities and huge
// values would be undefined behaviour in the cast, so they become script errors.
bool fits_int32(LuaNumber value) {
    // NaN compares false and infinities fall outside the bounds, so the range test alone rejects them.
    return value > LuaNumber(-2147483000) && value < LuaNumber(2147483000);
}

// Lua numbers reach the XML token lists as the text the parser would read: integers plainly, anything else as a
// decimal of at most six places.
std::string number_text(LuaNumber value) {
    const auto whole = static_cast<std::int32_t>(value);
    const LuaNumber rest = value - LuaNumber(whole);
    if (rest == LuaNumber(0)) return std::to_string(whole);
    const auto scaled = static_cast<std::int32_t>(rest * LuaNumber(1000000));
    std::string digits = std::to_string(std::abs(scaled));
    digits.insert(0, 6 - std::min<std::size_t>(6, digits.size()), '0');
    while (!digits.empty() && digits.back() == '0') digits.pop_back();
    const bool negative = whole < 0 || (whole == 0 && scaled < 0);
    return std::string(negative ? "-" : "") + std::to_string(std::abs(whole)) + (digits.empty() ? "" : "." + digits);
}

// A parameter value: a string as written or a number (WST-56).
const char* token_of(const Value& value, std::string& out) {
    if (const auto* text = as_text(value)) { out = *text; return nullptr; }
    if (const auto* number = std::get_if<LuaNumber>(&value.data)) {
        if (!fits_int32(*number)) return "expects a finite number within the 32-bit range";
        out = number_text(*number); return nullptr;
    }
    return "expects a string or a number";
}

// Dialog arguments keep their kind for the presentation that resolves them (WST-56: object, type, text id, integer).
const char* dialog_argument(const Value& value, std::string& out) {
    if (const auto* text = as_text(value)) { out = "text:" + *text; return nullptr; }
    if (const auto* number = std::get_if<LuaNumber>(&value.data)) {
        if (!fits_int32(*number)) return "accepts a game object, an object type, a string or a finite number within the 32-bit range";
        out = "int:" + std::to_string(static_cast<std::int32_t>(*number)); return nullptr;
    }
    if (const auto* object = as_handle(value, handle_game_object)) { out = "object:" + std::to_string(object->id); return nullptr; }
    if (const auto* type = as_handle(value, handle_type)) { out = "type:" + std::to_string(type->id); return nullptr; }
    return "accepts a game object, an object type, a string or a number";
}

const StoryPlotLink::Plot* plot_of(const StoryPlotLink& link, const Handle& handle) {
    return handle.id >= 1 && handle.id <= link.plots.size() ? &link.plots[handle.id - 1] : nullptr;
}

void stage_plot(BindingContext& context, Op op, std::string subplot, std::string event, std::uint32_t index, std::vector<std::string> tokens) {
    ValueList arguments;
    arguments.push_back(Value::text(std::string(op_names[static_cast<std::size_t>(op)])));
    arguments.push_back(Value::text(std::move(subplot)));
    arguments.push_back(Value::text(std::move(event)));
    arguments.push_back(Value::number(LuaNumber(index)));
    for (auto& token : tokens) arguments.push_back(Value::text(std::move(token)));
    context.issue_command(std::string(verb_story_plot), std::move(arguments));
}
} // namespace

void register_story_plot_bindings(ScriptScheduler& scripts, const std::shared_ptr<Host>& host, std::vector<core::Diagnostic>& errors) {
    const auto add = [&](auto registration) { if (!registration) errors.push_back(registration.error()); };
    add(scripts.register_binding("Get_Story_Plot", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 1 || !as_text(arguments[0])) return fail("Get_Story_Plot expects the sub-plot file name");
        const auto& link = host->story_plots;
        const auto found = link ? link->by_name.find(*as_text(arguments[0])) : std::map<std::string, std::uint32_t>::const_iterator{};
        if (!link || found == link->by_name.end()) {
            // WST-56: a warning and nothing. The plot is looked up by the name as written.
            context.report(authoritative::codes::script_warning, "Get_Story_Plot: no loaded sub-plot named " + *as_text(arguments[0]));
            return none();
        }
        return one(handle(handle_story_plot, std::uint64_t{found->second} + 1));
    }));
    const auto plot_method = [&](std::string_view name, Op op) {
        add(scripts.register_method(handle_story_plot, name, [host, op](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
            const auto* self = arguments.empty() ? nullptr : as_handle(arguments[0], handle_story_plot);
            const auto* plot = self && host->story_plots ? plot_of(*host->story_plots, *self) : nullptr;
            if (!plot || arguments.size() != 1) return fail("plot method expects a loaded plot and no arguments");
            stage_plot(context, op, plot->name, {}, 0, {});
            return none();
        }));
    };
    plot_method("Activate", Op::activate);
    plot_method("Suspend", Op::suspend);
    plot_method("Reset", Op::reset);
    add(scripts.register_method(handle_story_plot, "Get_Event", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto* self = arguments.empty() ? nullptr : as_handle(arguments[0], handle_story_plot);
        const auto* plot = self && host->story_plots ? plot_of(*host->story_plots, *self) : nullptr;
        if (!plot || arguments.size() != 2 || !as_text(arguments[1])) return fail("Get_Event expects a loaded plot and an event name");
        const auto event = plot->events.find(upper(*as_text(arguments[1])));
        if (event == plot->events.end()) return fail("Get_Event: the plot has no event named " + *as_text(arguments[1])); // WST-56: a script error
        return one(handle(handle_story_event, (self->id << 32) | (std::uint64_t{event->second} + 1)));
    }));
    // Resolves an event object to its plot and event names; null when it names nothing loaded.
    const auto event_of = [host](const ValueList& arguments, const StoryPlotLink::Plot*& plot, std::string& event) {
        const auto* self = arguments.empty() ? nullptr : as_handle(arguments[0], handle_story_event);
        if (!self || !host->story_plots) return false;
        const auto plot_index = self->id >> 32, event_index = self->id & 0xffffffffU;
        if (plot_index < 1 || plot_index > host->story_plots->plots.size()) return false;
        plot = &host->story_plots->plots[plot_index - 1];
        for (const auto& [name, index] : plot->events) if (index + 1 == event_index) { event = name; return true; }
        return false;
    };
    add(scripts.register_method(handle_story_event, "Is_Valid", [event_of](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const StoryPlotLink::Plot* plot{}; std::string event;
        return one(boolean(event_of(arguments, plot, event)));
    }));
    const auto indexed = [&](std::string_view name, Op op, std::size_t slots) {
        add(scripts.register_method(handle_story_event, name, [event_of, op, slots, label = std::string(name)](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
            const StoryPlotLink::Plot* plot{}; std::string event;
            if (!event_of(arguments, plot, event)) return fail(label + " requires a valid event");
            const auto* position = arguments.size() > 1 ? std::get_if<LuaNumber>(&arguments[1].data) : nullptr;
            if (!position || arguments.size() < 3) return fail(label + " expects a parameter index and its value");
            if (!fits_int32(*position)) return fail(label + ": parameter index is not a finite 32-bit number");
            // WST-56: the index is rounded to the nearest integer and counts from 0.
            auto rounded = static_cast<std::int32_t>(*position);
            const LuaNumber rest = *position - LuaNumber(rounded);
            if (rest * LuaNumber(2) >= LuaNumber(1)) ++rounded;
            else if (rest * LuaNumber(2) <= LuaNumber(-1)) --rounded;
            if (rounded < 0 || static_cast<std::size_t>(rounded) >= slots) return fail(label + ": parameter index outside the event's slots");
            std::vector<std::string> tokens;
            for (std::size_t at = 2; at < arguments.size(); ++at) {
                std::string token;
                if (const auto* problem = token_of(arguments[at], token)) return fail(label + " " + problem);
                tokens.push_back(std::move(token));
            }
            stage_plot(context, op, plot->name, event, static_cast<std::uint32_t>(rounded), std::move(tokens));
            return none();
        }));
    };
    indexed("Set_Event_Parameter", Op::event_param, sim::galactic::story::token_slots_event);
    indexed("Set_Reward_Parameter", Op::reward_param, sim::galactic::story::token_slots_reward);
    const auto named = [&](std::string_view name, Op op) {
        add(scripts.register_method(handle_story_event, name, [event_of, op, label = std::string(name)](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
            const StoryPlotLink::Plot* plot{}; std::string event;
            if (!event_of(arguments, plot, event)) return fail(label + " requires a valid event");
            if (arguments.size() != 2 || !as_text(arguments[1])) return fail(label + " expects a name");
            stage_plot(context, op, plot->name, event, 0, {*as_text(arguments[1])});
            return none();
        }));
    };
    named("Set_Reward_Type", Op::reward_type);
    named("Set_Dialog", Op::dialog);
    add(scripts.register_method(handle_story_event, "Clear_Dialog_Text", [event_of](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const StoryPlotLink::Plot* plot{}; std::string event;
        if (!event_of(arguments, plot, event) || arguments.size() != 1) return fail("Clear_Dialog_Text requires a valid event and no arguments");
        stage_plot(context, Op::clear_dialog_text, plot->name, event, 0, {});
        return none();
    }));
    add(scripts.register_method(handle_story_event, "Add_Dialog_Text", [event_of](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const StoryPlotLink::Plot* plot{}; std::string event;
        if (!event_of(arguments, plot, event)) return fail("Add_Dialog_Text requires a valid event");
        // WST-56: the text id, then up to two formatting arguments.
        if (arguments.size() < 2 || arguments.size() > 4 || !as_text(arguments[1])) return fail("Add_Dialog_Text expects a text id and up to two arguments");
        std::vector<std::string> tokens{"text:" + *as_text(arguments[1])};
        for (std::size_t at = 2; at < arguments.size(); ++at) {
            std::string token;
            if (const auto* problem = dialog_argument(arguments[at], token)) return fail(std::string("Add_Dialog_Text argument ") + problem);
            tokens.push_back(std::move(token));
        }
        stage_plot(context, Op::add_dialog_text, plot->name, event, 0, std::move(tokens));
        return none();
    }));
}

std::string commit_story_plot(const Host& host, const authoritative::ScriptCommand& command) {
    const auto& link = host.story_plots;
    if (!link || !link->apply) return "no plot engine is hosted";
    const auto& arguments = command.arguments;
    if (arguments.size() < 4 || !as_text(arguments[0]) || !as_text(arguments[1]) || !as_text(arguments[2])
        || !std::holds_alternative<LuaNumber>(arguments[3].data)) return "malformed plot request";
    sim::galactic::story::PlotCommand plot;
    const auto found = std::find(std::begin(op_names), std::end(op_names), *as_text(arguments[0]));
    if (found == std::end(op_names)) return "unknown plot operation";
    plot.op = static_cast<Op>(found - std::begin(op_names));
    plot.subplot = *as_text(arguments[1]);
    plot.event = *as_text(arguments[2]);
    plot.index = static_cast<std::uint32_t>(std::get<LuaNumber>(arguments[3].data));
    for (std::size_t at = 4; at < arguments.size(); ++at) {
        const auto* token = as_text(arguments[at]);
        if (!token) return "malformed plot request";
        plot.values.push_back(*token);
    }
    auto applied = link->apply(plot);
    return applied ? std::string{} : applied.error().message;
}
} // namespace detail
} // namespace eawr::script::foc
