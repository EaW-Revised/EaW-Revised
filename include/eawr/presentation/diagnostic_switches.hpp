#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::diagnostic_switches {

// PF ablation ladder: measurement hooks that switch one presentation cost off, process-local and off by default
// (never a shipped setting, never saved). `--eawr-diagnostic-off a,b,c` names them; every active one prints
// "EAWR diagnostic: <name> active" at startup. They only skip presentation work; the simulation is untouched.
//   particles  no particle system is stepped, culled, built or drawn through any registry call, batched or scalar
//              (spawns still happen)
//   turrets    hardpoint/turret animation poses are not applied (turrets keep their last pose)
//   skeletons  no clip or idle skin pose is built or uploaded (pieces keep their last pose; hull transforms still
//              move). The clip and idle sampling and the held/hidden bookkeeping still run, so death clones still hide
//              and retire, and the additive-effect clock still advances
//   shadows    no instance casts a shadow (the lights stay; --eawr-shadows only reaches the map and replay paths)
// The flag may repeat; the switches of every occurrence are switched on together (see merge).
struct Switches final {
    bool particles{};
    bool turrets{};
    bool skeletons{};
    bool shadows{};
    friend bool operator==(const Switches&, const Switches&) = default;
};

inline constexpr std::string_view flag = "--eawr-diagnostic-off";

// The process-wide state. Written once at startup, before the first frame; read by the hooks.
inline Switches active{};

// Parses a comma list. Empty entries are ignored; an unknown name sets `error` and leaves `out` untouched.
[[nodiscard]] inline bool parse(std::string_view list, Switches& out, std::string& error) {
    Switches parsed{};
    while (!list.empty()) {
        const auto comma = list.find(',');
        const auto name = list.substr(0, comma);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        if (name.empty()) continue;
        if (name == "particles") parsed.particles = true;
        else if (name == "turrets") parsed.turrets = true;
        else if (name == "skeletons") parsed.skeletons = true;
        else if (name == "shadows") parsed.shadows = true;
        else {
            error = std::string(flag) + ": unknown switch '" + std::string(name) + "' (particles, turrets, skeletons, shadows)";
            return false;
        }
    }
    out = parsed;
    return true;
}

// Switches on every switch `from` has on (a repeated flag adds to the earlier ones).
inline void merge(Switches& into, const Switches& from) noexcept {
    into.particles = into.particles || from.particles;
    into.turrets = into.turrets || from.turrets;
    into.skeletons = into.skeletons || from.skeletons;
    into.shadows = into.shadows || from.shadows;
}

// The startup lines for the active switches, in a fixed order.
[[nodiscard]] inline std::vector<std::string> announce(const Switches& switches) {
    std::vector<std::string> lines;
    if (switches.particles) lines.emplace_back("EAWR diagnostic: particles active");
    if (switches.turrets) lines.emplace_back("EAWR diagnostic: turrets active");
    if (switches.skeletons) lines.emplace_back("EAWR diagnostic: skeletons active");
    if (switches.shadows) lines.emplace_back("EAWR diagnostic: shadows active");
    return lines;
}

} // namespace eawr::presentation::diagnostic_switches
