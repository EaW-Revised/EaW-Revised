#pragma once

#include "eawr/presentation/animation/idle_playback.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Unit clips from tactical state (#81). FoC names a model's clips by rule, not
// in XML: for every clip type it loads `<model>_<TYPE>_NN.ala` for NN = 00, 01,
// ... up to the first missing index. The facts and their evidence are in
// docs/behaviour/unit-animation.md. Engine-free; presentation only: the clip
// clock is the view's, never the simulation's.
namespace eawr::presentation::animation {

// The retail clip types in their table order; the index is the type.
inline constexpr std::array<std::string_view, 119> clip_type_names{
    "IDLE", "SPACE_IDLE", "MOVE", "TURNL", "TURNR", "ATTACK", "ATTACKIDLE", "DIE", "ROTATE",
    "SPECIAL_A", "SPECIAL_B", "SPECIAL_C", "TURNL_BEGIN", "TURNL_END", "TURNR_BEGIN", "TURNR_END",
    "MOVESTART", "MOVE_ENDONE", "MOVE_ENDTWO", "MOVE_ENDTHREE", "MOVE_ENDFOUR", "TURNL_HALF",
    "TURNL_QUARTER", "TURNR_HALF", "TURNR_QUARTER", "DEPLOY", "UNDEPLOY", "CINEMATIC",
    "BLOCK_BLASTER", "REDIRECT_BLASTER", "IDLE_BLOCKBLASTER", "FW_ATTACK", "FW_DIE", "FTK_ATTACK",
    "FTK_HOLD", "FTK_RELEASE", "FTK_DIE", "FB_ATTACK", "FB_HOLD", "FB_RELEASE", "FL_ATTACK", "FL_DIE",
    "FORCE_RUN", "LAND", "TAKEOFF", "FLAME_ATTACK", "DEMOLITION", "BOMBTOSS", "JUMP", "FLYIDLE",
    "FLYLAND", "FLYLANDIDLE", "FLYLANDDROP", "HC_WIN", "HC_LOSE", "HC_DRAW", "SHIELD_ON",
    "SHIELD_OFF", "CA_DIE", "DEPLOYED_CA_DIE", "DEPLOYED_DIE", "FIRE_MOVE", "FIRE_DIE",
    "POUND_ATTACK", "EAT_ATTACK", "EATEN_DIE", "WALKMOVE", "CROUCHMOVE", "OPEN", "HOLD", "CLOSE",
    "CROUCHIDLE", "CROUCHTURNL", "CROUCHTURNR", "BUILD", "TRANS", "SELF_DESTRUCT", "ATTENTION",
    "CELEBRATE", "FLINCHL", "FLINCHR", "FLINCHF", "FLINCHB", "ATTACKFLINCHL", "ATTACKFLINCHR",
    "ATTACKFLINCHF", "ATTACKFLINCHB", "TALK", "TALKGESTURE", "TALKQUESTION", "HACKING", "REPAIRING",
    "CHOKE", "CHOKEDEATH", "TROOPDROP", "ROPESLIDE", "ROPELAND", "ROPE_DROP", "ROPE_LIFT", "ALARM",
    "WARNING", "CRUSHED", "POWERDOWN", "POWERUP", "SPINMOVE", "FORCE_REVEAL_BEGIN",
    "FORCE_REVEAL_LOOP", "FORCE_REVEAL_END", "SWORD_THROW", "SWORD_CONTROL", "SWORD_CATCH",
    "SWORDSPIN", "CONTAMINATE_ATTACK", "CONTAIMINATE_LOOP", "CONTAMINATE_RELEASE", "WALK",
    "PAD_BUILD", "PAD_SELL", "HEAL"};

// The types the tactical states below use.
namespace clip_types {
inline constexpr std::size_t idle = 0;
inline constexpr std::size_t space_idle = 1;
inline constexpr std::size_t move = 2;
inline constexpr std::size_t attack = 5;
inline constexpr std::size_t attack_idle = 6;
inline constexpr std::size_t die = 7;
inline constexpr std::size_t deploy = 25;
inline constexpr std::size_t undeploy = 26;
} // namespace clip_types

// The logical ALA paths of `model_path`'s clips of `type`: the model's
// directory and stem (or `anim_override`'s stem, the *_Model_Anim_Override_Name
// the object type declares) joined with `_<TYPE>_NN.ala`, lower case, for NN
// from 00 while `exists` finds the file. Empty for a type past the table or a
// model path without `.alo`.
[[nodiscard]] std::vector<std::string> model_clip_paths(
    std::string_view model_path, std::size_t type, const std::function<bool(const std::string&)>& exists,
    std::string_view anim_override = {});

// One reading of an ALA path under the naming rule: the model path it would
// belong to (`<dir>/<base>.alo`), its type and its index.
struct ClipName final {
    std::string model_path;
    std::size_t type{};
    std::uint32_t index{};

    friend bool operator==(const ClipName&, const ClipName&) = default;
};

// Every way `ala_path` splits as `<dir>/<base>_<TYPE>_NN.ala` (NN two decimal
// digits, TYPE in the table, ASCII case-insensitive), longest base first. An
// empty result means no model's load can ever reach this file.
[[nodiscard]] std::vector<ClipName> parse_clip_name(std::string_view ala_path);

// The Death_Clone entry a destroyed unit uses. Retail picks the entry for the
// killing blow's damage type, then the Damage_Misc entry, then the unit's own
// type. The tactical snapshot carries no damage type, so this picks the
// Damage_Normal entry, then Damage_Misc; nullopt when neither is listed.
// `entries` are the Death_Clone values, "<damage type>, <object type>".
[[nodiscard]] std::optional<std::string> death_clone_type(std::span<const std::string> entries);

// A clip type name's index in clip_type_names (ASCII case-insensitive,
// surrounding blanks ignored), as Specific_Death_Anim_Type names one; nullopt
// for a name not in the table.
[[nodiscard]] std::optional<std::size_t> clip_type_index(std::string_view name);

// Which of the clone model's `variants` clips of its death type starts
// (DeathBehavior; Set_Target_Animation_Type): the declared
// Specific_Death_Anim_Index, or when none is declared the variant retail draws
// from the synchronized generator, here `draw` modulo the count (UA-P2).
// Nullopt when the clip cannot start: the model has no clip of the type or the
// declared index is past its last one.
[[nodiscard]] std::optional<std::size_t> death_clip_variant(std::size_t variants, std::optional<std::uint32_t> index,
                                                           std::uint64_t draw) noexcept;

// What a death clone does when it appears: plays its clip, or, when the clip
// did not start, is removed at once if its type has Remove_Upon_Death and
// otherwise stays in the pose it has (DeathBehavior::Init).
enum class DeathStart : std::uint8_t { clip, pose, removed };
[[nodiscard]] DeathStart death_start(bool clip_started, bool remove_upon_death) noexcept;

// How a death clone plays its death clip (DeathBehavior; Death_* tags):
// Specific_Death_Anim_Type (DIE) once, blended in from the pose it had over
// `blend_ticks`, then held on its last frame. With a Death_Persistence_Duration
// of zero or more the clone fades out over Death_Fade_Time once the clip has
// ended and that duration has passed; the default -1 never fades it.
struct DeathPlayback final {
    std::uint32_t blend_ticks{};
    std::optional<std::uint64_t> persistence_ticks;
    std::uint64_t fade_ticks{};

    friend bool operator==(const DeathPlayback&, const DeathPlayback&) = default;
};

// Seconds as whole ticks of a `ticks_per_second` clock, rounded to nearest;
// nullopt for a negative, non-finite or out-of-range value.
[[nodiscard]] std::optional<std::uint64_t> seconds_to_ticks(double seconds, std::uint32_t ticks_per_second) noexcept;

// The death clip at `tick` ticks after the clone appeared: the clip position
// (clamped to its last frame), the weight of the pose it blends in from (1 at
// the start, 0 from blend_ticks on) and whether the clone is still shown (it
// is hidden once its fade has run out, and never shown again). A clone whose
// clip did not start (DeathStart::pose) passes 0 playable frames: retail starts
// its persistence count at once, since its active clip is not the death type.
// Nullopt when the frame or tick rate is
// zero or the arithmetic does not fit 64 bits.
struct DeathFrame final {
    ClipPosition position{};
    float blend_from{};
    bool shown{true};

    friend bool operator==(const DeathFrame&, const DeathFrame&) = default;
};
[[nodiscard]] std::optional<DeathFrame> death_frame(const DeathPlayback& playback, std::uint32_t playable_frames,
                                                    std::uint32_t frames_per_second, std::uint64_t tick,
                                                    std::uint32_t ticks_per_second) noexcept;

// The pose of `player`'s death clip at `frame`: the clip sampled at the
// frame's position by Player::sample_position, blended in from the bind pose
// by the frame's blend_from while that is above 0. When the blended sample
// fails the unblended pose stands; an unblended failure is returned.
[[nodiscard]] core::Result<Pose> sample_death_frame(const Player& player, const DeathFrame& frame);

} // namespace eawr::presentation::animation
