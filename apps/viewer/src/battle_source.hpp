#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace eawr::data { class Catalog; }
namespace eawr::vfs { class Vfs; }

namespace eawr::presentation::godot_backend {

namespace live_session { struct Options; }
struct BattleSourceState;

class BattlePreparation final {
public:
    explicit BattlePreparation(const live_session::Options& options);
    ~BattlePreparation();
    [[nodiscard]] BattleSourceState& state() noexcept;

private:
    std::unique_ptr<BattleSourceState> state_;
};

enum class BattleAiCapability { configurable, recorded, borrowed };

// Setup only: the source supplies the battle, its start-arrival metadata and AI
// installation through the owned preparation. The view binds art afterwards.
class BattleSource {
public:
    virtual ~BattleSource();
    [[nodiscard]] virtual BattleAiCapability ai_capability() const noexcept = 0;
    [[nodiscard]] virtual bool prepare(BattlePreparation& prepared, const vfs::Vfs& files,
        const data::Catalog& catalog, std::string_view map_path, std::string& failure) const = 0;
};

// Select the setup-time source using the existing CLI fixture spellings.
[[nodiscard]] const BattleSource* local_battle_source(std::string_view fixture);
[[nodiscard]] const BattleSource* battle_source(const live_session::Options& options);

} // namespace eawr::presentation::godot_backend
