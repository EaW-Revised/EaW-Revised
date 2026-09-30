// Idle_Anim_00 tag contracts (#145 space, #157 land). The objects are shaped
// like FoC XML types but invented here; no installation is read.

#include "eawr/scene/idle_tags.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/data/xml.hpp"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using eawr::assets::MapKind;
using eawr::scene::idle_tags;

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

eawr::data::EffectiveObject object(const std::initializer_list<std::pair<std::string, std::string>> tags) {
    eawr::data::EffectiveObject result;
    for (const auto& [name, text] : tags) {
        eawr::data::EffectiveValue value;
        value.value.name = name;
        value.value.raw_text = text;
        result.values.push_back(std::move(value));
    }
    return result;
}

void land_contracts() {
    // A mineral pad: IDLE in Behavior, Loop_Idle_Anim_00 = No.
    const auto pad = idle_tags(object({{"Behavior", "IDLE, TACTICAL_BUILD_OBJECTS, REVEAL, CAPTURE_POINT"},
                                       {"LandBehavior", "TERRAIN_TEXTURE_MODIFICATION, LAND_OBSTACLE"},
                                       {"Loop_Idle_Anim_00", "No"}}),
                               MapKind::land);
    expect(pad.idle_behavior && !pad.loop && pad.rate_mod.empty() && !pad.dummy_starship,
           "land: IDLE in Behavior restarts; No does not loop");
    // A communications array: looping, no IDLE anywhere.
    const auto array = idle_tags(object({{"Behavior", "DUMMY_GROUND_STRUCTURE,SELECTABLE"},
                                         {"LandBehavior", "SPACE_OBSTACLE, LAND_OBSTACLE,REVEAL,UNIT_AI,"},
                                         {"Loop_Idle_Anim_00", " Yes "},
                                         {"Idle_Anim_00_Rate_Mod", " 0.5 "}}),
                                 MapKind::land);
    expect(array.loop && array.rate_mod == "0.5" && !array.idle_behavior, "land: loop flag and trimmed rate are read");
    expect(idle_tags(object({{"LandBehavior", "\n\t idle \n"}}), MapKind::land).idle_behavior,
           "land: LandBehavior names IDLE in any case");
    expect(!idle_tags(object({{"SpaceBehavior", "IDLE"}}), MapKind::land).idle_behavior,
           "land: SpaceBehavior is not a land list");
    expect(!idle_tags(object({{"LandBehavior", "IDLE_WANDER, UNIT_AI"}}), MapKind::land).idle_behavior,
           "land: a behaviour name matches whole list entries only");
    expect(!idle_tags(object({{"Behavior", "DUMMY_STARSHIP, IDLE"}}), MapKind::land).dummy_starship,
           "land: DUMMY_STARSHIP is never read, retail checks it in space only");
    expect(idle_tags(object({}), MapKind::land) == eawr::scene::IdleTags{}, "land: no tags, no playback facts");
}

void space_contracts() {
    expect(!idle_tags(object({{"LandBehavior", "IDLE"}}), MapKind::space).idle_behavior,
           "space: LandBehavior is not a space list");
    expect(idle_tags(object({{"SpaceBehavior", "IDLE"}}), MapKind::space).idle_behavior,
           "space: SpaceBehavior names IDLE");
    expect(idle_tags(object({{"Behavior", "dummy_starship"}}), MapKind::space).dummy_starship
               && idle_tags(object({{"SpaceBehavior", "DUMMY_STARSHIP"}}), MapKind::space).dummy_starship,
           "space: DUMMY_STARSHIP is read from Behavior and SpaceBehavior");
    for (const std::string_view yes : {"yes", "TRUE", "1", " Yes\n"}) {
        expect(idle_tags(object({{"Loop_Idle_Anim_00", std::string(yes)}}), MapKind::space).loop,
               "space: '" + std::string(yes) + "' loops");
    }
    for (const std::string_view no : {"no", "0", "", "yess"}) {
        expect(!idle_tags(object({{"Loop_Idle_Anim_00", std::string(no)}}), MapKind::space).loop,
               "space: '" + std::string(no) + "' does not loop");
    }
}

} // namespace

int main() {
    land_contracts();
    space_contracts();
    if (failures != 0) {
        std::cerr << failures << " idle tag contract(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "idle tag contracts passed\n";
    return EXIT_SUCCESS;
}
