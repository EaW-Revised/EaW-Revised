#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace eawr::viewer::galactic {

enum class BattleDialogPhase { hidden, choice, progress, summary };
enum class BattleChoice { fight, auto_resolve, retreat, continue_galaxy, invade };
struct BattleDialogSide {
    std::string name, icon;
    std::array<float, 3> colour{1, 1, 1};
    std::vector<std::string> combatants;
    friend bool operator==(const BattleDialogSide&, const BattleDialogSide&) = default;
};
struct CampaignRecordEntry {
    std::string original_type;
    std::uint32_t owner{};
};
struct CampaignResultType {
    std::string name;
    bool scored{}, hero{};
    double score_cost{}, combat_power{};
    // WGP-115: resolved UTF-16 display units, before UTF-8 rendering.
    std::u16string lexical_name{};
};
struct CampaignLoss {
    std::string type, name; // WAR-88 original type name, not tactical numeric ID
    std::uint64_t count{};
    friend bool operator==(const CampaignLoss&, const CampaignLoss&) = default;
};
struct CampaignResults {
    std::array<std::vector<CampaignLoss>, 2> losses, heroes;
    std::array<std::uint64_t, 2> totals{};
    std::array<double, 2> score_cost{}, combat_power{};
    std::uint64_t elapsed_milliseconds{};
    friend bool operator==(const CampaignResults&, const CampaignResults&) = default;
};
// WGP-86: copied local tally, never a payment or a cost-derived award.
// The result/scoring host supplies the localized amount caption.
struct CampaignBounty {
    bool positive{}, benefits_from_corruption{};
    std::string caption;
    friend bool operator==(const CampaignBounty&, const CampaignBounty&) = default;
};
inline bool campaign_bounty_visible(const std::optional<CampaignBounty>& bounty) {
    return bounty && (bounty->positive || bounty->benefits_from_corruption);
}
// WGP-85: independently round the complementary unrounded figures.
inline std::array<unsigned, 2> campaign_efficiency(const CampaignResults& results) {
    const auto total = results.combat_power[0] + results.combat_power[1];
    const auto own = total > 0 ? std::clamp(results.combat_power[1] * 100 / total, 0.0, 100.0) : 0.0;
    return {static_cast<unsigned>(std::floor(own + 0.5)), static_cast<unsigned>(std::floor(100 - own + 0.5))};
}
// The conflict owner supplies permissions and elapsed time. This view never
// drives the solver, guesses admission, or adds a simulation delay (GFC-24).
struct BattleDialogView {
    std::uint64_t conflict{}, response_revision{}; // owner increments on a new choice response
    BattleDialogPhase phase{BattleDialogPhase::hidden};
    std::string planet;
    bool pending_land{}, pending_multiplayer{};
    std::optional<std::uint64_t> pending_countdown;
    std::array<BattleDialogSide, 2> sides; // defender, invader (WAR-67)
    bool pending_visible{true}, single_button{}, waiting_for_opponent{}, roster_complete{};
    bool involved{}, choice_ready{true}, fight_allowed{}, tactical_available{}, auto_allowed{}, retreat_allowed{}, won{};
    std::string refusal;
    double elapsed_seconds{}, display_seconds{}; // validated AutoResolveDisplayTime
    CampaignResults results;
    std::string picture;
    std::optional<CampaignBounty> bounty;
    bool auto_resolved{};
    std::uint64_t invasion_fleet{}, invasion_planet{};
    std::vector<std::string> advisor;
    std::string tactical_refusal_code, tactical_refusal;
    friend bool operator==(const BattleDialogView&, const BattleDialogView&) = default;
};

// GHUD-S75: solo suffixes include their authored punctuation. Multiplayer
// inserts the separator itself and appends the published whole-second timer.
inline std::string pending_battle_title(const BattleDialogView& view,
    const std::function<std::string(std::string_view)>& text) {
    if (!view.pending_multiplayer)
        return view.planet + text(view.pending_land ? "TEXT_TACTICAL_LAND_BATTLE_IS_PENDING"
            : "TEXT_TACTICAL_SPACE_BATTLE_IS_PENDING");
    auto value = view.planet + ": " + text(view.waiting_for_opponent ? "TEXT_WAITING_FOR_OPPONENT"
        : view.pending_land ? "TEXT_MP_TACTICAL_LAND_BATTLE_IS_PENDING" : "TEXT_MP_TACTICAL_SPACE_BATTLE_IS_PENDING");
    if (view.pending_countdown) value += " " + std::to_string(*view.pending_countdown);
    return value;
}

// Both runtime reports use the same observer-scoped admission projection.
inline std::string battle_tactical_report_json(const BattleDialogView& view,
    const std::function<std::string(std::string_view)>& quote) {
    return std::string("{\"available\":") + (view.tactical_available ? "true" : "false")
        + ",\"code\":" + quote(view.tactical_refusal_code)
        + ",\"reason\":" + quote(view.tactical_refusal) + "}";
}

inline bool battle_auto_enabled(const BattleDialogView& view, bool submitted) {
    return view.involved && view.phase == BattleDialogPhase::choice
        && view.choice_ready && view.auto_allowed && !submitted;
}

// Latch only accepted dispatches; a click-time refusal permits an immediate retry.
inline bool dispatch_battle_choice(bool& submitted, std::uint64_t conflict, BattleChoice choice,
    const std::function<bool(std::uint64_t, BattleChoice)>& dispatch) {
    if (submitted) return false;
    submitted = true;
    if (!dispatch || !dispatch(conflict, choice)) { submitted = false; return false; }
    return true;
}

// WAR-67: these are identical time bars, never remaining-health graphs.
inline double auto_resolve_progress(double elapsed, double display_seconds) {
    if (!std::isfinite(elapsed) || !std::isfinite(display_seconds) || display_seconds <= 0) return 0;
    return 1.0 - std::clamp(elapsed / (display_seconds * 0.6666), 0.0, 1.0);
}

// WAR-88/WBF-45: consume the confirmed record, not selected lethal requests
// or a before/after fleet diff. Keep original-type identity and separate heroes.
inline std::u16string campaign_name_units(std::string_view name) {
    std::u16string units;
    for (std::size_t index = 0; index < name.size();) {
        const auto first = static_cast<unsigned char>(name[index++]);
        std::uint32_t code = first;
        unsigned remaining = 0;
        if (first >= 0xF0) { code &= 7; remaining = 3; }
        else if (first >= 0xE0) { code &= 15; remaining = 2; }
        else if (first >= 0xC0) { code &= 31; remaining = 1; }
        // Text database values round-trip through valid UTF-8; identifiers
        // missing a translation use the same character-unit adapter.
        for (unsigned count = 0; count < remaining && index < name.size(); ++count)
            code = (code << 6) | (static_cast<unsigned char>(name[index++]) & 63);
        if (code <= 0xFFFF) units.push_back(static_cast<char16_t>(code));
        else {
            code -= 0x10000;
            units.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
            units.push_back(static_cast<char16_t>(0xDC00 + (code & 1023)));
        }
    }
    return units;
}

inline CampaignResults campaign_record_results(
    std::span<const CampaignRecordEntry> record, std::uint32_t local,
    const std::function<bool(std::uint32_t)>& enemy,
    const std::function<CampaignResultType(std::string_view)>& type_info) {
    CampaignResults result;
    std::array<std::array<std::map<std::u16string, CampaignLoss>, 2>, 2> grouped;
    for (const auto& row : record) {
        if (row.owner != local && !enemy(row.owner)) continue;
        const auto info = type_info(row.original_type);
        if (!info.scored) continue;
        const std::size_t side = row.owner == local ? 0 : 1;
        const auto key = info.lexical_name.empty() ? campaign_name_units(info.name) : info.lexical_name;
        auto [group, inserted] = grouped[side][info.hero ? 1 : 0].try_emplace(key,
            CampaignLoss{row.original_type, info.name, 0});
        static_cast<void>(inserted);
        ++group->second.count; // First provider entry retains its representative.
        ++result.totals[side];
        result.score_cost[side] += info.score_cost;
        result.combat_power[side] += info.combat_power;
    }
    for (std::size_t side = 0; side < 2; ++side) for (std::size_t pane = 0; pane < 2; ++pane)
        for (auto& [name, loss] : grouped[side][pane]) {
            static_cast<void>(name);
            (pane ? result.heroes[side] : result.losses[side]).push_back(std::move(loss));
        }
    return result; // WGP-34: auto-resolve time is zero.
}

inline std::string campaign_loss_count(std::uint64_t count) {
    return count > 1 ? "x" + std::to_string(count) : std::string{};
}

struct AdvisorLine {
    std::string key;
    std::vector<std::string> tokens;
};
// WAR-64 counts all record deaths, independently of summary scoring filters.
inline std::vector<AdvisorLine> auto_resolve_advisor(
    std::span<const CampaignRecordEntry> record, std::uint32_t viewer,
    std::uint32_t winner, bool land, std::string faction, std::string planet,
    const std::function<bool(std::string_view)>& named_hero,
    const std::function<std::string(std::string_view)>& display_name) {
    std::vector<AdvisorLine> lines;
    std::uint64_t own{}, other{};
    for (const auto& row : record) {
        if (named_hero(row.original_type)) lines.push_back({"TEXT_AR_HERO_LOSS", {display_name(row.original_type), planet}});
        if (row.owner == viewer) ++own; else ++other;
    }
    const bool won = viewer == winner;
    const auto smaller = won ? own : other;
    const auto larger = won ? other : own;
    const std::string band = smaller < larger / 3 + (larger % 3 != 0 ? 1 : 0)
        ? "MAJOR" : smaller < larger ? "REGULAR" : "MINOR";
    lines.push_back({std::string("TEXT_AR_") + (land ? "LAND_" : "SPACE_") + (won ? "WIN_" : "LOSS_") + band,
        {std::move(faction), std::move(planet), std::to_string(own)}});
    return lines;
}

} // namespace eawr::viewer::galactic
