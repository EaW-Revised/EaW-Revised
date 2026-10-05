#pragma once

#include "eawr/skirmish/setup.hpp"
#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <filesystem>
#include <memory>
#include <optional>

namespace eawr::presentation::godot_backend {

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
    void process();
    void request_start();
    [[nodiscard]] std::optional<skirmish::FixtureOptions> take_start();
    [[nodiscard]] std::shared_ptr<const BattleContent> content() const;
    [[nodiscard]] std::shared_ptr<GodotShaderCache> shaders() const;
    [[nodiscard]] godot::BaseButton* start_button() const;
    void show(std::string message = {});
    void hide();
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
