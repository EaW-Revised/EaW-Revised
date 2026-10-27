#pragma once
// All viewer modes consume the same expansion; profile IO and discovery run once per process.
#include "eawr/mods/profile.hpp"
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace eawr::presentation::godot_backend {
inline const mods::ProfileArguments& viewer_profile_options() {
    static const auto expanded = [] {
        std::vector<std::string> args;
        const auto raw = godot::OS::get_singleton()->get_cmdline_user_args();
        for (int64_t i = 0; i < raw.size(); ++i) args.emplace_back(raw[i].utf8().get_data());
        return mods::expand_profile_arguments(args);
    }();
    return expanded;
}
inline godot::PackedStringArray viewer_profile_arguments() {
    godot::PackedStringArray args;
    for (const auto& arg : viewer_profile_options().arguments) args.push_back(godot::String::utf8(arg.c_str()));
    return args;
}
} // namespace eawr::presentation::godot_backend
