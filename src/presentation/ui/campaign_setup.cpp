#include "eawr/presentation/ui/campaign_setup.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace eawr::presentation::ui {
namespace {

char fold(const char c) noexcept { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool same(const std::string_view a, const std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
        return fold(x) == fold(y);
    });
}

// Case-insensitive ordering of two titles (WGP-66).
bool before(const std::string_view a, const std::string_view b) noexcept {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](const char x, const char y) { return fold(x) < fold(y); });
}

} // namespace

std::string_view to_string(const CampaignDifficulty difficulty) noexcept {
    switch (difficulty) {
    case CampaignDifficulty::easy: return "easy";
    case CampaignDifficulty::normal: return "normal";
    case CampaignDifficulty::hard: return "hard";
    }
    return "normal";
}

CampaignSetupModel::CampaignSetupModel(std::vector<CampaignFacts> campaigns, std::vector<FactionFacts> factions,
                                       const CampaignHostSupport support)
    : campaigns_(std::move(campaigns)), support_(support) {
    // WGP-65: listed, not multiplayer, not a tutorial, and the first entry of each set.
    std::vector<std::string> sets;
    for (std::size_t index = 0; index < campaigns_.size(); ++index) {
        const CampaignFacts& entry = campaigns_[index];
        if (!entry.listed || entry.multiplayer || entry.tutorial) continue;
        // An entry without a set is its own set.
        const std::string& key = entry.campaign_set.empty() ? entry.name : entry.campaign_set;
        if (std::any_of(sets.begin(), sets.end(), [&](const std::string& seen) { return same(seen, key); })) continue;
        sets.push_back(key);
        rows_.push_back({index, entry.title.empty() ? entry.name : entry.title, entry.description});
    }
    // WGP-66: ascending Sort_Order, equal keys by title; a stable sort keeps the data's order beyond that.
    std::stable_sort(rows_.begin(), rows_.end(), [&](const CampaignRow& a, const CampaignRow& b) {
        const std::int64_t left = campaigns_[a.campaign].sort_order;
        const std::int64_t right = campaigns_[b.campaign].sort_order;
        if (left != right) return left < right;
        return before(a.title, b.title);
    });
    // WGP-40: every playable faction by display name (the drop-down is a sorted control).
    for (FactionFacts& faction : factions) {
        if (!faction.playable) continue;
        factions_.push_back({faction.name, faction.title.empty() ? faction.name : faction.title, faction.displayed_tech_adjustment});
    }
    std::stable_sort(factions_.begin(), factions_.end(), [](const FactionChoice& a, const FactionChoice& b) {
        return before(a.title, b.title);
    });
    if (!factions_.empty()) faction_ = factions_.size() - 1;
}

void CampaignSetupModel::reset() noexcept {
    accepted_.reset(); editing_.reset();
    row_.reset();
    faction_ = factions_.empty() ? std::nullopt : std::optional<std::size_t>(factions_.size() - 1);
    difficulty_ = CampaignDifficulty::normal;
}

void CampaignSetupModel::select_row(const std::optional<std::size_t> row) {
    accepted_.reset(); editing_.reset(); // WGP-67
    row_ = row && *row < rows_.size() ? row : std::nullopt;
}

const CampaignRow* CampaignSetupModel::selected() const noexcept { return row_ ? &rows_[*row_] : nullptr; }

std::string_view CampaignSetupModel::description() const noexcept {
    return row_ ? std::string_view(rows_[*row_].description) : std::string_view{};
}

void CampaignSetupModel::select_faction(const std::optional<std::size_t> faction) {
    accepted_.reset(); editing_.reset(); // WGP-67
    if (faction && *faction < factions_.size()) faction_ = faction;
}

bool CampaignSetupModel::difficulty_available(const CampaignDifficulty level) const noexcept {
    return level == CampaignDifficulty::normal || support_.difficulty;
}

std::string CampaignSetupModel::difficulty_reason(const CampaignDifficulty level) const {
    if (difficulty_available(level)) return {};
    return "The galactic mode starts every campaign at the authored normal difficulty; "
           "other levels are not applied yet.";
}

bool CampaignSetupModel::select_difficulty(const CampaignDifficulty level) {
    if (!difficulty_available(level)) return false;
    difficulty_ = level;
    return true;
}

const CampaignFacts* CampaignSetupModel::variant() const noexcept {
    if (!row_ || !faction_) return nullptr;
    const CampaignFacts& listed = campaigns_[rows_[*row_].campaign];
    const std::string& set = listed.campaign_set.empty() ? listed.name : listed.campaign_set;
    const std::string& faction = factions_[*faction_].name;
    for (const CampaignFacts& entry : campaigns_) {
        const std::string& key = entry.campaign_set.empty() ? entry.name : entry.campaign_set;
        if (same(key, set) && same(entry.faction, faction)) return &entry;
    }
    return nullptr;
}

std::string CampaignSetupModel::start_disabled_reason() const {
    if (variant() != nullptr) return {};
    if (!row_) return "Choose a campaign first.";
    if (!faction_) return "No playable faction is available.";
    return "This campaign cannot be played as " + factions_[*faction_].title + ".";
}

std::optional<CampaignStartRequest> CampaignSetupModel::start_request() const {
    const CampaignFacts* found = variant();
    if (found == nullptr) return std::nullopt;
    CampaignStartRequest request;
    request.kind = CampaignStartRequest::Kind::custom;
    request.campaign = found->name;
    request.faction = factions_[*faction_].name;
    request.difficulty = std::string(to_string(difficulty_));
    request.custom_options = accepted_ && accepted_->custom;
    if (request.custom_options) request.economy = CampaignStartRequest::Economy{
        found->starting_credits, accepted_->starting, accepted_->maximum};
    return request;
}

bool CampaignSetupModel::options_available() const noexcept {
    const CampaignFacts* found = variant();
    return support_.custom_options && found != nullptr && found->supports_custom_settings
        && found->minimum_custom_tech <= found->maximum_custom_tech;
}

std::int64_t CampaignSetupModel::minimum_tech() const noexcept {
    return variant() ? variant()->minimum_custom_tech : 1;
}
std::int64_t CampaignSetupModel::maximum_tech() const noexcept {
    return variant() ? variant()->maximum_custom_tech : 5;
}
CampaignSetupModel::TechOptions CampaignSetupModel::tech_options() const noexcept {
    if (editing_) return *editing_;
    if (accepted_) return *accepted_;
    const auto* found = variant();
    if (!found || !faction_) return {};
    const auto adjustment = factions_[*faction_].displayed_tech_adjustment;
    return {found->starting_tech + adjustment, found->maximum_tech + adjustment, false};
}
bool CampaignSetupModel::open_options() {
    if (!options_available()) return false;
    editing_ = tech_options();
    return true;
}
void CampaignSetupModel::close_options(const bool accept) {
    if (accept && editing_) accepted_ = editing_;
    editing_.reset();
}
void CampaignSetupModel::default_options() {
    if (!editing_) return;
    const auto* found = variant();
    if (!found || !faction_) return;
    const auto adjustment = factions_[*faction_].displayed_tech_adjustment;
    editing_ = TechOptions{found->starting_tech + adjustment, found->maximum_tech + adjustment, false};
}
bool CampaignSetupModel::select_starting_tech(const std::int64_t displayed) {
    if (!editing_) return false;
    const auto upper = std::clamp(editing_->maximum, minimum_tech(), maximum_tech());
    editing_->starting = std::clamp(displayed, minimum_tech(), upper);
    editing_->maximum = upper;
    editing_->custom = true;
    return true;
}
bool CampaignSetupModel::select_maximum_tech(const std::int64_t displayed) {
    if (!editing_) return false;
    editing_->starting = std::clamp(editing_->starting, minimum_tech(), maximum_tech());
    editing_->maximum = std::clamp(displayed, editing_->starting, maximum_tech());
    editing_->custom = true;
    return true;
}

std::string CampaignSetupModel::options_disabled_reason() const {
    if (options_available()) return {};
    if (!support_.custom_options)
        return "The galactic mode does not apply custom campaign options (credits, tech levels, auto-resolve) yet.";
    if (variant() == nullptr) return start_disabled_reason();
    return "This campaign does not support custom settings.";
}

std::string campaign_entry_disabled_reason(const std::size_t listed_rows, const std::string_view problem) {
    if (!problem.empty()) return "The campaign data could not be read: " + std::string(problem);
    if (listed_rows == 0) return "The loaded data lists no galactic conquest campaign.";
    return {};
}

} // namespace eawr::presentation::ui
