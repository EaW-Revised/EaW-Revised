#pragma once

#include "ai_engine.hpp"
#include "eawr/core/record_digest.hpp"
#include <compare>
#include <mutex>

namespace eawr::script::foc::ai {
class GalacticWorld;
// WGA-426..429/461: authored hints and retained additive values are separate.
struct GalacticMarkupHints {
    std::string set;
    std::map<std::string, Real, std::less<>> defaults;
    std::map<std::string, std::map<std::string, Real, std::less<>>, std::less<>> planets;
    friend bool operator==(const GalacticMarkupHints&, const GalacticMarkupHints&) = default;
};
struct GalacticMarkupKey {
    tactical::PlayerId observer{};
    bool player_target{};
    std::uint64_t target{};
    friend auto operator<=>(const GalacticMarkupKey&, const GalacticMarkupKey&) = default;
};
struct GalacticMarkupHandle {
    Real value{};
    std::vector<GalacticMarkupKey> targets; // repetitions preserve authored application order
    friend bool operator==(const GalacticMarkupHandle&, const GalacticMarkupHandle&) = default;
};
struct GalacticMarkupState {
    std::map<tactical::PlayerId, GalacticMarkupHints> hints;
    std::map<GalacticMarkupKey, Real> values;
    std::map<std::uint64_t, GalacticMarkupHandle> handles;
    std::uint64_t next_handle{1};
    friend bool operator==(const GalacticMarkupState&, const GalacticMarkupState&) = default;
};
inline constexpr std::uint32_t handle_galactic_markup = 17;
class GalacticMarkup final {
public:
    GalacticMarkup(std::map<tactical::PlayerId, GalacticMarkupHints>, std::set<tactical::PlayerId>,
        std::set<tactical::PlayerId>, std::set<sim::galactic::PlanetId>);
    [[nodiscard]] Real value(const GalacticMarkupKey&) const;
    [[nodiscard]] std::optional<Real> hint(tactical::PlayerId, std::string_view planet, std::string_view name) const;
    [[nodiscard]] GalacticMarkupState state() const;
    [[nodiscard]] std::shared_ptr<const GalacticMarkupState> snapshot() const;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::string record_sync_hash(bool cold = false) const;
    [[nodiscard]] std::size_t record_byte_upper_bound() const noexcept;
    [[nodiscard]] bool is_ai_player(tactical::PlayerId player) const { return ai_players_.contains(player); }
    [[nodiscard]] core::Result<void> validate(const GalacticMarkupState&) const;
    void replace(GalacticMarkupState);
    [[nodiscard]] core::Result<std::uint64_t> apply(GalacticMarkupState&, tactical::PlayerId,
        std::span<const GalacticMarkupKey>, Real, std::optional<std::uint64_t>) const;
    [[nodiscard]] bool destroy(GalacticMarkupState&, std::uint64_t) const;
    void retain_planets(std::span<const sim::galactic::Planet>);
    [[nodiscard]] std::vector<std::uint8_t> encode() const;
    [[nodiscard]] core::Result<GalacticMarkupState> decode(std::span<const std::uint8_t>) const;
    [[nodiscard]] core::Result<authoritative::ValueList> query_with_state(const GalacticMarkupState&,
        const std::function<core::Result<authoritative::ValueList>()>&) const;
private:
    [[nodiscard]] bool alive(const GalacticMarkupKey&) const;
    void subtract_contribution(GalacticMarkupState&, const GalacticMarkupHandle&) const;
    [[nodiscard]] core::RecordDigest rebuild_records() const;
    void update_records(const GalacticMarkupState* before);
    core::RecordDigest record_digest_;
    mutable std::mutex mutex_;
    GalacticMarkupState state_;
    mutable std::shared_ptr<const GalacticMarkupState> snapshot_;
    std::set<tactical::PlayerId> players_, ai_players_;
    std::set<sim::galactic::PlanetId> planets_;
    std::uint64_t revision_{};
};
using GalacticMarkupPath = std::function<core::Result<std::vector<sim::galactic::PlanetId>>(
    tactical::PlayerId, sim::galactic::PlanetId, sim::galactic::PlanetId)>;
[[nodiscard]] core::Result<void> register_galactic_markup_bindings(authoritative::ScriptScheduler&,
    std::shared_ptr<const GalacticWorld>, std::shared_ptr<const Engine>, GalacticMarkupPath);
[[nodiscard]] std::shared_ptr<authoritative::ImmediateEffectHost> markup_immediate_host(
    std::shared_ptr<authoritative::ImmediateEffectHost>, std::shared_ptr<GalacticMarkup>,
    std::shared_ptr<const GalacticWorld> = {}, std::shared_ptr<const Engine> = {});
} // namespace eawr::script::foc::ai
