#include "galactic_ai_services.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/sync_digest.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"
#include "../script/foc/galactic_ai_funding.hpp"

#include <exception>
#include <cstdlib>
#include <cassert>

namespace eawr::campaign {
namespace ai = script::foc::galactic_ai;
namespace {
template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic error; error.code = "EAWR-GCAI-STATE-0001"; error.message = std::move(message);
    return core::Result<T>::failure(std::move(error));
}
void put(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
}
constexpr std::array<std::string_view, 6> components{
    "galactic-ai-lifecycle", "GALACTIC_PERCEPTION", "galactic-goals",
    "funding", "galactic-execution", "galactic-production"};
bool verification_requested() {
#ifdef _MSC_VER
    char* value{}; std::size_t size{};
    if (_dupenv_s(&value, &size, "EAWR_VERIFY_GALACTIC_HASH") || !value) return false;
    const bool enabled = std::string_view(value) == "1";
    std::free(value); return enabled;
#else
    const auto* value = std::getenv("EAWR_VERIFY_GALACTIC_HASH");
    return value && std::string_view(value) == "1";
#endif
}
class FundingContributor final : public ai::StateContributor {
public:
    FundingContributor(std::shared_ptr<script::foc::ai::GalacticFunding> funding,
        decltype(GalacticAiContributors::validate_funding) validator,
        std::function<core::Result<void>()> publish)
        : funding_(std::move(funding)), validator_(std::move(validator)), publish_(std::move(publish)) {}
    std::optional<std::uint64_t> state_revision() const noexcept override { return funding_->state_revision(); }
    core::Result<void> validate_current() const { return validate(funding_->state()); }
    core::Result<ai::NativeState> capture() const override {
        if (auto valid = validate(funding_->state()); !valid) return core::Result<ai::NativeState>::failure(valid.error());
        auto bytes = ai::encode_funding_state(funding_->state());
        if (!bytes) return core::Result<ai::NativeState>::failure(bytes.error());
        ai::NativeState state; state.components.emplace("funding", std::move(bytes).value());
        return core::Result<ai::NativeState>::success(std::move(state));
    }
    core::Result<void> restore(const ai::NativeState& state) override {
        const auto at = state.components.find("funding");
        if (state.version != ai::native_state_version || at == state.components.end())
            return fail<void>("required funding component is missing");
        auto decoded = ai::decode_funding_state(at->second);
        if (!decoded) return core::Result<void>::failure(decoded.error());
        if (auto valid = validate(decoded.value()); !valid) return valid;
        if (auto restored = funding_->restore(std::move(decoded).value()); !restored) return restored;
        return publish_ ? publish_() : core::Result<void>::success();
    }
    core::Result<std::string> state_hash() const override {
        auto saved = capture();
        if (!saved) return core::Result<std::string>::failure(saved.error());
        return ai::native_state_hash(saved.value());
    }
private:
    core::Result<void> validate(const ai::FundingState& state) const {
        if (validator_) return validator_(state);
        for (const auto& [player, ledger] : state) {
            static_cast<void>(player);
            if (!ledger.goals.empty()) return fail<void>("funding goal state requires its native identity validator");
        }
        return core::Result<void>::success();
    }
    std::shared_ptr<script::foc::ai::GalacticFunding> funding_;
    decltype(GalacticAiContributors::validate_funding) validator_;
    std::function<core::Result<void>()> publish_;
};
// Contributors run on the completed owner barrier. Convert a thrown consumer
// error to a refusal so a partial restore still takes the rollback path.
core::Result<void> restore_owner(ai::StateContributor& owner, const ai::NativeState& state) {
    try { return owner.restore(state); }
    catch (const std::exception& error) { return fail<void>("native restore threw: " + std::string(error.what())); }
    catch (...) { return fail<void>("native restore threw"); }
}
core::Result<void> publish_restored_copy(const std::function<core::Result<void>()>& publish) {
    if (!publish) return core::Result<void>::success();
    try { return publish(); }
    catch (const std::exception& error) { return fail<void>("native derived publication threw: " + std::string(error.what())); }
    catch (...) { return fail<void>("native derived publication threw"); }
}
} // namespace
ComposedGalacticAiState::ComposedGalacticAiState(std::array<Contributor, 6> owners)
    : owners_(std::move(owners)), verify_hash_(verification_requested()) {}
core::Result<std::shared_ptr<ComposedGalacticAiState>> ComposedGalacticAiState::create(GalacticAiContributors owners) {
    using Result = core::Result<std::shared_ptr<ComposedGalacticAiState>>;
    if (!owners.lifecycle || !owners.perception || !owners.goals || !owners.funding || !owners.execution || !owners.production)
        return fail<std::shared_ptr<ComposedGalacticAiState>>("composition requires all six native owners");
    auto state = std::shared_ptr<ComposedGalacticAiState>(new ComposedGalacticAiState({
        std::move(owners.lifecycle), std::move(owners.perception), std::move(owners.goals),
        std::make_shared<FundingContributor>(std::move(owners.funding), std::move(owners.validate_funding), std::move(owners.after_funding_restore)), std::move(owners.execution), std::move(owners.production)}));
    for (auto& [name, owner] : owners.external) {
        if (name.empty() || !owner || std::find(components.begin(), components.end(), name) != components.end())
            return fail<std::shared_ptr<ComposedGalacticAiState>>("invalid external native owner identity");
        state->external_cache_.emplace(name, OwnerCache{});
    }
    state->external_ = std::move(owners.external);
    state->after_restore_ = std::move(owners.after_restore);
    auto saved = state->capture();
    if (!saved) return Result::failure(saved.error());
    return Result::success(std::move(state));
}
core::Result<ai::NativeState> ComposedGalacticAiState::capture() const {
    if (failure_) return core::Result<ai::NativeState>::failure(*failure_);
    ai::NativeState state;
    for (std::size_t i = 0; i < owners_.size(); ++i) {
        auto saved = owners_[i]->capture();
        if (!saved) return core::Result<ai::NativeState>::failure(saved.error());
        if (saved.value().version != ai::native_state_version || saved.value().components.size() != 1
            || !saved.value().components.contains(components[i]))
            return fail<ai::NativeState>("native owner has an unexpected component: " + std::string(components[i]));
        state.components.merge(saved.value().components);
    }
    for (const auto& [name, owner] : external_) {
        auto saved = owner->capture();
        if (!saved) return core::Result<ai::NativeState>::failure(saved.error());
        if (saved.value().version != ai::native_state_version || saved.value().components.size() != 1
            || !saved.value().components.contains(name)) return fail<ai::NativeState>("unexpected external native component: " + name);
        state.components.merge(saved.value().components);
    }
    auto encoded = ai::encode_native_state(state);
    if (!encoded) return core::Result<ai::NativeState>::failure(encoded.error());
    return core::Result<ai::NativeState>::success(std::move(state));
}
core::Result<void> ComposedGalacticAiState::restore(const ai::NativeState& state) {
    if (failure_) return core::Result<void>::failure(*failure_);
    if (auto encoded = ai::encode_native_state(state); !encoded) return core::Result<void>::failure(encoded.error());
    if (state.components.size() != components.size() + external_.size()) return fail<void>("composed native component set differs");
    for (const auto name : components) if (!state.components.contains(name)) return fail<void>("composed native component missing: " + std::string(name));
    for (const auto& [name, owner] : external_) {
        static_cast<void>(owner);
        if (!state.components.contains(name)) return fail<void>("external native component missing: " + name);
    }
    auto previous = capture();
    if (!previous) return core::Result<void>::failure(previous.error());
    // Includes failed/throwing restore and rollback, even for external owners.
    for (auto& cache : owner_cache_) cache.revision.reset();
    for (auto& [name, cache] : external_cache_) { static_cast<void>(name); cache.revision.reset(); }
    for (auto& [name, cache] : sync_cache_) { static_cast<void>(name); cache.revision.reset(); }
    sync_validation_pending_ = true;
    std::vector<Contributor> restore_order(owners_.begin(), owners_.begin() + 4);
    for (const auto& [name, owner] : external_) { static_cast<void>(name); restore_order.push_back(owner); }
    restore_order.insert(restore_order.end(), owners_.begin() + 4, owners_.end());
    auto restored = core::Result<void>::success();
    for (const auto& owner : restore_order) {
        restored = restore_owner(*owner, state);
        if (!restored) break;
    }
    if (restored) restored = publish_restored_copy(after_restore_);
    if (!restored) {
        // Even a rejecting owner or publisher might have made a partial write.
        // Reconstruct every prior dependency in the same forward order.
        for (const auto& old_owner : restore_order) {
            if (auto rollback = restore_owner(*old_owner, previous.value()); !rollback) {
                failure_ = fail<void>("native restore rollback failed: " + rollback.error().message).error();
                return core::Result<void>::failure(*failure_);
            }
        }
        if (auto rollback = publish_restored_copy(after_restore_); !rollback) {
            failure_ = fail<void>("native derived rollback publication failed: " + rollback.error().message).error();
            return core::Result<void>::failure(*failure_);
        }
        return restored;
    }
    return core::Result<void>::success();
}
core::Result<std::string> ComposedGalacticAiState::sync_hash() const {
    gc_profile::Scope profile_scope(gc_profile::Phase::native_digest);
    if (failure_) return core::Result<std::string>::failure(*failure_);
    ++digest_requests_;
    ai::NativeDigests digests;
    std::size_t total_bytes{};
    const auto append = [&](std::string name, const Contributor& owner) -> core::Result<void> {
        auto& cache = sync_cache_[name];
        const auto revision = owner->state_revision();
        if (!revision || cache.revision != revision || cache.digest.empty()) {
            auto saved = owner->capture();
            if (!saved) return core::Result<void>::failure(saved.error());
            if (saved.value().version != ai::native_state_version || saved.value().components.size() != 1
                || !saved.value().components.contains(name)) return fail<void>("unexpected sync component: " + name);
            auto bytes = std::move(saved.value().components.begin()->second);
            if (bytes.empty() || bytes.size() > ai::native_state_max_bytes) return fail<void>("invalid sync component byte count");
            if ((name == components[2] || name == components[3]) && cache.bytes != bytes) sync_validation_pending_ = true;
            cache.digest = core::sync_state_digest(bytes); cache.bytes = std::move(bytes); cache.revision = revision;
            ++cache.encodes;
        }
        if (cache.bytes.size() > ai::native_state_max_bytes - total_bytes) return fail<void>("native sync byte budget exceeded");
        total_bytes += cache.bytes.size();
        digests.emplace(std::move(name), cache.digest);
        return core::Result<void>::success();
    };
    for (std::size_t i = 0; i < owners_.size(); ++i)
        if (auto copied = append(std::string(components[i]), owners_[i]); !copied) return core::Result<std::string>::failure(copied.error());
    for (const auto& [name, owner] : external_)
        if (auto copied = append(name, owner); !copied) return core::Result<std::string>::failure(copied.error());
    if (sync_validation_pending_) {
        if (auto valid = static_cast<const FundingContributor&>(*owners_[3]).validate_current(); !valid)
            return core::Result<std::string>::failure(valid.error());
        sync_validation_pending_ = false;
    }
    const auto result = ai::compose_native_sync_digests(digests);
    if (cached_sync_digest_ != result) { cached_sync_digest_ = result; ++sync_generations_; }
    if (full_verification_enabled()) {
        auto saved = capture(); if (!saved) return core::Result<std::string>::failure(saved.error());
        auto oracle = ai::native_sync_state_hash(saved.value()); if (!oracle) return oracle;
        if (result != oracle.value()) return fail<std::string>("incremental native sync digest differs from full capture");
        // The expensive canonical native oracle stays behind the existing switch.
        if (auto full = state_hash(); !full) return full;
    }
    return core::Result<std::string>::success(result);
}
core::Result<std::string> ComposedGalacticAiState::state_hash() const {
    ++digest_requests_;
    if (failure_) return core::Result<std::string>::failure(*failure_);
    bool changed = cached_digest_.empty();
    for (std::size_t i = 0; i < owners_.size(); ++i) {
        auto& cache = owner_cache_[i];
        const auto revision = owners_[i]->state_revision();
        if (revision && cache.revision == revision && !cache.digest.empty()) continue;
        auto saved = owners_[i]->capture();
        if (!saved) return core::Result<std::string>::failure(saved.error());
        if (saved.value().version != ai::native_state_version || saved.value().components.size() != 1
            || !saved.value().components.contains(components[i]))
            return fail<std::string>("native owner has an unexpected component: " + std::string(components[i]));
        if (auto valid = ai::encode_native_state(saved.value()); !valid) return core::Result<std::string>::failure(valid.error());
        auto bytes = std::move(saved.value().components.begin()->second);
        ++owner_encodes_[i];
        if (cache.digest.empty() || cache.bytes != bytes) {
            cache.digest = core::sha256_hex(bytes); cache.bytes = std::move(bytes); changed = true;
            // A later contributor may refuse: do not leave an old composition
            // paired with already updated owner caches on the next request.
            cached_digest_.clear();
        }
        cache.revision = revision;
    }
    for (const auto& [name, owner] : external_) {
        auto& cache = external_cache_.at(name);
        const auto revision = owner->state_revision();
        if (revision && cache.revision == revision && !cache.digest.empty()) continue;
        auto saved = owner->capture();
        if (!saved) return core::Result<std::string>::failure(saved.error());
        if (saved.value().version != ai::native_state_version || saved.value().components.size() != 1
            || !saved.value().components.contains(name)) return fail<std::string>("unexpected external native component: " + name);
        if (auto valid = ai::encode_native_state(saved.value()); !valid) return core::Result<std::string>::failure(valid.error());
        auto bytes = std::move(saved.value().components.begin()->second);
        ++cache.encodes;
        if (cache.digest.empty() || cache.bytes != bytes) {
            cache.digest = core::sha256_hex(bytes); cache.bytes = std::move(bytes); changed = true; cached_digest_.clear();
        }
        cache.revision = revision;
    }
    std::size_t total_bytes = 4 + std::string_view("eawr-galactic-ai-native").size() + 4 + 8;
    for (std::size_t i = 0; i < owners_.size(); ++i) {
        const auto size = owner_cache_[i].bytes.size() + components[i].size() + 12;
        if (size > ai::native_state_max_bytes - total_bytes) return fail<std::string>("native AI byte budget exceeded");
        total_bytes += size;
    }
    for (const auto& [name, cache] : external_cache_) {
        const auto size = cache.bytes.size() + name.size() + 12;
        if (size > ai::native_state_max_bytes - total_bytes) return fail<std::string>("native AI byte budget exceeded");
        total_bytes += size;
    }
    if (changed) {
        // Funding references consume restored/live goals; a goal change must
        // still validate them even when the ledger payload itself is unchanged.
        if (auto valid = static_cast<const FundingContributor&>(*owners_[3]).validate_current(); !valid)
            return core::Result<std::string>::failure(valid.error());
        ai::NativeDigests digests;
        for (std::size_t i = 0; i < owners_.size(); ++i) digests.emplace(components[i], owner_cache_[i].digest);
        for (const auto& [name, cache] : external_cache_) digests.emplace(name, cache.digest);
        cached_digest_ = ai::compose_native_digests(digests);
        ++digest_generations_;
    }
    bool verify = verify_hash_;
#ifndef NDEBUG
    verify = true;
#endif
    if (verify) {
        auto saved = capture();
        if (!saved) return core::Result<std::string>::failure(saved.error());
        auto full = ai::native_state_hash(saved.value());
        if (!full) return full;
        if (full.value() != cached_digest_) {
            assert(false && "native mutation skipped its owner revision bump");
            return fail<std::string>("incremental native digest differs from full recompute");
        }
    }
    return core::Result<std::string>::success(cached_digest_);
}
std::map<std::string, std::uint64_t, std::less<>> ComposedGalacticAiState::external_encodes() const {
    std::map<std::string, std::uint64_t, std::less<>> counts;
    for (const auto& [name, cache] : external_cache_) counts.emplace(name, cache.encodes);
    return counts;
}
std::map<std::string, std::uint64_t, std::less<>> ComposedGalacticAiState::sync_encodes() const {
    std::map<std::string, std::uint64_t, std::less<>> counts;
    for (const auto& [name, cache] : sync_cache_) counts.emplace(name, cache.encodes);
    return counts;
}
bool ComposedGalacticAiState::full_verification_enabled() const noexcept {
#ifndef NDEBUG
    return true;
#else
    return verify_hash_;
#endif
}
core::Result<std::shared_ptr<ComposedGalacticAiState>> install_composed_galactic_ai(GameSession& game,
    GalacticAiContributors owners, std::function<core::Result<void>(const GalacticAiBattleNotice&)> battle) {
    auto state = ComposedGalacticAiState::create(std::move(owners));
    if (!state) return state;
    if (auto installed = game.set_galactic_ai_services({state.value(), std::move(battle)}); !installed)
        return core::Result<std::shared_ptr<ComposedGalacticAiState>>::failure(installed.error());
    return state;
}
core::Result<std::shared_ptr<ComposedGalacticAiState>> install_composed_galactic_ai(GameSession& game,
    GalacticAiContributorsFactory factory, std::function<core::Result<void>(const GalacticAiBattleNotice&)> battle,
    GalacticAiStateAdapter adapt) {
    if (!factory) return fail<std::shared_ptr<ComposedGalacticAiState>>("composed native owner factory is missing");
    std::shared_ptr<ComposedGalacticAiState> state;
    auto installed = game.configure_galactic_ai_services([&](ai::gc::GalacticSession& world) -> core::Result<GalacticAiServices> {
        auto owners = factory(world);
        if (!owners) return core::Result<GalacticAiServices>::failure(owners.error());
        auto composed = ComposedGalacticAiState::create(std::move(owners).value());
        if (!composed) return core::Result<GalacticAiServices>::failure(composed.error());
        state = std::move(composed).value();
        std::shared_ptr<ai::StateContributor> installed_state = state;
        if (adapt) {
            auto adapted = adapt(state);
            if (!adapted) return core::Result<GalacticAiServices>::failure(adapted.error());
            installed_state = std::move(adapted).value();
            if (!installed_state) return fail<GalacticAiServices>("native AI state adapter returned no owner");
        }
        return core::Result<GalacticAiServices>::success({std::move(installed_state), std::move(battle)});
    });
    if (!installed) return core::Result<std::shared_ptr<ComposedGalacticAiState>>::failure(installed.error());
    return core::Result<std::shared_ptr<ComposedGalacticAiState>>::success(std::move(state));
}
core::Result<std::vector<std::uint8_t>> encode_galactic_ai_save(const GalacticAiSave& save) {
    using Bytes = std::vector<std::uint8_t>;
    Bytes bytes; put(bytes, save.completed_tick); put(bytes, save.galactic_frame);
    bytes.push_back(save.state ? 1 : 0);
    if (save.state) {
        auto encoded = ai::encode_native_state(*save.state); if (!encoded) return core::Result<Bytes>::failure(encoded.error());
        bytes.insert(bytes.end(), encoded.value().begin(), encoded.value().end());
    }
    return core::Result<Bytes>::success(std::move(bytes));
}
core::Result<GalacticAiSave> decode_galactic_ai_save(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 17 || bytes[16] > 1) return fail<GalacticAiSave>("invalid native AI save boundary");
    const auto word = [&](std::size_t offset) {
        std::uint64_t value{}; for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8U);
        return value;
    };
    GalacticAiSave save{word(0), word(8), {}};
    if (bytes[16]) {
        auto decoded = ai::decode_native_state(bytes.subspan(17)); if (!decoded) return core::Result<GalacticAiSave>::failure(decoded.error());
        save.state = std::move(decoded).value();
    } else if (bytes.size() != 17) return fail<GalacticAiSave>("trailing native AI save bytes");
    return core::Result<GalacticAiSave>::success(std::move(save));
}
} // namespace eawr::campaign
