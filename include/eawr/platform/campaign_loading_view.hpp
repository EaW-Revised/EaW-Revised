#pragma once

#include "eawr/campaign/conflict.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace eawr::platform {

// WGP-161/162/168/170: admission and location facts are retained before work.
// Missing inputs remain provider seams (legacy #1115), never map-name inference.
enum class CampaignLoadingKind { ordinary, tutorial, story, corruption };
struct CampaignLoadingDescriptor {
    sim::EntityId conflict{};
    sim::galactic::PlanetId planet{};
    campaign::BattleLayer layer{campaign::BattleLayer::space};
    CampaignLoadingKind kind{CampaignLoadingKind::ordinary};
    sim::galactic::PlayerId invader{}, defender{};
    std::optional<sim::galactic::PlayerId> actual_owner, map_owner;
    std::string location_type;
    std::optional<std::string> current_type, current_model, authored_model;
    std::optional<sim::math::Fixed> model_scale, basic_income;
    std::optional<std::int64_t> base_level;
    std::string selected_map;
    std::optional<std::string> dialog_map, original_map, script;
};

// WGP-163..166: retain order and list identity, including decreasing fractions.
// This is native producer progress, separate from BLD-10 viewer work weights.
enum class CampaignLoadingProducer { space_map, land_file, force_roster, transition };
struct CampaignLoadingProgress {
    CampaignLoadingProducer producer{CampaignLoadingProducer::transition};
    std::uint64_t list{}, index{}, count{};
    double fraction{};
};
enum class CampaignLoadingOutcome { pending, succeeded, failed };
struct CampaignLoadingProgressNode {
    CampaignLoadingProgress value;
    std::uint64_t sequence{};
    std::shared_ptr<const CampaignLoadingProgressNode> previous;
};
struct CampaignLoadingView {
    CampaignLoadingDescriptor descriptor;
    // Persistent append-only chain: ordered by sequence, O(1) append even for
    // large rosters. Old publications share nodes instead of recopying history.
    std::shared_ptr<const CampaignLoadingProgressNode> progress;
    // Verified platform content preparation. A renderer must also verify its
    // own scene preparation before Begin; this is not a successful map assertion.
    CampaignLoadingOutcome outcome{CampaignLoadingOutcome::pending};
    std::optional<core::Diagnostic> failure;
    std::uint64_t revision{1};
};
using CampaignLoadingReporter = std::function<void(CampaignLoadingProgress)>;

// Platform-only publication state. No replay/checkpoint bytes or simulation
// policy. Copies are rebuilt on producer changes; readers retain immutable views.
class CampaignLoadingPublication final {
public:
    void begin(CampaignLoadingDescriptor descriptor) {
        CampaignLoadingView next;
        next.descriptor = std::move(descriptor);
        view_ = std::make_shared<const CampaignLoadingView>(std::move(next));
    }
    void report(CampaignLoadingProgress progress) {
        if (!view_ || !std::isfinite(progress.fraction) || progress.fraction < 0.0 || progress.fraction > 1.0) return;
        auto next = *view_;
        next.progress = std::make_shared<const CampaignLoadingProgressNode>(CampaignLoadingProgressNode{
            progress, view_->progress ? view_->progress->sequence + 1 : 1, view_->progress});
        ++next.revision;
        view_ = std::make_shared<const CampaignLoadingView>(std::move(next));
    }
    void finish(std::optional<core::Diagnostic> failure = std::nullopt) {
        if (!view_ || view_->outcome != CampaignLoadingOutcome::pending) return;
        auto next = *view_;
        next.outcome = failure ? CampaignLoadingOutcome::failed : CampaignLoadingOutcome::succeeded;
        next.failure = std::move(failure);
        ++next.revision;
        view_ = std::make_shared<const CampaignLoadingView>(std::move(next));
    }
    void clear() noexcept { view_.reset(); }
    [[nodiscard]] std::shared_ptr<const CampaignLoadingView> view() const noexcept { return view_; }
private:
    std::shared_ptr<const CampaignLoadingView> view_;
};
} // namespace eawr::platform
