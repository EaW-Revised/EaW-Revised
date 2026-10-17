#include "launch_plan.hpp"

namespace eawr::launcher {

namespace {

void append_quoted(std::wstring& line, const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += argument;
        return;
    }
    line.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            line.append(backslashes * 2 + 1, L'\\');   // escape the backslashes and the quote
        } else {
            line.append(backslashes, L'\\');
        }
        backslashes = 0;
        line.push_back(c);
    }
    line.append(backslashes * 2, L'\\');   // backslashes before the closing quote
    line.push_back(L'"');
}

} // namespace

std::vector<std::wstring> godot_arguments(const LaunchInputs& inputs) {
    std::vector<std::wstring> args = {L"--path", inputs.project.wstring()};
    if (inputs.quit_after) {
        args.push_back(L"--quit-after");
        args.push_back(std::to_wstring(*inputs.quit_after));
    }
    args.insert(args.end(), {L"--", L"--eawr-game-root", inputs.game_root.wstring(), L"--eawr-front-end"});
    if (inputs.allow_unknown_build) args.push_back(L"--eawr-allow-unknown-build");
    args.insert(args.end(), {L"--eawr-report", inputs.report.wstring()});
    args.insert(args.end(), inputs.extra.begin(), inputs.extra.end());
    return args;
}

std::wstring quote_command_line(const std::wstring& program, const std::vector<std::wstring>& args) {
    std::wstring line;
    append_quoted(line, program);
    for (const std::wstring& argument : args) {
        line.push_back(L' ');
        append_quoted(line, argument);
    }
    return line;
}

} // namespace eawr::launcher
