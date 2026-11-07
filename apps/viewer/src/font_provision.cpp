#include "font_provision.hpp"

#include "viewer_path.hpp"

#include "eawr/presentation/ui/font_extract.hpp"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <system_error>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace ui = presentation::ui;

[[nodiscard]] std::string to_utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
}

} // namespace

std::string provision_fonts(const std::filesystem::path& game_root, const bool allow_unknown_build,
                            const bool required) {
    OS* const os = OS::get_singleton();
    const PackedStringArray arguments = os->get_cmdline_user_args();
    std::filesystem::path named_cache;
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] != String("--eawr-font-cache")) continue;
        if (index + 1 < arguments.size()) named_cache = ViewerPath{to_utf8(arguments[index + 1])}.native();
        break;
    }
    // A named cache that already holds fonts is used as it is. Real play (a second instance sharing the first
    // instance's cache before that one ever ran) fills an empty one; a failure then only costs the fallback font.
    std::error_code probe;
    const bool fill_named = required && !named_cache.empty() && !game_root.empty()
        && !std::filesystem::exists(named_cache / "fonts.json", probe);
    if (!named_cache.empty() && !fill_named) return {};
    if (named_cache.empty() && !os->get_environment("EAWR_FONT_CACHE").is_empty()) return {};

    const std::filesystem::path cache = fill_named ? named_cache
                                                   : ViewerPath{to_utf8(os->get_user_data_dir())}.native() / "fonts";
    const auto use_cache = [&] {
        os->set_environment("EAWR_FONT_CACHE", String::utf8(ViewerPath::utf8(cache).c_str()));
    };
    if (!game_root.empty()) {
        const ui::FontExtractResult result = ui::ensure_font_cache(game_root, cache, allow_unknown_build);
        for (const std::string& warning : result.warnings) {
            UtilityFunctions::printerr(String("EAWR fonts: ") + String::utf8(warning.c_str()));
        }
        if (result.status != ui::FontExtractStatus::failed) {
            UtilityFunctions::print(String(result.status == ui::FontExtractStatus::cached
                                               ? "EAWR fonts: valid fonts already in "
                                               : "EAWR fonts: extracted from your Forces of Corruption to ")
                                    + String::utf8(ViewerPath::utf8(cache).c_str()));
            use_cache();
            return {};
        }
        UtilityFunctions::printerr(String("EAWR fonts: ") + String::utf8(result.message.c_str()));
        if (required && !fill_named) return result.message;
    }
    // No game root (or a run that may go on without it): fonts a previous run left in user data.
    std::error_code error;
    if (std::filesystem::exists(cache / "fonts.json", error)) {
        use_cache();
    } else if (required && game_root.empty()) {
        UtilityFunctions::printerr(String("EAWR fonts: no game root (--eawr-game-root) and no fonts in user data yet; "
                                          "the interface uses a fallback font."));
    }
    return {};
}

} // namespace eawr::presentation::godot_backend
