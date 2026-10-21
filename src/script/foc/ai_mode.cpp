#include "ai_mode.hpp"

#include <array>

namespace eawr::script::foc::ai {
namespace {

// Space keeps today's fixed list (the closure hash must not move). Land needs the shared
// files and its own equations; its goal sets and goals come from directory discovery.
constexpr std::array<std::string_view, 17> space_xml{
    "data/xml/ai/players/basicempireplayer.xml",
    "data/xml/ai/players/basicrebelplayer.xml",
    "data/xml/ai/players/ai_player_underworld.xml",
    "data/xml/ai/templates/basicgenerictemplates.xml",
    "data/xml/ai/goalfunctions/basicoffensivespaceset.xml",
    "data/xml/ai/goalfunctions/ai_goalset_underworld_space.xml",
    "data/xml/ai/goalfunctions/systemfunctions.xml",
    "data/xml/ai/goals/offensivespacegoals.xml",
    "data/xml/ai/goals/ai_goals_underworld_space.xml",
    "data/xml/ai/perceptualequations/offensivespaceequations.xml",
    "data/xml/ai/perceptualequations/ai_equations_underworld_space.xml",
    "data/xml/ai/perceptualequations/budgetingequations.xml",
    "data/xml/ai/perceptualequations/basicgalacticequations.xml",
    "data/xml/ai/perceptualequations/basiclandequations.xml",
    "data/xml/ai/perceptualequations/ai_equations_expansiongeneric_landskirmish.xml",
    "data/xml/difficultyadjustments.xml",
    "data/xml/gameconstants.xml",
};

constexpr std::array<std::string_view, 11> land_xml{
    "data/xml/ai/players/basicempireplayer.xml",
    "data/xml/ai/players/basicrebelplayer.xml",
    "data/xml/ai/players/ai_player_underworld.xml",
    "data/xml/ai/templates/basicgenerictemplates.xml",
    "data/xml/ai/goalfunctions/systemfunctions.xml",
    "data/xml/ai/perceptualequations/budgetingequations.xml",
    "data/xml/ai/perceptualequations/basicgalacticequations.xml",
    "data/xml/ai/perceptualequations/basiclandequations.xml",
    "data/xml/ai/perceptualequations/ai_equations_expansiongeneric_landskirmish.xml",
    "data/xml/difficultyadjustments.xml",
    "data/xml/gameconstants.xml",
};

constexpr AiMode space_mode{"SPACE", "SPACEMODE", "Data/Scripts/AI/SpaceMode/", "Space", "Space", "", space_xml};
constexpr AiMode land_mode{"LAND", "LANDMODE", "Data/Scripts/AI/LandMode/", "Land", "Land",
    "eawr-tactical-closure-v1-land", land_xml};

} // namespace

const AiMode& ai_mode(const sim::tactical::BattleDomain domain) noexcept {
    return domain == sim::tactical::BattleDomain::land ? land_mode : space_mode;
}

} // namespace eawr::script::foc::ai
