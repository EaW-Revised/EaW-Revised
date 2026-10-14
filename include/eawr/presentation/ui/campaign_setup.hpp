#pragma once

// The galactic custom-campaign staging screen's model (MNU-34, WGP-28/29/40, WGP-65..67, docs/ui/
// menu-screens.md): which campaign entries the list shows and in which order, the faction choices,
// the difficulty radios, and when Start and the options button are available. Engine-free: the
// viewer reads the facts from the loaded campaign data and the text database, draws the retail dialog
// and gives the Start request to the host's campaign launcher.
//
// Nothing here names a campaign, a set or a faction: the list is whatever the data lists, so a mod's
// campaigns and factions appear without code (owner rule 2026-10-10).

#include "eawr/presentation/ui/front_end.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

// The catalogue dialog the screen is built from.
inline constexpr std::string_view campaign_staging_dialog = "IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG";

// One authored campaign entry as the staging reads it. Titles are already localized: the sort
// order compares them (WGP-66), and the model has no text database.
struct CampaignFacts final {
    std::string name;         // the entry's definition name
    std::string campaign_set; // Campaign_Set: the entries of one scenario, one per faction
    std::string faction;      // Starting_Active_Player: the faction the entry is played as
    std::string title;        // Text_ID, localized
    std::string description;  // Description_Text, localized
    // WGP-74: the defaults of a tag the entry does not author.
    std::int64_t sort_order{1000};
    bool listed{true};
    bool multiplayer{};
    bool tutorial{};
    bool supports_custom_settings{true};
    std::int64_t starting_credits{};
    std::int64_t starting_tech{};
    std::int64_t maximum_tech{5};
    std::int64_t minimum_custom_tech{1};
    std::int64_t maximum_custom_tech{5};
};

struct FactionFacts final {
    std::string name;  // the faction's data name
    std::string title; // Text_ID, localized
    bool playable{};
    std::int64_t displayed_tech_adjustment{};
};

// What the campaign host can apply today. A level or option it cannot apply stays on the dialog,
// disabled with the reason below, never silently ignored.
struct CampaignHostSupport final {
    bool difficulty{};     // easy and hard; normal is the authored default every host starts at
    bool custom_options{}; // the advanced options page (credits, tech levels, auto-resolve)
};

enum class CampaignDifficulty : std::uint8_t { easy, normal, hard };
[[nodiscard]] std::string_view to_string(CampaignDifficulty difficulty) noexcept;

// WGP-65/66: one visible row of the list.
struct CampaignRow final {
    std::size_t campaign{}; // index into the facts the model was built from
    std::string title;
    std::string description;
};

// WGP-40: one entry of the faction drop-down.
struct FactionChoice final {
    std::string name;
    std::string title;
    std::int64_t displayed_tech_adjustment{};
};

class CampaignSetupModel final {
public:
    CampaignSetupModel(std::vector<CampaignFacts> campaigns, std::vector<FactionFacts> factions,
                       CampaignHostSupport support = {});

    // WGP-65: the entries with Is_Listed true and no multiplayer or tutorial flag, the first
    // representative of each Campaign_Set in the data's order, sorted by Sort_Order and then by
    // title (case-insensitive). Empty when the data lists none: the entry stays off.
    [[nodiscard]] const std::vector<CampaignRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] const std::vector<CampaignFacts>& campaigns() const noexcept { return campaigns_; }
    // WGP-40: the playable factions by display name (the drop-down sorts), and the last is selected
    // while nothing else is.
    [[nodiscard]] const std::vector<FactionChoice>& factions() const noexcept { return factions_; }

    // Opening the screen: no row (WGP-66: a profile's last campaign is not stored yet), the last
    // faction and the normal difficulty (WGP-40).
    void reset() noexcept;
    void select_row(std::optional<std::size_t> row);
    [[nodiscard]] std::optional<std::size_t> selected_row() const noexcept { return row_; }
    [[nodiscard]] const CampaignRow* selected() const noexcept;
    // The description box: the selected row's, empty with none (WGP-40).
    [[nodiscard]] std::string_view description() const noexcept;

    void select_faction(std::optional<std::size_t> faction);
    [[nodiscard]] std::optional<std::size_t> selected_faction() const noexcept { return faction_; }

    // WGP-40: normal on open. A level the host cannot apply is refused.
    [[nodiscard]] CampaignDifficulty difficulty() const noexcept { return difficulty_; }
    [[nodiscard]] bool difficulty_available(CampaignDifficulty level) const noexcept;
    [[nodiscard]] std::string difficulty_reason(CampaignDifficulty level) const;
    bool select_difficulty(CampaignDifficulty level);

    // WGP-67: the entry of the selected row's set played as the selected faction, or null.
    [[nodiscard]] const CampaignFacts* variant() const noexcept;
    [[nodiscard]] bool can_start() const noexcept { return variant() != nullptr; }
    [[nodiscard]] std::string start_disabled_reason() const;
    // WGP-29: the entry to start, its faction and the difficulty. Empty while Start is off.
    [[nodiscard]] std::optional<CampaignStartRequest> start_request() const;

    // WGP-67: the options button needs a selected entry that supports custom settings, and a host
    // that applies them.
    [[nodiscard]] bool options_available() const noexcept;
    [[nodiscard]] std::string options_disabled_reason() const;
    struct TechOptions final {
        std::int64_t starting{}; // displayed coordinates (WGP-190)
        std::int64_t maximum{};
        bool custom{};
    };
    bool open_options();
    void close_options(bool accept);
    void default_options();
    bool select_starting_tech(std::int64_t displayed);
    bool select_maximum_tech(std::int64_t displayed);
    [[nodiscard]] bool options_open() const noexcept { return editing_.has_value(); }
    [[nodiscard]] TechOptions tech_options() const noexcept;
    [[nodiscard]] std::int64_t minimum_tech() const noexcept;
    [[nodiscard]] std::int64_t maximum_tech() const noexcept;

private:
    std::vector<CampaignFacts> campaigns_;
    std::vector<CampaignRow> rows_;
    std::vector<FactionChoice> factions_;
    CampaignHostSupport support_;
    std::optional<std::size_t> row_;
    std::optional<std::size_t> faction_;
    CampaignDifficulty difficulty_{CampaignDifficulty::normal};
    std::optional<TechOptions> accepted_{};
    std::optional<TechOptions> editing_{};
};

// The reason the single-player menu's Custom Campaign entry is off when the data lists no
// campaign, or the campaign data could not be read (`problem`, empty when it read).
[[nodiscard]] std::string campaign_entry_disabled_reason(std::size_t listed_rows, std::string_view problem);

} // namespace eawr::presentation::ui
