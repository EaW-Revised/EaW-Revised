#include "cli_console.hpp"
#include "eawr/presentation/console/core.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

namespace sim_headless::cli {
namespace {
std::string json_string(const std::string& text) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}
struct EmptyIndex final : eawr::presentation::console::NameIndex {
    std::vector<eawr::presentation::console::Name> names(eawr::presentation::console::ArgumentType) const override { return {}; }
};
}
int run_console(int argc, const char* const argv[]) {
    using namespace eawr::presentation::console;
    std::string script, output; std::uint64_t ticks = 0; bool ticks_seen = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) { std::cerr << "console: missing option value\n"; return 1; }
        const std::string value = argv[++i];
        if (arg == "--script" && script.empty()) script = value;
        else if (arg == "--out" && output.empty()) output = value;
        else if (arg == "--ticks" && !ticks_seen) {
            ticks_seen = true;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), ticks);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) { std::cerr << "console: invalid ticks\n"; return 1; }
        } else { std::cerr << "console: unknown or repeated option\n"; return 1; }
    }
    if (script.empty() || output.empty()) { std::cerr << "console: --script and distinct --out required\n"; return 1; }
    std::error_code script_error, output_error;
    const auto script_path = std::filesystem::weakly_canonical(script, script_error);
    const auto output_path = std::filesystem::weakly_canonical(output, output_error);
    std::error_code equivalent_error;
    const bool equivalent = std::filesystem::equivalent(script, output, equivalent_error);
    if (script_error || output_error || script_path == output_path || (!equivalent_error && equivalent)) {
        std::cerr << "console: --script and distinct --out required\n"; return 1;
    }
    std::ifstream input(script); if (!input) { std::cerr << "console: cannot read script\n"; return 1; }
    std::ofstream out(output, std::ios::binary); if (!out) { std::cerr << "console: cannot write transcript\n"; return 1; }
    EmptyIndex index; History history; Output log; std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tokens = tokenize(line); std::string key; std::string result;
        if (!tokens.error.empty()) key = "TEXT_EAWR_CONSOLE_ERROR_SYNTAX";
        else if (tokens.values.empty()) continue;
        else if (const auto* command = find_command(core_commands(), tokens.values.front())) {
            const std::vector<std::string> args(tokens.values.begin() + 1, tokens.values.end());
            const auto parsed = parse_arguments(*command, args, index);
            if (!permitted(*command, Context::menu, false, false, 1)) key = "TEXT_EAWR_CONSOLE_ERROR_CONTEXT";
            else if (!parsed.error.empty()) {
                key = parsed.error;
                for (const auto& suggestion : parsed.suggestions) {
                    if (!result.empty()) result += ' ';
                    result += suggestion.name;
                }
            }
            else {
                key = command->help_key;
                if (command->name == "help") {
                    if (args.empty()) {
                        for (const auto& item : core_commands()) {
                            if (!permitted(item, Context::menu, false, false, 1)) continue;
                            if (!result.empty()) result += ' ';
                            result += item.name;
                        }
                    } else if (const auto* item = find_command(core_commands(), args[0]); item && permitted(*item, Context::menu, false, false, 1)) result = item->help_key;
                    else key = "TEXT_EAWR_CONSOLE_ERROR_UNKNOWN";
                } else if (command->name == "list" && args[0] == "commands") {
                    for (const auto& item : core_commands()) {
                        if (!permitted(item, Context::menu, false, false, 1)) continue;
                        if (args.size() > 1 && item.name.find(args[1]) != 0) continue;
                        if (!result.empty()) result += ' ';
                        result += item.name;
                    }
                } else if (command->name == "list") key = "TEXT_EAWR_CONSOLE_ERROR_ARGUMENT";
                else if (command->name == "clear") log.clear();
                else if (command->name == "history") {
                    auto count = parsed.values.front().number.value_or(20);
                    if (count < 0) count = 0;
                    const auto& lines = history.lines();
                    const auto skip = static_cast<std::uint64_t>(count) >= lines.size() ? 0 : lines.size() - static_cast<std::size_t>(count);
                    for (std::size_t n = skip; n < lines.size(); ++n) { if (!result.empty()) result += '\n'; result += lines[n]; }
                } else if (command->name == "cheats") key = "TEXT_EAWR_CONSOLE_NO_ACTIVE_GAME";
                else if (command->name == "version") result = EAWR_CONSOLE_VERSION;
            }
        } else key = "TEXT_EAWR_CONSOLE_ERROR_UNKNOWN";
        history.add(line);
        log.append(Level::info, result.empty() ? key : result);
        out << "{\"input\":" << json_string(line) << ",\"output_keys\":[" << json_string(key)
            << "],\"output\":" << json_string(result) << ",\"envelope\":\"\",\"tick\":null,\"state_hash\":null,\"requested_ticks\":" << ticks << "}\n";
    }
    out.flush();
    return input.bad() || !out ? 1 : 0;
}
} // namespace sim_headless::cli
