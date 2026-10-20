#pragma once

#include "../support/result_require.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend { class LanMatch; }
namespace eawr::net { enum class LobbyReason : std::uint8_t; struct SetupOffer; }
namespace eawr::sim {
struct MatchAgreement;
namespace math { class Fixed; }
namespace tactical { struct TacticalSetup; struct TacticalContent; }
}

namespace viewer_lan_test {
using Bytes = std::vector<std::uint8_t>;
extern std::string stage;
void expect(bool value, const std::string& message);
eawr::sim::math::Fixed units(std::int64_t value);
eawr::sim::MatchAgreement identity();
eawr::sim::tactical::TacticalSetup setup();
eawr::sim::tactical::TacticalContent content();
eawr::net::SetupOffer offer();
eawr::sim::tactical::TacticalSetup human_setup(unsigned count);
using eawr::testing::result_code_colon::require;
std::shared_ptr<eawr::presentation::godot_backend::LanMatch> make_match(std::filesystem::path root = {}, std::uint64_t delay = 3,
    bool progress = false, std::filesystem::path trace = {});
void player_row_parser();
void staging_ingress_bounds();
void refused_rosters();
void separate_mesh(std::uint32_t count, bool coop, bool ability_triage = false, unsigned workers = 1);
void shared_builder_matches_former_live_encoding();
void lan_overflow_human_keeps_its_seat();
void lan_ai_defaults_follow_authored_constraints();
void lobby_refusal_and_placeholder_contracts();
void terminal_lobby_commands_are_inert();
void close_one_mesh_edge();
void missing_planned_owner_is_refused();
void nine_seat_staging(const std::vector<std::uint32_t>& owners);
void editable_staging_team_rules();
void advanced_options_three_peers();
void authoritative_staging(bool play, std::optional<eawr::net::LobbyReason> end_during_start = {});
void loopback(bool abrupt, bool departed_ahead = false, bool closed_before_launch = false, int final_window = 0,
    bool speed_during_departure = false, std::shared_ptr<eawr::presentation::godot_backend::LanMatch> existing_host = {},
    std::shared_ptr<eawr::presentation::godot_backend::LanMatch> existing_join = {}, unsigned workers = 1, bool fail_recovery = false,
    int policy_disagreement = 0, bool script_only = false);
}
