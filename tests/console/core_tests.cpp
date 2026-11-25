#include "eawr/presentation/console/core.hpp"
#include "eawr/data/ui/text_database.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace eawr::presentation::console;
namespace {
void check(bool condition, const char* message) { if (!condition) { std::cerr << message << '\n'; std::exit(1); } }
struct Index final : NameIndex {
    std::vector<Name> names(ArgumentType) const override { return {{"Alpha", "Alpha", 1}, {"Alpine", "Alpine", 2}, {"Zalpha", "Zalpha", 3}}; }
    std::vector<std::uint64_t> selection(ArgumentType) const override { return {1, 2}; }
};
}
int main() {
    const auto tokens = tokenize("help \"two words\" ");
    check(tokens.error.empty() && tokens.values.size() == 2 && tokens.values[1] == "two words", "tokenizer");
    check(!tokenize("\"bad").error.empty(), "unterminated quote");
    check(parse_integer("-42") == -42 && parse_integer("+1") == 1 && !parse_integer("4x"), "integer");
    check(parse_number("-1.5") == -25165824 && !parse_number("1.2x"), "fixed number");
    check(parse_number("0.1") == 1677722 && parse_number("-0.1") == -1677722, "fixed rounding");
    check(parse_number(".000000059604644775390625") == 1 && parse_number("+1.") == 16777216, "precise fixed decimal");
    check(parse_number(".0000000298023223876953125") == 0 &&
          parse_number(".0000000894069671630859375") == 2, "fixed half ties to even");
    check(!parse_number("549755813888") && parse_number("-549755813888") == std::numeric_limits<std::int64_t>::min(), "fixed limits");
    check(parse_boolean("ON") == true && parse_boolean("0") == false && !parse_boolean("maybe"), "boolean");
    Index index;
    Registry registry;
    for (const auto& command : core_commands()) check(registry.add(command), "register core command");
    check(registry.find("?") && !registry.add(core_commands().front()), "registry aliases and duplicates");
    Command missing{"missing", {}, {}, "help", 15, CommandKind::simulation, {}};
    check(!registry.add(missing), "registry requires verb");
    for (auto kind : {CommandKind::view, CommandKind::simulation, CommandKind::single_player}) {
        missing.kind = kind;
        check(!registry.add(missing), "registry requires verb for non-local command");
    }
    missing.verb = 100;
    check(registry.add(missing), "registry accepts verb");
    missing.name = "other";
    check(!registry.add(missing), "registry rejects duplicate verb");
    for (auto type : {ArgumentType::faction, ArgumentType::planet, ArgumentType::type, ArgumentType::fleet,
                      ArgumentType::unit, ArgumentType::event}) {
        Command command{"x", {}, {{"name", type, false, ""}}, "help", 15, CommandKind::local, {}};
        const auto parsed = parse_arguments(command, {"Alpha"}, index);
        check(parsed.error.empty() && parsed.values[0].id == 1, "name argument");
    }
    for (auto type : {ArgumentType::fleet, ArgumentType::unit}) {
        Command command{"x", {}, {{"entity", type, false, ""}}, "help", 15, CommandKind::local, {}};
        check(parse_arguments(command, {"1"}, index).values[0].id == 1, "visible numeric entity");
        check(!parse_arguments(command, {"99"}, index).error.empty(), "hidden numeric entity");
        check(parse_arguments(command, {"sel"}, index).values[0].ids == std::vector<std::uint64_t>({1, 2}), "selected entities");
    }
    Command point{"x", {}, {{"point", ArgumentType::point, false, ""}}, "help", 15, CommandKind::local, {}};
    const auto parsed_point = parse_arguments(point, {"1.5", "-2"}, index);
    check(parsed_point.error.empty() && parsed_point.values[0].number == 25165824 &&
          parsed_point.values[0].second_number == -33554432, "point argument");
    check(parse_arguments(point, {"cursor"}, index).error.empty() &&
          parse_arguments(point, {"camera"}, index).error.empty() &&
          !parse_arguments(point, {"1"}, index).error.empty(), "point keywords and incomplete pair");
    for (auto type : {ArgumentType::file, ArgumentType::text}) {
        Command command{"x", {}, {{"value", type, false, ""}}, "help", 15, CommandKind::local, {}};
        check(parse_arguments(command, {"synthetic name"}, index).values[0].text == "synthetic name", "text argument");
    }
    for (auto type : {ArgumentType::integer, ArgumentType::number, ArgumentType::boolean}) {
        Command command{"x", {}, {{"value", type, false, ""}}, "help", 15, CommandKind::local, {}};
        check(parse_arguments(command, {"1"}, index).error.empty(), "numeric argument");
        check(!parse_arguments(command, {"invalid"}, index).error.empty(), "invalid numeric argument");
    }
    auto matches = complete("alp", index.names(ArgumentType::type));
    check(matches.size() == 3 && matches[0].name == "Alpha" && matches[2].name == "Zalpha", "completion order");
    check(resolve("alpha", index.names(ArgumentType::type)).value->id == 1, "exact resolution");
    check(!resolve("al", index.names(ArgumentType::type)).value, "ambiguous resolution");
    const auto typo = resolve("Alpga", index.names(ArgumentType::type));
    check(!typo.value && !typo.suggestions.empty() && typo.suggestions[0].name == "Alpha", "closest suggestion");
    Command named{"x", {}, {{"name", ArgumentType::type, false, ""}}, "help", 15, CommandKind::local, {}};
    const auto bad_name = parse_arguments(named, {"Alpga"}, index);
    check(bad_name.error == "TEXT_EAWR_CONSOLE_ERROR_ARGUMENT" && !bad_name.suggestions.empty() &&
          bad_name.suggestions[0].name == "Alpha", "localized argument error and suggestion");
    for (auto kind : {CommandKind::local, CommandKind::view, CommandKind::simulation, CommandKind::single_player}) {
        for (auto context : {Context::menu, Context::skirmish, Context::galaxy, Context::gc_battle}) {
            Command command{"x", {}, {}, "help", 15, kind, 1};
            check(permitted(command, context, false, false, 1), "single player permission");
            check(!permitted(command, context, true, false, 2), "multiplayer blocked");
            check(permitted(command, context, true, true, 2) == (kind != CommandKind::single_player), "multiplayer enabled");
            check(permitted(command, context, true, true, 1) == (kind != CommandKind::single_player), "single-human multiplayer");
            command.contexts = 0;
            check(!permitted(command, context, false, true, 1), "context gate");
        }
    }
    eawr::data::ui::TextDatabase database;
    eawr::data::ui::TextLookup lookup(database);
    for (const auto& command : core_commands()) {
        check(!command.help_key.empty() && lookup.contains(command.help_key), "missing help");
        check(command.kind == CommandKind::local || command.verb.has_value(), "missing verb");
    }
    check(lookup.contains("TEXT_EAWR_CONSOLE_NO_ACTIVE_GAME"), "missing no-game message");
    History history(2); history.add("one"); history.add("two"); history.add("three");
    check(history.previous() == "three" && history.previous() == "two" && !history.previous(), "history");
    check(history.next() == "three" && history.next() == "" && !history.next(), "history forward");
    Output output(1); output.append(Level::info, "x"); output.append(Level::info, "x"); output.append(Level::error, "y");
    check(output.dropped() == 2 && output.counts()[1] == 2 && output.lines().front().text == "y", "output ring");
    output.clear(); check(output.lines().empty() && output.dropped() == 0 && output.counts()[1] == 0, "clear output");
    return 0;
}
