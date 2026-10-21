#pragma once

#include "eawr/skirmish/start.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/sim/commands.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/victory.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace eawr::script::foc::ai { struct EffectCounts; }

namespace foc_ai_test_support {
namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace foc = eawr::script::foc;
extern int& failures;
extern std::uint64_t effect_barriers;

struct Run {
    std::uint64_t special_use_purchases{}, special_projectiles{}; // observation only in the ordinary battle
    std::set<std::uint64_t> special_projectile_ids;
    std::vector<std::string> hashes;       // combined world and script state per tick
    std::vector<std::string> world_hashes; // world state per tick
    std::vector<std::string> commands;     // routed script commands, in order
    std::vector<std::string> events;       // world events, in order
    std::vector<tactical::Event> ai_rejections; // optional all-admitted observation
    std::set<std::string> diagnostics;     // distinct script diagnostics
    std::int64_t max_instructions{};
    std::int64_t total_instructions{};
    std::uint64_t point_defence_zaps{};    // shots a Crusader's laser defence shot down
    std::uint64_t crusader_hits{};         // projectile hits on the AI player's Crusaders
    std::map<std::string, std::uint64_t> crusader_hit_by; // ... by the shooter's type
    std::uint64_t ai_hits{};               // projectile hits by the AI player's units
    std::uint64_t first_ai_hit{};          // the tick of the AI's first hit, 0 if none
    // The stock economy witness also observes units admitted after the starting roster.
    std::uint64_t stock_hits{}, first_stock_hit{};
    std::uint64_t player_hits{}, first_player_hit{}; // all-admitted AI hits on this fixture's human player
    std::uint64_t pad_constructions_started{}, pad_constructions_completed{}, pending_pad_constructions{};
    std::string hits_csv;                  // every projectile hit
    std::map<std::string, std::uint64_t> orders_by_type; // the AI player's accepted orders per unit type
    std::set<eawr::sim::EntityId> artillery_seen; // observation only: stock proximity consumers
    std::set<eawr::sim::EntityId> droids;  // WAB-71: every Buzz_Droids object seen
    std::int64_t blast_hull_drop{};        // WSD-06: the player's hull lost in the tick after an ability blast object existed
    std::uint64_t blast_objects{};         // WSD-06: ticks that showed an ability blast object
    std::uint64_t sd_alive_first{}, sd_alive_last{}, sd_ready_ticks{}; // WSD-09 diagnostics: a SELF_DESTRUCT holder's life
    std::int64_t sd_min_gap = -1;          // its closest approach to a player unit (world units)
    std::uint64_t plan_buzz{};             // EX-46: accepted BUZZ_DROIDS script requests
    std::uint64_t shield_leech_frames{}; // WLS-04: successful object-target locks in the AI battle
    std::optional<std::uint64_t> first_leech_lock, first_tractor_lock;
    std::optional<std::uint64_t> first_leech_ready_target, first_tractor_ready_target;
    std::uint64_t tractor_frames{};      // WAB-36: frames with a tractor beam locked onto a target
    std::uint64_t concentrate_frames{};  // WAB-36: frames with CONCENTRATE_FIRE on a target
    std::uint64_t recruit_hits{};        // WHE-24: hits on a concentrated target by other shooters
    std::set<eawr::sim::EntityId> corrupted; // WHE-33: recipients reached by the AI's cast
    std::uint64_t plan_lure{};             // WLU-10: accepted LURE script requests
    std::uint64_t lure_frames{};           // WLU-02: craft frames with LURE switched on
    std::vector<std::uint64_t> hidden_refusals; // WST-11/16: ticks of the AI's attack orders refused on a hidden enemy
    std::vector<std::pair<std::uint64_t, eawr::sim::EntityId>> ai_hit_targets; // WST-16: (tick, target) of the AI's hits
    std::vector<std::uint64_t> ai_accepted;     // WST-16: ticks of the AI player's accepted orders
    std::vector<std::string> cloak_contacts; // selected target and visibility at launch/contact
    std::uint64_t salvo_ready_ticks{}, salvo_active_ticks{};
    std::int64_t salvo_min_gap = -1;
    double seconds{};
    tactical::TacticalReplay record;
    foc::AiJournal journal;
};

struct Content {
    skirmish::SkirmishStart start;
    skirmish::SessionContent content;
    tactical::VictoryRules victory;
    foc::AiSetup ai;
    std::map<std::string, std::string> modules;
    std::map<std::string, std::string> plan_modules; // modules and the selected plans
    std::map<tactical::TypeId, std::string> type_names;
    tactical::EconomyRules economy; // SAE-01/02/03: the shared skirmish-start economy.
};

void expect(bool condition, const std::string& message);
std::optional<std::string> environment(const char* name);
std::optional<Content> load(const std::filesystem::path& root, bool rebel_ai = false, bool underworld = false,
                            std::vector<std::string> underworld_fleet = {}, bool crusader = false,
                            std::optional<std::uint64_t> battle_seed = std::nullopt,
                            std::vector<std::string> player_fleet = {},
                            std::vector<std::string> ai_fleet = {}, bool three_vs_three = false,
                            bool serial_profile = false);
int ability_trace(const Content& content, std::uint64_t ticks, const std::filesystem::path& path);
std::optional<Run> run(const Content& content, std::size_t workers, std::uint64_t ticks,
    const std::vector<tactical::PlayerCommand>& input = {}, bool observe_stock = false, bool observe_autofire = false,
    std::ostream* squadron_rows = nullptr);
void progressive_random_probe(const std::filesystem::path& root, const Content& content);
std::optional<Content> combat_engagement_witness(const std::filesystem::path& root, std::uint64_t seed,
    bool economy_disabled = false);
void check_combat_engagement(const Run& result, const std::uint64_t seed, const std::uint64_t ticks);
int passive_contact_control(const std::filesystem::path& root, const std::uint64_t ticks);
int capital_avoidance_probe(const std::filesystem::path& root);
int stock_economy_witness(const std::filesystem::path& root);
void hutt_nearest_probe(const Content& content);
void ability_probe(const Content& content);
void vengeance_probe(const std::filesystem::path& root);
int cloaked_enemy_probe(const std::filesystem::path& root, const std::uint64_t ticks);
void crusader_probe(const std::filesystem::path& root);
void count_effects(std::uint64_t, const foc::ai::EffectCounts& counts) noexcept;
void report_effects(const std::string& label);
int player_stream_profile(const std::filesystem::path& root, std::uint64_t ticks,
    std::size_t only_workers, bool checkpoint_restore = false);
int serial_profile(const std::filesystem::path& root, const std::uint64_t ticks,
    const std::vector<std::size_t>& worker_counts);
} // namespace foc_ai_test_support
