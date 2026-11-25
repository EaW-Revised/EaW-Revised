#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::console {

enum class Context : unsigned { menu = 1, skirmish = 2, galaxy = 4, gc_battle = 8 };
enum class CommandKind { local, view, simulation, single_player };
enum class ArgumentType { integer, number, boolean, faction, planet, type, fleet, unit, point, event, file, text };
struct Argument { std::string name; ArgumentType type; bool optional{}; std::string default_value; };
struct Command {
    std::string name;
    std::vector<std::string> aliases;
    std::vector<Argument> arguments;
    std::string help_key;
    unsigned contexts{};
    CommandKind kind{};
    std::optional<std::uint16_t> verb;
};
class Registry {
public:
    [[nodiscard]] bool add(Command command);
    [[nodiscard]] const Command* find(std::string_view name) const;
    [[nodiscard]] const std::vector<Command>& commands() const { return commands_; }
private:
    std::vector<Command> commands_;
};

struct Tokens { std::vector<std::string> values; std::string error; };
[[nodiscard]] Tokens tokenize(std::string_view line);
[[nodiscard]] const std::vector<Command>& core_commands();
[[nodiscard]] const Command* find_command(const std::vector<Command>& commands, std::string_view name);
[[nodiscard]] bool permitted(const Command& command, Context context, bool multiplayer, bool host_cheats, unsigned humans);

struct Name { std::string name; std::string display; std::uint64_t id{}; };
class NameIndex {
public:
    virtual ~NameIndex() = default;
    // Include context-specific keywords such as me, here, and player slots as names.
    // Fleet and unit entries must contain only entities visible to this player.
    [[nodiscard]] virtual std::vector<Name> names(ArgumentType type) const = 0;
    [[nodiscard]] virtual std::vector<std::uint64_t> selection(ArgumentType) const { return {}; }
};
[[nodiscard]] std::vector<Name> complete(std::string_view input, const std::vector<Name>& names, std::size_t limit = 8);
struct Resolution { std::optional<Name> value; std::vector<Name> suggestions; };
[[nodiscard]] Resolution resolve(std::string_view input, const std::vector<Name>& names);
[[nodiscard]] std::optional<std::int64_t> parse_integer(std::string_view text);
// Fixed-point values use 24 fractional bits and round to nearest, ties to even.
[[nodiscard]] std::optional<std::int64_t> parse_number(std::string_view text);
[[nodiscard]] std::optional<bool> parse_boolean(std::string_view text);
struct ParsedArgument {
    ArgumentType type{};
    std::string text;
    std::optional<std::int64_t> number;
    std::optional<std::int64_t> second_number;
    std::optional<bool> boolean;
    std::optional<std::uint64_t> id;
    std::vector<std::uint64_t> ids;
};
struct ParseResult { std::vector<ParsedArgument> values; std::string error; std::vector<Name> suggestions; };
[[nodiscard]] ParseResult parse_arguments(const Command& command, const std::vector<std::string>& tokens,
                                          const NameIndex& index);

class History {
public:
    explicit History(std::size_t capacity = 200) : capacity_(capacity) {}
    void add(std::string line);
    [[nodiscard]] const std::deque<std::string>& lines() const { return lines_; }
    [[nodiscard]] std::optional<std::string> previous();
    [[nodiscard]] std::optional<std::string> next();
private:
    std::size_t capacity_; std::size_t cursor_{}; std::deque<std::string> lines_;
};
enum class Level : std::size_t { log, info, warning, error, debug, count };
struct OutputLine { Level level; std::string text; std::size_t repeats{1}; };
class Output {
public:
    explicit Output(std::size_t capacity = 500) : capacity_(capacity) {}
    void append(Level level, std::string text);
    void clear();
    [[nodiscard]] const std::deque<OutputLine>& lines() const { return lines_; }
    [[nodiscard]] const std::array<std::size_t, static_cast<std::size_t>(Level::count)>& counts() const { return counts_; }
    [[nodiscard]] std::size_t dropped() const { return dropped_; }
private:
    std::size_t capacity_; std::size_t dropped_{}; std::deque<OutputLine> lines_;
    std::array<std::size_t, static_cast<std::size_t>(Level::count)> counts_{};
};
} // namespace eawr::presentation::console
