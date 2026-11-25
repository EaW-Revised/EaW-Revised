#include "eawr/presentation/console/core.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <utility>

namespace eawr::presentation::console {
namespace {
std::string lower(std::string_view s) {
    std::string out;
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}
}
Tokens tokenize(std::string_view line) {
    Tokens result; std::string word; bool quoted = false; bool active = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; active = true; }
        else if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (active) { result.values.push_back(word); word.clear(); active = false; }
        } else { word.push_back(c); active = true; }
    }
    if (quoted) { result.error = "unterminated quote"; result.values.clear(); }
    else if (active) result.values.push_back(word);
    return result;
}
const std::vector<Command>& core_commands() {
    constexpr unsigned all = 15;
    static const std::vector<Command> commands{
        {"help", {"?"}, {{"command", ArgumentType::text, true, ""}}, "TEXT_EAWR_CONSOLE_HELP", all, CommandKind::local, {}},
        {"list", {"ls"}, {{"kind", ArgumentType::text, false, ""}, {"prefix", ArgumentType::text, true, ""}}, "TEXT_EAWR_CONSOLE_LIST", all, CommandKind::local, {}},
        {"clear", {"cls"}, {}, "TEXT_EAWR_CONSOLE_CLEAR", all, CommandKind::local, {}},
        {"history", {}, {{"n", ArgumentType::integer, true, "20"}}, "TEXT_EAWR_CONSOLE_HISTORY", all, CommandKind::local, {}},
        {"fps", {"perf"}, {{"enabled", ArgumentType::boolean, true, ""}}, "TEXT_EAWR_CONSOLE_FPS", all, CommandKind::local, {}},
        {"version", {"ver"}, {}, "TEXT_EAWR_CONSOLE_VERSION", all, CommandKind::local, {}},
        {"cheats", {}, {}, "TEXT_EAWR_CONSOLE_CHEATS", all, CommandKind::local, {}}
    };
    return commands;
}
const Command* find_command(const std::vector<Command>& commands, std::string_view name) {
    const auto key = lower(name);
    for (const auto& command : commands) {
        if (lower(command.name) == key) return &command;
        for (const auto& alias : command.aliases) if (lower(alias) == key) return &command;
    }
    return nullptr;
}
bool Registry::add(Command command) {
    if (command.name.empty() || command.help_key.empty() || command.contexts == 0 ||
        (command.kind != CommandKind::local && (!command.verb || *command.verb == 0))) return false;
    std::vector<std::string> keys{lower(command.name)};
    for (const auto& alias : command.aliases) keys.push_back(lower(alias));
    std::sort(keys.begin(), keys.end());
    if (keys.front().empty() || std::adjacent_find(keys.begin(), keys.end()) != keys.end()) return false;
    for (const auto& key : keys) if (find(key)) return false;
    for (const auto& entry : commands_) if (command.verb && entry.verb == command.verb) return false;
    commands_.push_back(std::move(command));
    return true;
}
const Command* Registry::find(std::string_view name) const { return find_command(commands_, name); }
bool permitted(const Command& command, Context context, bool multiplayer, bool host_cheats, unsigned humans) {
    if ((command.contexts & static_cast<unsigned>(context)) == 0) return false;
    // Owner decision D3: blocked in multiplayer unless host_cheats is enabled (DC-7 #8374, default off).
    if (multiplayer && !host_cheats) return false;
    return command.kind != CommandKind::single_player || (!multiplayer && humans <= 1);
}
std::vector<Name> complete(std::string_view input, const std::vector<Name>& names, std::size_t limit) {
    const auto key = lower(input);
    std::vector<std::pair<int, Name>> matches;
    for (const auto& name : names) {
        const auto candidate = lower(name.name); const auto at = candidate.find(key);
        if (at != std::string::npos) matches.emplace_back(candidate == key ? 0 : at == 0 ? 1 : 2, name);
    }
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        const auto left = lower(a.second.name); const auto right = lower(b.second.name);
        return left != right ? left < right : a.second.id < b.second.id;
    });
    std::vector<Name> result;
    for (const auto& match : matches) { if (result.size() == limit) break; result.push_back(match.second); }
    return result;
}
Resolution resolve(std::string_view input, const std::vector<Name>& names) {
    auto suggestions = complete(input, names);
    if (suggestions.size() == 1 || (!suggestions.empty() && lower(suggestions.front().name) == lower(input) &&
        lower(suggestions[1].name) != lower(input))) return {suggestions.front(), {}};
    if (suggestions.empty() && !input.empty()) {
        std::vector<std::pair<std::size_t, Name>> distances;
        const auto query = lower(input);
        for (const auto& name : names) {
            const auto candidate = lower(name.name);
            std::vector<std::size_t> row(candidate.size() + 1);
            for (std::size_t i = 0; i < row.size(); ++i) row[i] = i;
            for (std::size_t i = 0; i < query.size(); ++i) {
                auto previous = row[0]; row[0] = i + 1;
                for (std::size_t j = 0; j < candidate.size(); ++j) {
                    const auto old = row[j + 1];
                    row[j + 1] = std::min({row[j + 1] + 1, row[j] + 1, previous + (query[i] == candidate[j] ? 0U : 1U)});
                    previous = old;
                }
            }
            distances.emplace_back(row.back(), name);
        }
        std::sort(distances.begin(), distances.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first < b.first;
            const auto left = lower(a.second.name); const auto right = lower(b.second.name);
            return left != right ? left < right : a.second.id < b.second.id;
        });
        for (const auto& entry : distances) { if (suggestions.size() == 8) break; suggestions.push_back(entry.second); }
    }
    return {{}, std::move(suggestions)};
}
std::optional<std::int64_t> parse_integer(std::string_view text) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '-' || text.front() == '+') return {};
    }
    std::int64_t value{}; const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()) return {};
    return value;
}
std::optional<std::int64_t> parse_number(std::string_view text) {
    if (text.empty() || text.size() > 4096) return {};
    const bool negative = text.front() == '-';
    if (negative || text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return {};
    const auto dot = text.find('.'); const auto whole = text.substr(0, dot);
    const auto digits = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() && digits.empty()) return {};
    std::uint64_t integer{};
    if (!whole.empty()) {
        const auto parsed = std::from_chars(whole.data(), whole.data() + whole.size(), integer);
        if (parsed.ec != std::errc{} || parsed.ptr != whole.data() + whole.size()) return {};
    }
    constexpr std::uint64_t scale = 1U << 24U;
    if (integer > (std::uint64_t{1} << 39U)) return {};
    std::vector<unsigned> fraction;
    fraction.reserve(digits.size());
    for (char digit : digits) {
        if (digit < '0' || digit > '9') return {};
        fraction.push_back(static_cast<unsigned>(digit - '0'));
    }
    std::uint64_t fraction_bits = 0;
    for (int bit = 0; bit < 24; ++bit) {
        unsigned carry = 0;
        for (auto it = fraction.rbegin(); it != fraction.rend(); ++it) {
            const auto doubled = *it * 2U + carry; *it = doubled % 10U; carry = doubled / 10U;
        }
        fraction_bits = fraction_bits * 2U + carry;
    }
    auto magnitude = integer * scale + fraction_bits;
    if (!fraction.empty()) {
        const bool above_half = fraction.front() > 5U || (fraction.front() == 5U &&
            std::any_of(fraction.begin() + 1, fraction.end(), [](unsigned digit) { return digit != 0; }));
        const bool tie = fraction.front() == 5U &&
            std::all_of(fraction.begin() + 1, fraction.end(), [](unsigned digit) { return digit == 0; });
        if (above_half || (tie && (magnitude & 1U) != 0)) ++magnitude;
    }
    const auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (magnitude > max + static_cast<unsigned>(negative)) return {};
    if (negative && magnitude == max + 1) return std::numeric_limits<std::int64_t>::min();
    const auto value = static_cast<std::int64_t>(magnitude); return negative ? -value : value;
}
std::optional<bool> parse_boolean(std::string_view text) {
    const auto key = lower(text); if (key == "on" || key == "true" || key == "1") return true;
    if (key == "off" || key == "false" || key == "0") return false;
    return {};
}
ParseResult parse_arguments(const Command& command, const std::vector<std::string>& tokens, const NameIndex& index) {
    ParseResult result;
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < command.arguments.size(); ++i) {
        const auto& spec = command.arguments[i];
        if (cursor >= tokens.size() && !spec.optional) { result.error = "TEXT_EAWR_CONSOLE_ERROR_ARGUMENT"; return result; }
        const std::string value = cursor < tokens.size() ? tokens[cursor++] : spec.default_value;
        ParsedArgument parsed{spec.type, value, {}, {}, {}, {}, {}};
        if (value.empty() && spec.optional) { result.values.push_back(std::move(parsed)); continue; }
        switch (spec.type) {
        case ArgumentType::integer: parsed.number = parse_integer(value); if (!parsed.number) result.error = "TEXT_EAWR_CONSOLE_ERROR_ARGUMENT"; break;
        case ArgumentType::number: parsed.number = parse_number(value); if (!parsed.number) result.error = "TEXT_EAWR_CONSOLE_ERROR_ARGUMENT"; break;
        case ArgumentType::boolean: parsed.boolean = parse_boolean(value); if (!parsed.boolean) result.error = "TEXT_EAWR_CONSOLE_ERROR_ARGUMENT"; break;
        case ArgumentType::faction: case ArgumentType::planet: case ArgumentType::type:
        case ArgumentType::fleet: case ArgumentType::unit: case ArgumentType::event: {
            const auto names = index.names(spec.type);
            if (spec.type == ArgumentType::fleet || spec.type == ArgumentType::unit) {
                if (lower(value) == "sel") {
                    parsed.ids = index.selection(spec.type);
                    if (parsed.ids.empty() || std::any_of(parsed.ids.begin(), parsed.ids.end(), [&names](auto id) {
                        return std::none_of(names.begin(), names.end(), [id](const auto& name) { return name.id == id; });
                    })) result.error = "TEXT_EAWR_CONSOLE_ERROR_ARGUMENT";
                    break;
                }
                std::uint64_t numeric{};
                const auto converted = std::from_chars(value.data(), value.data() + value.size(), numeric);
                if (converted.ec == std::errc{} && converted.ptr == value.data() + value.size()) {
                    const auto found = std::find_if(names.begin(), names.end(), [numeric](const auto& name) { return name.id == numeric; });
                    if (found != names.end()) { parsed.id = found->id; parsed.ids.push_back(found->id); break; }
                    result.error = "TEXT_EAWR_CONSOLE_ERROR_ARGUMENT";
                    break;
                }
            }
            auto resolved = resolve(value, names);
            if (resolved.value) {
                parsed.id = resolved.value->id;
                if (spec.type == ArgumentType::fleet || spec.type == ArgumentType::unit) parsed.ids.push_back(*parsed.id);
            }
            else { result.error = "TEXT_EAWR_CONSOLE_ERROR_ARGUMENT"; result.suggestions = std::move(resolved.suggestions); }
            break;
        }
        case ArgumentType::point:
            if (lower(value) != "cursor" && lower(value) != "camera") {
                parsed.number = parse_number(value);
                if (cursor < tokens.size()) parsed.second_number = parse_number(tokens[cursor++]);
                if (!parsed.number || !parsed.second_number) result.error = "TEXT_EAWR_CONSOLE_ERROR_ARGUMENT";
            }
            break;
        case ArgumentType::file: case ArgumentType::text: break;
        }
        if (!result.error.empty()) { result.values.clear(); return result; }
        result.values.push_back(std::move(parsed));
    }
    if (cursor != tokens.size()) { result.values.clear(); result.error = "TEXT_EAWR_CONSOLE_ERROR_ARGUMENT"; }
    return result;
}
void History::add(std::string line) {
    if (capacity_ == 0 || line.empty()) return;
    if (lines_.size() == capacity_) lines_.pop_front();
    lines_.push_back(std::move(line)); cursor_ = lines_.size();
}
std::optional<std::string> History::previous() { if (cursor_ == 0) return {}; return lines_[--cursor_]; }
std::optional<std::string> History::next() { if (cursor_ >= lines_.size()) return {}; ++cursor_; return cursor_ == lines_.size() ? std::optional<std::string>{""} : lines_[cursor_]; }
void Output::append(Level level, std::string text) {
    ++counts_[static_cast<std::size_t>(level)];
    if (!lines_.empty() && lines_.back().level == level && lines_.back().text == text) { ++lines_.back().repeats; return; }
    if (capacity_ == 0) { ++dropped_; return; }
    if (lines_.size() == capacity_) { dropped_ += lines_.front().repeats; lines_.pop_front(); }
    lines_.push_back({level, std::move(text), 1});
}
void Output::clear() { lines_.clear(); counts_.fill(0); dropped_ = 0; }
} // namespace eawr::presentation::console
