#pragma once
#include "campaign_score_model.hpp"
#include "ui/dialog_builder.hpp"
#include "ui/battle_overlay.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// GHUD-S48/H120 presentation adapter: the full-screen campaign score dialog built
// from the mounted IDD_CAMPAIGN_SCORE_DIALOG. It copies the end view's facts into
// the authored controls and forwards the button/Escape to #2837's presentation
// callback; it owns no simulation state and no menu (the owner of the main menu
// consumes the published destination).
class CampaignScoreDialog final {
public:
    struct Style {
        std::function<BuiltDialog(std::string_view, godot::Vector2)> dialog;
        std::function<std::string(std::string_view)> text;
        std::function<godot::Ref<godot::Texture2D>(std::string_view)> texture;
    };
    // `space_draw` is the presentation-only WGP-38 50/50 draw; `exit` receives the
    // destination chosen by CampaignEndPresentation::score_exit.
    CampaignScoreDialog(godot::Control& parent, Style style, std::function<bool()> space_draw,
        viewer::galactic::CampaignScoreExit exit);
    ~CampaignScoreDialog();
    // Publishes the immutable end view once; a null or hidden-route view hides it.
    void present(std::shared_ptr<const platform::CampaignEndView> view);
    void service_layout();
    [[nodiscard]] bool visible() const noexcept { return shown_; }
    // Button and Escape share this path (WGP-111, WGS-116/117).
    void activate_quit(bool escape = false);
    // Movie continuation is H121/GHUD-S49's: the report names what it would run.
    [[nodiscard]] std::string report_json() const;
    [[nodiscard]] platform::CampaignScoreDestination destination() const noexcept { return destination_; }

private:
    void rebuild();
    void refresh();
    Style style_;
    std::function<bool()> space_draw_;
    viewer::galactic::CampaignScoreExit exit_;
    godot::Control* root_{};
    std::optional<BuiltDialog> dialog_;
    std::vector<EawrOverlayButton*> relays_;
    std::shared_ptr<const platform::CampaignEndView> view_;
    std::optional<platform::CampaignEndPresentation> presentation_;
    viewer::galactic::CampaignScoreCaptions captions_;
    godot::Vector2 size_;
    platform::CampaignScoreDestination destination_{platform::CampaignScoreDestination::none};
    bool shown_{}, space_{}, drawn_{}, exited_{};
    std::uint64_t rebuilds_{}, exits_{}, escapes_{};
};

} // namespace eawr::presentation::godot_backend
