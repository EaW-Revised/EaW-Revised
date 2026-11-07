#pragma once

#include "eawr/presentation/godot/renderer.hpp"
#include "journal_append.hpp"
#include "viewer_path.hpp"

#include <godot_cpp/classes/os.hpp>
#include <system_error>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace eawr::presentation::godot_backend {

// Opt-in (--eawr-texture-census) load-phase texture snapshots. Each snapshot is appended to
// `<report path>.texture-census.jsonl` (else the user-data logs directory) and closed again, so a run that
// is stopped mid-load still leaves every earlier snapshot on disk (the first write of a process truncates the file). With the switch off nothing happens
// beyond one flag read. Main thread only; one walk of the live renderers per snapshot.
class TextureCensusJournal final {
public:
    static void snapshot(const char* label) {
        if (!GodotRenderer::texture_census_enabled()) return;
        const auto lines = GodotRenderer::texture_census_snapshots(label);
        if (lines.empty()) return;
        const auto unix_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string text;
        for (std::size_t index = 0; index < lines.size(); ++index) {
            text += "{\"label\":\"" + std::string(label) + "\",\"renderer\":" + std::to_string(index)
                + ",\"unix_us\":" + std::to_string(unix_us) + ",\"census\":" + lines[index] + "}\n";
        }
        // The first write of the process truncates, so a rerun with the same report path starts clean.
        static bool first_write = true;
        append_journal(path(), text, first_write);
        last() = std::chrono::steady_clock::now();
    }
    // Called from busy loading paths: takes a snapshot at most every `seconds` while loading.
    static void periodic(const char* label, const double seconds = 20.0) {
        if (!GodotRenderer::texture_census_enabled()) return;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - last()).count() < seconds) return;
        snapshot(label);
        last() = std::chrono::steady_clock::now(); // also throttles a walk that found no textures
    }

private:
    static std::chrono::steady_clock::time_point& last() {
        static auto value = std::chrono::steady_clock::now();
        return value;
    }
    static const std::filesystem::path& path() {
        static const std::filesystem::path value = [] {
            auto* os = godot::OS::get_singleton();
            std::string text;
            const auto arguments = os->get_cmdline_user_args();
            for (std::int64_t index = 0; index + 1 < arguments.size(); ++index) {
                if (godot::String(arguments[index]) == godot::String("--eawr-report")) {
                    text = std::string(godot::String(arguments[index + 1]).utf8().get_data()) + ".texture-census.jsonl";
                    break;
                }
            }
            if (text.empty()) {
                text = std::string(os->get_user_data_dir().utf8().get_data()) + "/logs/texture-census-"
                    + std::to_string(os->get_process_id()) + ".jsonl";
                std::error_code ignored;
                std::filesystem::create_directories(std::filesystem::path(text).parent_path(), ignored);
            }
            godot::UtilityFunctions::print(godot::String::utf8(("EAWR texture census journal: " + text).c_str()));
            return ViewerPath(text).native();
        }();
        return value;
    }
};

} // namespace eawr::presentation::godot_backend
