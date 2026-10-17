#pragma once

#include "ai_engine.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"

namespace eawr::script::foc::ai {
// WGA-56: providers restore world/targets before this owner, then funding and
// execution validate their saved references against the exact restored IDs.
// content_identity is the SHA-256 of the selected XML/Lua/content closure.
// A fresh Engine must use the same closure; pointers, derived producer caches,
// worker handles and diagnostic timelines are never archived.
class GoalStateContributor final : public galactic_ai::StateContributor {
public:
    GoalStateContributor(std::shared_ptr<Engine> engine, std::string content_identity)
        : engine_(std::move(engine)), content_identity_(std::move(content_identity)) { if (engine_) engine_->enable_native_records(); }
    [[nodiscard]] core::Result<galactic_ai::NativeState> capture() const override;
    [[nodiscard]] core::Result<void> restore(const galactic_ai::NativeState&) override;
    [[nodiscard]] core::Result<std::string> state_hash() const override;
    [[nodiscard]] core::Result<std::string> record_sync_hash(bool cold = false) const override;
    [[nodiscard]] std::size_t record_byte_upper_bound() const noexcept override { return engine_ ? engine_->native_records_.byte_size() + 1024 : 0; }
    [[nodiscard]] std::optional<std::uint64_t> state_revision() const noexcept override {
        return engine_ ? std::optional{engine_->native_state_revision()} : std::nullopt;
    }
    static constexpr std::string_view component = "galactic-goals";
private:
    friend class Engine;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> encode(Engine&) const;
    [[nodiscard]] core::Result<void> validate(const Engine&, const detail::WorldView* completed_view = nullptr) const;
    std::shared_ptr<Engine> engine_;
    std::string content_identity_;
};
} // namespace eawr::script::foc::ai
