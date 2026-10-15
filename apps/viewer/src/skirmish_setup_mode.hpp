#pragma once

#include "eawr/skirmish/setup.hpp"
#include "eawr/presentation/ui/settings.hpp"
#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <filesystem>
#include <memory>
#include <optional>

namespace eawr::vfs { class Vfs; }
namespace eawr::data { class Catalog; }
namespace eawr::data::ui { struct DialogCatalog; }
namespace eawr::presentation::ui { struct ThemeModel; }

namespace eawr::presentation::godot_backend {

class LanMatch;
class EawrLanLobby;
class FontProvider;
class UiTextures;
struct BattleContent;
class GodotShaderCache;

class SkirmishSetupMode final {
public:
    struct Options final {
        std::filesystem::path game_root, mod_root, report_path, capture_path;
        std::string profile;
        bool cache_shaders{true};
    };
    explicit SkirmishSetupMode(Options options);
    ~SkirmishSetupMode();
    [[nodiscard]] bool ready(godot::Node3D& host);
    [[nodiscard]] const std::string& failure() const;
    void process();
    void request_start();
    [[nodiscard]] std::optional<skirmish::FixtureOptions> take_start();
    // A start was asked for and the host has not taken it yet (the front end stops the attract then).
    [[nodiscard]] bool start_pending() const;
    [[nodiscard]] std::shared_ptr<const BattleContent> content() const;
    // The listed map at a logical path (any filter), or null.
    [[nodiscard]] const skirmish::SetupMap* map(const std::string& path) const;
    [[nodiscard]] std::shared_ptr<GodotShaderCache> shaders() const;
    [[nodiscard]] godot::BaseButton* start_button() const;
    // The LAN connection's Host button, which only shows once the LAN connection is chosen (the GPU
    // contract's text-sharpness probe: a part that was hidden while the window was sized).
    [[nodiscard]] godot::BaseButton* lan_host_button() const;
    [[nodiscard]] std::shared_ptr<LanMatch> lan_match() const;
    void show(std::string message = {});
    void hide();
    // MNU-21: the staging's Cancel button. A shell that owns the screen calls route_cancel() once and then
    // polls take_cancel(); without a shell Cancel quits the stand-alone screen, as closing its window does.
    void route_cancel();
    [[nodiscard]] bool take_cancel();

    // What the front end shell (front_end_mode.hpp) shares with this screen: the mounted data, the
    // skin and the theme it built, so the main menu is drawn from the same load. All pointers are
    // null until ready() succeeded and stay valid for this object's life.
    struct UiResources final {
        const vfs::Vfs* filesystem{};
        const data::Catalog* catalog{};
        const data::ui::DialogCatalog* dialogs{};
        const presentation::ui::ThemeModel* theme_model{};
        godot::Ref<godot::Theme> theme;
        const FontProvider* fonts{};
    };
    [[nodiscard]] UiResources ui_resources() const;
    // The shell opens the screen at the single-player or the LAN connection and closes it again.
    void enter(bool lan);
    void enter_online(presentation::ui::NetworkSettings settings);
    [[nodiscard]] EawrLanLobby* online_lobby() const;
    void leave();
    [[nodiscard]] bool visible() const;
    [[nodiscard]] bool lan_selected() const;
    // MAT-14: the shell says whether the demo battle runs behind the screen. The single-player
    // staging then leaves its backing clear so the battle shows; the LAN connection (a multiplayer
    // menu, which retail draws over a hidden battle) and a screen without a battle stay opaque.
    void scene_behind(bool running);
    [[nodiscard]] bool scene_visible() const;
    // The GPU probe: hides the dialog's own controls so the pixels behind them can be read.
    void set_content_visible(bool visible);
    // The shell's Back (and Escape) is the staging's Cancel: enable_back routes it like route_cancel,
    // back_button is the Cancel button and take_back the same request as take_cancel.
    void enable_back(const std::string& caption);
    [[nodiscard]] godot::BaseButton* back_button() const;
    [[nodiscard]] bool take_back();
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
