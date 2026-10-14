#pragma once

#include "eawr/sim/galactic/story_adapters.hpp"

#include <optional>

namespace eawr::campaign {
struct GameStoryNotice {
    std::optional<sim::galactic::story::Player> player; // absence broadcasts to story recipients
    sim::galactic::story::Payload payload;
    // Observer context is separate from the native story's broadcast filter.
    sim::galactic::story::Player old_owner{}, new_owner{};
    sim::galactic::PlanetId observer_planet{}; // nonzero requires current discovery for UI delivery
};
} // namespace eawr::campaign
