#pragma once

// The galactic custom-campaign staging screen (MNU-34, WGP-28/29/40, WGP-65..67, WGP-73): the
// retail IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG built from the loaded catalogue under the front end's
// root, the campaign list, description, preview, difficulty radios and faction drop-down driven by
// the engine-free ui::CampaignSetupModel. The campaigns and factions are read from the loaded game
// data, never from a fixed list. Start produces a ui::CampaignStartRequest the front end hands to
// the host's campaign launcher; Cancel and Escape are the front end's Back.
//
// Not a Node: the front end owns it, shows and hides its frame and polls it once per frame, as it
// does for its menu dialogs.

#include "ui/dialog_builder.hpp"

#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/presentation/ui/campaign_setup.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/texture_rect.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::vfs { class Vfs; }
namespace eawr::data { class Catalog; }
namespace eawr::data::ui { struct DialogCatalog; }

namespace eawr::presentation::godot_backend {

class CampaignSetupScreen final {
public:
    // What the screen reads from the front end's load. The pointers outlive the screen.
    struct Resources final {
        const vfs::Vfs* filesystem{};
        const data::Catalog* catalog{};
        const data::ui::DialogCatalog* dialogs{};
        const presentation::ui::ThemeModel* theme_model{};
        data::ui::TextLookup* text{}; // null shows the keys as written
    };
    enum class Action : std::uint8_t { none, back, start };

    CampaignSetupScreen();
    ~CampaignSetupScreen();
    CampaignSetupScreen(const CampaignSetupScreen&) = delete;
    CampaignSetupScreen& operator=(const CampaignSetupScreen&) = delete;

    // Reads the campaign data and builds the dialog under `root`, hidden. A false return leaves the
    // screen unusable with the reason in failure(); the entry that opens it stays off.
    [[nodiscard]] bool build(godot::Control& root, const Resources& resources,
                             presentation::ui::CampaignHostSupport support);
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    // Rows the list shows; 0 keeps the single-player menu's entry off (data that lists none).
    [[nodiscard]] std::size_t listed_rows() const noexcept;
    // Opening resets the selection and the working choices (WGP-40, WGP-66).
    void open();
    void close();
    [[nodiscard]] bool visible() const noexcept;
    // One frame: reads the controls, updates the model and the dialog, reports a pressed Cancel or
    // Start (the request is then taken with take_start()).
    [[nodiscard]] Action poll();
    [[nodiscard]] std::optional<presentation::ui::CampaignStartRequest> take_start();

    // Test and report seams: the model, a gadget by its resource name, one line of facts.
    [[nodiscard]] const presentation::ui::CampaignSetupModel* model() const noexcept { return model_.get(); }
    [[nodiscard]] godot::Control* gadget(std::string_view id) const;
    [[nodiscard]] std::string summary() const;
    bool cancel_options(); // Escape dismisses edits before leaving staging

private:
    void rebuild_model_rows();
    void refresh();
    void refresh_preview();
    void refresh_options();
    void finish_options(bool accept);
    [[nodiscard]] std::string caption(std::string_view key) const;

    Resources resources_;
    std::string failure_;
    std::optional<data::galactic::GalacticData> tables_;
    std::unique_ptr<presentation::ui::CampaignSetupModel> model_;
    std::optional<BuiltDialog> built_;
    std::optional<BuiltDialog> advanced_{};
    double shown_starting_tech_{};
    double shown_maximum_tech_{};
    std::uint64_t shown_starting_revision_{};
    std::uint64_t shown_maximum_revision_{};
    godot::Control* list_{};
    godot::Control* faction_combo_{};
    godot::Control* description_{};
    godot::Control* start_{};
    godot::Control* options_{};
    godot::Control* cancel_{};
    godot::Control* official_{};
    godot::Control* custom_{};
    godot::Control* radios_[3]{};
    godot::Control* preview_{};
    godot::TextureRect* preview_image_{};
    godot::Control* preview_blips_{};
    std::string preview_set_;
    std::vector<std::string> blip_names_;
    std::optional<presentation::ui::CampaignStartRequest> pending_;
    // The control states read last frame, to find a change a click made.
    int shown_row_{-2};
    int shown_faction_{-2};
    bool radio_shown_[3]{};
    bool start_shown_{};
    bool opened_{};
};

} // namespace eawr::presentation::godot_backend
