#pragma once

#include "eawr/assets/map.hpp"
#include "eawr/data/xml.hpp"

#include <string>

// What an object type declares about its first idle clip (Idle_Anim_00), for
// presentation/animation/idle_playback.hpp. The retail game starts that clip
// on every placed object when its model is created, on land and in space
// alike; only a space DUMMY_STARSHIP skips the random start frame (#145,
// #157). Engine-free.
namespace eawr::scene {

struct IdleTags final {
    bool loop{};             // Loop_Idle_Anim_00
    std::string rate_mod{};  // Idle_Anim_00_Rate_Mod text, trimmed; empty when absent
    // IDLE is named by Behavior or by the map kind's own list: LandBehavior
    // on a land map, SpaceBehavior on a space map.
    bool idle_behavior{};
    // Space only: DUMMY_STARSHIP is named by Behavior or SpaceBehavior. It is
    // never set on a land map, where retail does not check it.
    bool dummy_starship{};

    friend bool operator==(const IdleTags&, const IdleTags&) = default;
};

// XML booleans are accepted as yes/true/1 in any case, with surrounding
// whitespace. A behaviour list is comma or whitespace separated and matched
// entry by entry in any case.
[[nodiscard]] IdleTags idle_tags(const data::EffectiveObject& object, assets::MapKind kind);

} // namespace eawr::scene
