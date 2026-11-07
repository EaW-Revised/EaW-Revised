#include "launch_plan.hpp"

#include <algorithm>

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

bool evidence_requested(const std::vector<std::wstring>& extra) {
    return std::any_of(extra.begin(), extra.end(), [](const std::wstring& arg) {
        return arg == L"--eawr-capture" || arg == L"--eawr-report" || arg == L"--eawr-benchmark"
            || arg == L"--eawr-headless-probe" || arg == L"--eawr-renderer-runtime-test"
            || arg == L"--eawr-camera-input-selftest" || arg == L"--eawr-window-resize-test"
            || arg == L"--eawr-perf-trace";
    });
}

std::vector<std::wstring> godot_arguments(const LaunchInputs& inputs) {
    std::vector<std::wstring> args = {L"--path", inputs.project.wstring()};
    if (inputs.safe_render_thread.has_value()) args.insert(args.end(), {L"--render-thread", *inputs.safe_render_thread ? L"safe" : L"separate"});
    else if (inputs.render_thread) args.insert(args.end(), {L"--render-thread", *inputs.render_thread});
    if (inputs.quit_after) {
        args.push_back(L"--quit-after");
        args.push_back(std::to_wstring(*inputs.quit_after));
    }
    args.insert(args.end(), {L"--", L"--eawr-game-root", inputs.game_root.wstring(), L"--eawr-front-end"});
    if (!evidence_requested(inputs.extra)) args.push_back(L"--eawr-launcher-start");
    if (inputs.allow_unknown_build) args.push_back(L"--eawr-allow-unknown-build");
    if (inputs.engine_unverified) args.push_back(L"--eawr-engine-unverified");
    args.insert(args.end(), {L"--eawr-report", inputs.report.wstring()});
    if (inputs.depth_prepass_off) args.insert(args.end(), {L"--eawr-depth-prepass", L"off"});
    if (inputs.mod_profile) args.insert(args.end(), {L"--eawr-mod-profile", inputs.mod_profile->wstring()});
    if (inputs.save_dir) args.insert(args.end(), {L"--eawr-save-dir", inputs.save_dir->wstring()});
    if (inputs.font_cache) args.insert(args.end(), {L"--eawr-font-cache", inputs.font_cache->wstring()});
    if (inputs.lan_game_port) args.insert(args.end(), {L"--eawr-lan-game-port", std::to_wstring(*inputs.lan_game_port)});
    args.insert(args.end(), inputs.extra.begin(), inputs.extra.end());
    return args;
}

std::vector<std::wstring> mods_arguments(const fs::path& kit, const fs::path& godot,
                                         const fs::path& project, const fs::path& game_root, const fs::path& next_action) {
    return {L"--path", (kit / "launcher").wstring(), L"--", L"--eawr-mods-service",
            (kit / "eawr_mods_service.exe").wstring(), L"--eawr-godot", godot.wstring(),
            L"--eawr-game-project", project.wstring(), L"--eawr-game-root", game_root.wstring(),
            L"--eawr-launch-loop", next_action.wstring(),
            L"--eawr-launch-loop-stay-open", (next_action.parent_path() / "game-running.json").wstring()};
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
