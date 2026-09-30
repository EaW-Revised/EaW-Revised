// Unit clip contracts (#81): the retail clip naming rule, the Death_Clone
// entry a destroyed unit uses, the death clip's timing and its sampled pose. The file names
// below are the FoC death-clone clips the M2 roster uses; no asset is read.

#include "eawr/presentation/animation/unit_clips.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace clips = eawr::presentation::animation;

int failures{};
void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void test_table() {
    expect(clips::clip_type_names.size() == 119, "the retail table has 119 types");
    expect(clips::clip_type_names[clips::clip_types::idle] == "IDLE", "type 0 is IDLE");
    expect(clips::clip_type_names[clips::clip_types::space_idle] == "SPACE_IDLE", "type 1 is SPACE_IDLE");
    expect(clips::clip_type_names[clips::clip_types::move] == "MOVE", "type 2 is MOVE");
    expect(clips::clip_type_names[clips::clip_types::attack] == "ATTACK", "type 5 is ATTACK");
    expect(clips::clip_type_names[clips::clip_types::attack_idle] == "ATTACKIDLE", "type 6 is ATTACKIDLE");
    expect(clips::clip_type_names[clips::clip_types::die] == "DIE", "type 7 is DIE");
    expect(clips::clip_type_names[clips::clip_types::deploy] == "DEPLOY", "type 25 is DEPLOY");
    expect(clips::clip_type_names[clips::clip_types::undeploy] == "UNDEPLOY", "type 26 is UNDEPLOY");
    std::set<std::string_view> unique(clips::clip_type_names.begin(), clips::clip_type_names.end());
    expect(unique.size() == clips::clip_type_names.size(), "no two types share a name");
}

void test_model_clip_paths() {
    const std::set<std::string> files{
        "data/art/models/rv_corvette_d_die_00.ala",
        "data/art/models/rv_nebulonb_d_die_00.ala",
        "data/art/models/rv_nebulonb_d_death_00.ala",
        "data/art/models/rv_xwing_deploy_00.ala",
        "data/art/models/rv_xwing_undeploy_00.ala",
        "data/art/models/w_test_idle_00.ala",
        "data/art/models/w_test_idle_01.ala",
        "data/art/models/w_test_idle_03.ala",
        "data/art/models/w_other_move_00.ala",
    };
    const auto exists = [&](const std::string& path) { return files.contains(path); };
    const auto die = clips::model_clip_paths("DATA\\ART\\MODELS\\RV_Corvette_D.ALO", clips::clip_types::die, exists);
    expect(die == std::vector<std::string>{"data/art/models/rv_corvette_d_die_00.ala"},
           "the corvette death clone finds its DIE clip, path lowered and slashes forward");
    const auto nebulon = clips::model_clip_paths("data/art/models/rv_nebulonb_d.alo", clips::clip_types::die, exists);
    expect(nebulon == std::vector<std::string>{"data/art/models/rv_nebulonb_d_die_00.ala"},
           "a _death_ file is no DIE clip");
    expect(clips::model_clip_paths("data/art/models/rv_xwing.alo", clips::clip_types::deploy, exists).size() == 1,
           "the X-wing has one DEPLOY clip");
    expect(clips::model_clip_paths("data/art/models/rv_xwing.alo", clips::clip_types::attack, exists).empty(),
           "the X-wing has no ATTACK clip");
    expect(clips::model_clip_paths("data/art/models/w_test.alo", clips::clip_types::idle, exists).size() == 2,
           "variants stop at the first missing index");
    expect(clips::model_clip_paths("data/art/models/w_test.alo", clips::clip_types::move, exists,
                                   "W_Other.alo").size() == 1,
           "an animation override name replaces the model stem");
    expect(clips::model_clip_paths("data/art/models/w_test.ala", clips::clip_types::idle, exists).empty(),
           "a path that is no model has no clips");
    expect(clips::model_clip_paths("data/art/models/w_test.alo", 119, exists).empty(), "a type past the table");
}

void test_parse_clip_name() {
    const auto die = clips::parse_clip_name("Data\\Art\\Models\\RV_Corvette_D_DIE_00.ALA");
    expect(die.size() == 1 && die[0] == clips::ClipName{"data/art/models/rv_corvette_d.alo", clips::clip_types::die, 0},
           "the corvette death clip belongs to rv_corvette_d.alo");
    const auto turn = clips::parse_clip_name("data/art/models/ev_at-aa_turnr_half_00.ala");
    expect(!turn.empty() && turn[0].model_path == "data/art/models/ev_at-aa.alo" && turn[0].type == 23,
           "TURNR_HALF is read before a shorter type name");
    const auto attack = clips::parse_clip_name("data/art/models/ei_vader_attack_01.ala");
    expect(attack.size() == 1 && attack[0].type == clips::clip_types::attack && attack[0].index == 1,
           "an ATTACK variant keeps its index");
    const auto ambiguous = clips::parse_clip_name("data/art/models/x_attackidle_00.ala");
    expect(ambiguous.size() == 1 && ambiguous[0].type == clips::clip_types::attack_idle,
           "ATTACKIDLE is one type, ATTACK needs its own underscore");
    const auto both = clips::parse_clip_name("data/art/models/x_move_idle_00.ala");
    expect(both.size() == 1 && both[0].model_path == "data/art/models/x_move.alo",
           "MOVE_IDLE is no type; the model is x_move");
    expect(clips::parse_clip_name("data/art/models/nb_basepad_deploy.ala").empty(),
           "a clip without a two-digit index is never loaded");
    expect(clips::parse_clip_name("data/art/models/ub_turretmst50_idle00.ala").empty(),
           "an index without its underscore is never loaded");
    expect(clips::parse_clip_name("data/art/models/x_idle_00.alo").empty(), "a model file is no clip");
    expect(clips::parse_clip_name("data/art/models/_idle_00.ala").empty(), "an empty base is no model");
}

void test_death_clone_type() {
    const std::vector<std::string> corvette{"Damage_Normal, Corellian_Corvette_Death_Clone"};
    expect(clips::death_clone_type(corvette) == "Corellian_Corvette_Death_Clone", "the Damage_Normal entry");
    const std::vector<std::string> several{" Damage_Force_Lightning , A", "damage_misc,B", "Damage_Normal ,\tC "};
    expect(clips::death_clone_type(several) == "C", "Damage_Normal wins, whitespace trimmed");
    const std::vector<std::string> misc{"Damage_Fire, A", "Damage_Misc, B"};
    expect(clips::death_clone_type(misc) == "B", "Damage_Misc is the fallback");
    const std::vector<std::string> none{"Damage_Fire, A", "Damage_Normal", "Damage_Normal, "};
    expect(!clips::death_clone_type(none), "no usable entry");
}

void test_death_frame() {
    // The corvette death clip: rv_corvette_d_die_00 at 30 fps; 90 playable frames here.
    const clips::DeathPlayback held{.blend_ticks = 15, .persistence_ticks = std::nullopt, .fade_ticks = 8};
    const auto start = clips::death_frame(held, 90, 30, 0, 30);
    expect(start && start->position == clips::ClipPosition{0, 30} && start->blend_from == 1.0F && start->shown,
           "the clip starts at frame 0 blending in");
    const auto half = clips::death_frame(held, 90, 30, 15, 30);
    expect(half && half->position.position == 15U * 30U && half->blend_from == 0.0F, "the blend ends at 0.5 s");
    const auto quarter = clips::death_frame(held, 90, 30, 3, 30);
    expect(quarter && quarter->blend_from > 0.79F && quarter->blend_from < 0.81F, "the blend weight falls linearly");
    const auto end = clips::death_frame(held, 90, 30, 90, 30);
    const auto later = clips::death_frame(held, 90, 30, 100000, 30);
    expect(end && later && end->position.position == 90U * 30U && later->position == end->position,
           "the clip holds its last frame");
    expect(later && later->shown, "without a persistence duration the clone stays");
    // A 5 fps clip on a 30 Hz clock ends on the first tick at or past its end.
    const auto slow = clips::death_frame(held, 7, 5, 41, 30);
    const auto slow_end = clips::death_frame(held, 7, 5, 42, 30);
    expect(slow && slow->position.position == 41U * 5U && slow_end && slow_end->position.position == 7U * 30U,
           "a slow clip reaches its end exactly");
    // The M2 stations: Death_Persistence_Duration 34 s, the default 0.25 s fade.
    const clips::DeathPlayback station{.blend_ticks = 15, .persistence_ticks = 34U * 30U, .fade_ticks = 8};
    const auto shown = clips::death_frame(station, 90, 30, 90 + 34 * 30 + 7, 30);
    const auto gone = clips::death_frame(station, 90, 30, 90 + 34 * 30 + 8, 30);
    expect(shown && shown->shown && gone && !gone->shown, "the clone is gone once its fade has run out");
    expect(!clips::death_frame(held, 90, 0, 0, 30) && !clips::death_frame(held, 90, 30, 0, 0), "zero rates");
    const clips::DeathPlayback huge{.blend_ticks = 0, .persistence_ticks = ~std::uint64_t{0}, .fade_ticks = 1};
    expect(!clips::death_frame(huge, 90, 30, 0, 30), "a persistence past 64 bits");
    expect(clips::seconds_to_ticks(0.5, 30) == 15U && clips::seconds_to_ticks(0.25, 30) == 8U
               && clips::seconds_to_ticks(34.0, 30) == 1020U,
           "seconds round to the nearest tick");
    expect(!clips::seconds_to_ticks(-1.0, 30) && !clips::seconds_to_ticks(1.0e300, 30), "negative or huge seconds");
    // A clone whose clip did not start: its persistence runs from its appearance (#363 review 2).
    const clips::DeathPlayback posed{.blend_ticks = 15, .persistence_ticks = 60U, .fade_ticks = 8};
    const auto still = clips::death_frame(posed, 0, 30, 67, 30);
    const auto faded = clips::death_frame(posed, 0, 30, 68, 30);
    expect(still && still->shown && still->position.position == 0U && faded && !faded->shown,
           "a posed clone fades after its persistence from the frame it appeared");
    const auto kept = clips::death_frame(held, 0, 30, 100000, 30);
    expect(kept && kept->shown, "a posed clone without a persistence duration stays");
}

void test_death_start() {
    // #363 review 2: Specific_Death_Anim_Type names the clip type; Remove_Upon_Death alone removes
    // a clone whose clip does not start (DeathBehavior::Init).
    expect(clips::clip_type_index("DIE") == clips::clip_types::die && clips::clip_type_index(" die\t") == clips::clip_types::die,
           "DIE, case and blanks ignored");
    expect(clips::clip_type_index("Crushed") && clips::clip_type_names[*clips::clip_type_index("Crushed")] == "CRUSHED",
           "another death type");
    expect(!clips::clip_type_index("NOT_A_TYPE") && !clips::clip_type_index(""), "a name not in the table");
    expect(clips::death_clip_variant(3, std::nullopt, 7) == 1U, "no declared index: the draw modulo the variants");
    expect(clips::death_clip_variant(3, 2U, 7) == 2U, "the declared Specific_Death_Anim_Index");
    expect(!clips::death_clip_variant(3, 3U, 7), "a declared index past the last variant does not start");
    expect(!clips::death_clip_variant(0, std::nullopt, 7) && !clips::death_clip_variant(0, 0U, 7), "no clip of the type");
    expect(clips::death_start(true, true) == clips::DeathStart::clip
               && clips::death_start(true, false) == clips::DeathStart::clip,
           "a started clip plays whatever Remove_Upon_Death says");
    expect(clips::death_start(false, true) == clips::DeathStart::removed, "no clip with Remove_Upon_Death: removed");
    expect(clips::death_start(false, false) == clips::DeathStart::pose, "no clip without Remove_Upon_Death: kept posed");
}

// One bone moving along x from 0 to 10 over one playable frame at 1 fps; its
// bind pose sits at the origin.
std::optional<clips::Player> synthetic_death_player() {
    eawr::assets::Bone root;
    root.name = "root";
    root.parent = -1;
    root.visible = true;
    root.relative_transform = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    eawr::assets::Model model;
    model.bones = {root};
    eawr::assets::AnimationTrack track;
    track.bone_index = 0;
    track.bone_name = "root";
    track.translation_interpolation = eawr::assets::Interpolation::linear;
    track.scale_interpolation = eawr::assets::Interpolation::linear;
    track.rotation_interpolation = eawr::assets::Interpolation::spherical;
    track.samples = {
        {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
        {{10.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
    };
    eawr::assets::Animation animation;
    animation.source.logical_path = "synthetic_die_00.ala";
    animation.stored_frame_count = 2;
    animation.playable_frame_count = 1;
    animation.frames_per_second = 1.0F;
    animation.duration_seconds = 1.0F;
    animation.tracks.push_back(track);
    auto player = clips::Player::create(model, &animation);
    if (!player) return std::nullopt;
    return std::move(player.value());
}

void test_sample_death_frame() {
    const auto player = synthetic_death_player();
    expect(player.has_value(), "the synthetic death clip binds");
    if (!player) return;
    const auto x_at = [&](const clips::DeathFrame& frame) -> std::optional<float> {
        const auto pose = clips::sample_death_frame(*player, frame);
        if (!pose) return std::nullopt;
        return pose.value().bones[0].model_asset[12];
    };
    const auto near = [](const std::optional<float> value, const float expected) {
        return value && std::abs(*value - expected) < 0.0001F;
    };
    expect(near(x_at({clips::ClipPosition{15, 30}, 0.0F}), 5.0F), "no blend: the clip position alone");
    expect(near(x_at({clips::ClipPosition{15, 30}, 0.5F}), 2.5F), "half blended in from the bind pose");
    expect(near(x_at({clips::ClipPosition{30, 30}, 1.0F}), 0.0F), "fully blended: the bind pose");
    expect(!clips::sample_death_frame(*player, {clips::ClipPosition{1, 0}, 0.5F}), "zero subdivisions fail");
}

} // namespace

int main() {
    test_table();
    test_model_clip_paths();
    test_parse_clip_name();
    test_death_clone_type();
    test_death_frame();
    test_death_start();
    test_sample_death_frame();
    if (failures != 0) {
        std::cerr << failures << " unit clip contract(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "unit clip contracts passed\n";
    return EXIT_SUCCESS;
}
