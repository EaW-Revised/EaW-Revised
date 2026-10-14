#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace eawr::platform {

// WGP-97/111: values copied from the local scoring and authored faction providers
// before their owners are released. A missing provider is distinct from a stat
// provider which reports an absent control value.
struct CampaignEndInputs {
    std::string title_key;
    std::array<std::optional<double>, 3> efficiencies;
    std::string victory_text, defeat_text;
    std::string space_win_image, land_win_image, space_lose_image, land_lose_image;
    std::string generic_win_movie, superweapon_win_movie, finale_movie, second_finale_movie, post_credits_movie;
    bool victory_movies_enabled{}; // WGP-105: independently enabled, initially false.
    // GHUD-S61: why the title/efficiencies are absent when a provider could not produce them.
    std::string scoring_seam;
};
enum class CampaignEndRoute { score, multiplayer_quit };
enum class CampaignEndingMode { solo, lan, other_multiplayer };
struct CampaignEndView {
    sim::galactic::GalacticVictoryCondition condition;
    std::uint32_t local_player{};
    std::string winner_faction;
    bool local_won{}, explicit_quit{}, tutorial_queued{};
    CampaignEndingMode ending_mode{CampaignEndingMode::solo};
    CampaignEndRoute route{CampaignEndRoute::score};
    std::optional<CampaignEndInputs> inputs;
    std::string provider_seam;
};
inline std::string campaign_efficiency_text(std::optional<double> value) {
    // WGP-97: missing/non-numeric is visibly different from present zero.
    if (!value || !std::isfinite(*value)) return "0%";
    return std::to_string(static_cast<int>(std::clamp(*value * 100.0, 0.0, 100.0)));
}
inline std::string campaign_heading(const CampaignEndView& view) {
    return view.local_won ? "TEXT_VICTORY" : "TEXT_DEFEAT";
}
inline std::string campaign_score_text(const CampaignEndView& view) {
    if (!view.inputs) return {};
    return view.local_won ? view.inputs->victory_text : view.inputs->defeat_text;
}
inline std::string campaign_score_picture(const CampaignEndView& view, bool space_draw) {
    // WGP-38: the caller supplies its presentation-stream 50/50 draw.
    if (!view.inputs) return {};
    const auto& in = *view.inputs;
    return view.local_won ? (space_draw ? in.space_win_image : in.land_win_image)
        : (space_draw ? in.space_lose_image : in.land_lose_image);
}

enum class CampaignScoreDestination { none, main_menu, lan_lobby };
struct CampaignMovieRequest {
    std::string name;
    bool looping{}, hide_game{true};
    std::uint64_t token{};
};
// WGS-116..119, WGP-105..110: presentation callback ownership only. Movie opening,
// authored Cannot_Skip and the rendering-device gate are supplied by their owners.
class CampaignEndPresentation final {
public:
    explicit CampaignEndPresentation(CampaignEndView view) : view_(std::move(view)) {
        active_ = view_.route == CampaignEndRoute::score;
        if (!active_ || !view_.inputs) return;
        const auto& in = *view_.inputs;
        if (in.victory_movies_enabled && view_.ending_mode == CampaignEndingMode::solo) {
            const auto kind = view_.condition.kind;
            const bool superweapon = kind == sim::galactic::VictoryKind::superweapon_last_planet
                || kind == sim::galactic::VictoryKind::superweapon_kill_leader
                || kind == sim::galactic::VictoryKind::destroy_superweapon;
            initial_ = superweapon ? in.superweapon_win_movie : in.generic_win_movie;
            first_ = in.finale_movie; second_ = in.second_finale_movie; post_ = in.post_credits_movie;
        }
        stage_ = Stage::initial;
        request(initial_);
    }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const std::optional<CampaignMovieRequest>& movie_request() const noexcept { return request_; }
    [[nodiscard]] bool take_stop_music_and_speech() noexcept { return std::exchange(stop_audio_, false); }
    // WGP-108: initialized failure invokes the continuation synchronously;
    // uninitialized failure supplies no invented callback.
    void movie_open_result(std::uint64_t token, bool opened, bool scene_initialized) {
        if (!request_ || request_->token != token) return;
        if (!opened && scene_initialized) movie_stopped(token);
    }
    void movie_stopped(std::uint64_t token) {
        if (!request_ || request_->token != token) return;
        request_.reset(); // WGP-109: clear the previous continuation first.
        if (stage_ == Stage::initial) {
            stop_audio_ = true;
            if (view_.ending_mode != CampaignEndingMode::solo) { stage_ = Stage::done; return; }
            stage_ = Stage::first; request(first_);
        } else if (stage_ == Stage::first && !second_.empty()) {
            stage_ = Stage::second; request(second_);
        } else if (stage_ == Stage::first || stage_ == Stage::second) {
            stage_ = Stage::credits; request("Credits", true);
        } else stage_ = Stage::done; // Credits/post have no local continuation.
    }
    // WGP-109: a movie Escape/Space is consumed even when Cannot_Skip forbids stop.
    [[nodiscard]] bool movie_skip_key(bool escape_or_space, bool playing, bool hides_game,
        bool main_menu_active, bool cannot_skip) {
        if (!escape_or_space || !playing || !hides_game || main_menu_active) return false;
        if (!cannot_skip && request_) movie_stopped(request_->token);
        return true;
    }
    void credits_completed(std::uint64_t token, bool rendering_operational) {
        if (!rendering_operational || stage_ != Stage::credits || !request_ || request_->token != token) return;
        request_.reset(); stage_ = Stage::done;
        if (!post_.empty()) { stage_ = Stage::post; request(post_); }
    }
    [[nodiscard]] CampaignScoreDestination score_exit(bool topmost, bool escape_or_quit) {
        if (!active_ || !topmost || !escape_or_quit) return CampaignScoreDestination::none;
        active_ = false;
        initial_.clear(); first_.clear(); second_.clear(); post_.clear();
        if (!view_.explicit_quit) return CampaignScoreDestination::main_menu;
        if (view_.ending_mode == CampaignEndingMode::lan) return CampaignScoreDestination::lan_lobby;
        return view_.tutorial_queued ? CampaignScoreDestination::none : CampaignScoreDestination::main_menu;
    }
private:
    void request(std::string name, bool looping = false) {
        request_ = CampaignMovieRequest{std::move(name), looping, true, ++token_};
    }
    enum class Stage { initial, first, second, credits, post, done };
    CampaignEndView view_;
    bool active_{}, stop_audio_{};
    Stage stage_{Stage::done};
    std::string initial_, first_, second_, post_;
    std::optional<CampaignMovieRequest> request_;
    std::uint64_t token_{};
};
} // namespace eawr::platform
