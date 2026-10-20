#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/live_game_lockstep.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/platform/campaign_end_provider.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/core/ascii.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"

#include "eawr/campaign/game_session.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/checkpoint/game.hpp"
#include "save_repository.hpp"
#include "live_save_transport.hpp"
#include "../../campaign/galactic_ai_runtime.hpp"
#include "../../campaign/galactic_ai_usability.hpp"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <deque>
#include <mutex>
#include <numeric>
#include <thread>
#include <stdexcept>
#include <utility>
#include <variant>

namespace eawr::platform {
namespace galactic = sim::galactic;
using Clock = std::chrono::steady_clock;

namespace {
core::Diagnostic diagnostic(const std::string_view code, std::string message) {
    core::Diagnostic result;
    result.code = code;
    result.message = std::move(message);
    return result;
}

// GCMP-03 (#4359): a lockstep driver owns the clock and the stamping, so options that bring a second source of
// ticks or orders are refused before any thread starts.
std::optional<core::Diagnostic> lockstep_options_refusal(const LiveGameSession::Options& options) {
    if (!options.step_driver) return std::nullopt;
    if (options.await_presentation)
        return diagnostic("EAWR-PLATFORM-0600", "A lockstep live game cannot await presentation readiness (GCMP-08).");
    if (!options.entries.empty())
        return diagnostic("EAWR-PLATFORM-0600", "A lockstep live game cannot stage local conflict entries.");
    return std::nullopt;
}
LiveAutoResolveProgress progress_view(const campaign::AutoResolveProgress& source) {
    LiveAutoResolveProgress result;
    result.conflict = source.conflict;
    result.elapsed_seconds = static_cast<double>(source.elapsed_mode_seconds.raw()) / sim::math::Fixed::scale;
    result.display_seconds = static_cast<double>(source.display_seconds.raw()) / sim::math::Fixed::scale;
    result.initiated = source.initiated;
    result.withdraw_admitted = source.withdraw_admitted;
    result.retreating_player = source.retreating_player;
    if (source.roster) {
        result.roster.emplace(std::array<LiveAutoResolveProgress::SideRoster, 2>{});
        const auto copy = [](const auto& row) { return LiveAutoResolveProgress::Combatant{row.owner, row.original_type, row.current_type}; };
        for (std::size_t side = 0; side < 2; ++side) {
            const auto& from = (*source.roster)[side]; auto& to = (*result.roster)[side];
            to.owner = from.owner;
            if (from.current) to.current = copy(*from.current);
            if (from.escort) to.escort = copy(*from.escort);
            for (const auto& row : from.queue) to.queue.push_back(copy(row));
        }
    }
    for (std::size_t i = 0; i < result.combatants.size(); ++i) {
        const auto& combatant = source.combatants[i];
        result.combatants[i] = {combatant.owner, combatant.original_type, combatant.current_type};
    }
    return result;
}
void publish_progress(LiveGameFrame& frame, const campaign::AutoResolveProgress& source) {
    frame.auto_resolve_progress = std::make_shared<const LiveAutoResolveProgress>(progress_view(source));
    LiveAutoResolveProgress observer;
    observer.conflict = source.conflict;
    observer.elapsed_seconds = frame.auto_resolve_progress->elapsed_seconds;
    observer.display_seconds = frame.auto_resolve_progress->display_seconds;
    frame.auto_resolve_observer_progress = std::make_shared<const LiveAutoResolveProgress>(std::move(observer));
}
std::shared_ptr<const LiveAutoResolveOutcome> outcome_view(const campaign::AutoResolveOutcomeApplication& source) {
    auto result = std::make_shared<LiveAutoResolveOutcome>();
    result->planet = source.planet;
    result->invader = source.invader;
    result->defender = source.defender;
    result->land = source.land;
    result->result = source.result;
    result->record.reserve(source.record.size());
    for (const auto& row : source.record) result->record.push_back({row.original_type, row.owner});
    return result;
}
}

std::optional<CampaignTacticalView> campaign_tactical_view_for_observer(const LiveGameFrame& frame,
    galactic::SeatId seat) {
    if (!frame.latest || !frame.conflict || !frame.battle) return std::nullopt;
    const auto seats = frame.latest->seats();
    const auto observer = std::find_if(seats.begin(), seats.end(),
        [seat](const auto& row) { return row.seat_id == seat; });
    if (observer == seats.end() || !observer->active || observer->kind != galactic::SeatKind::human) return std::nullopt;
    const auto binding = std::find_if(frame.conflict->seats.begin(), frame.conflict->seats.end(),
        [seat](const auto& row) { return row.seat == seat; });
    if (binding == frame.conflict->seats.end() || binding->player == 0 || binding->player != observer->player
        || (binding->player != frame.conflict->invader && binding->player != frame.conflict->defender)) return std::nullopt;
    CampaignTacticalView view; view.player = binding->player;
    for (const auto& row : frame.battle->economy()) if (row.player == view.player) { view.economy = &row; break; }
    for (const auto& row : frame.battle->retreat_views()) if (row.player == view.player) { view.retreat = &row; break; }
    if (frame.retreat_policy && frame.retreat_policy->conflict == frame.conflict->id) view.policy = frame.retreat_policy;
    return view;
}

std::optional<LiveGameOrder> battle_choice_order(const LiveGameFrame& frame,
    galactic::SeatId seat, sim::EntityId conflict, bool tactical) {
    const auto pending = frame.pending_conflict;
    if (!frame.latest || !pending || !pending->choice_ready || pending->conflict.id != conflict
        || (!tactical && pending->arbitration.forced_tactical)
        || (pending->alert && !(tactical ? pending->alert->begin_admitted : pending->alert->resolve_admitted))) return std::nullopt;
    const auto seats = frame.latest->seats();
    const auto issuer = std::find_if(seats.begin(), seats.end(), [&](const auto& row) { return row.seat_id == seat; });
    if (issuer == seats.end() || !issuer->active || issuer->kind != galactic::SeatKind::human
        || (issuer->player != pending->conflict.invader && issuer->player != pending->conflict.defender)
        || (pending->alert && pending->alert->local != issuer->player)) return std::nullopt;
    return LiveGameOrder{seat, galactic::BattleChoice{conflict, tactical}, {}};
}

std::optional<LiveGameOrder> pending_battle_timeout_order(const LiveGameFrame& frame, galactic::SeatId seat) {
    if (!frame.pending_conflict || !frame.pending_conflict->alert || !frame.pending_conflict->alert->timeout_due) return std::nullopt;
    return battle_choice_order(frame, seat, frame.pending_conflict->conflict.id, true);
}

std::optional<LiveGameOrder> auto_resolve_withdraw_order(const LiveGameFrame& frame,
    galactic::SeatId seat, sim::EntityId conflict) {
    if (!frame.latest || !frame.pending_conflict || !frame.auto_resolve_progress
        || frame.pending_conflict->conflict.id != conflict || frame.auto_resolve_progress->conflict != conflict
        || !frame.auto_resolve_progress->initiated || !frame.auto_resolve_progress->withdraw_admitted) return std::nullopt;
    const auto seats = frame.latest->seats();
    const auto issuer = std::find_if(seats.begin(), seats.end(), [&](const auto& row) { return row.seat_id == seat; });
    const auto& entry = frame.pending_conflict->conflict;
    if (issuer == seats.end() || !issuer->active || issuer->kind != galactic::SeatKind::human
        || (issuer->player != entry.defender && issuer->player != entry.invader)) return std::nullopt;
    return LiveGameOrder{seat, galactic::AutoResolveWithdraw{conflict}, {}};
}

std::shared_ptr<const LiveAutoResolveProgress> auto_resolve_progress_for_observer(const LiveGameFrame& frame, galactic::SeatId observer) {
    if (!frame.auto_resolve_progress) return {};
    const auto* entry = frame.pending_conflict ? &frame.pending_conflict->conflict : frame.conflict.get();
    const auto seats = frame.latest ? frame.latest->seats() : std::span<const galactic::Seat>{};
    const auto seat = std::find_if(seats.begin(), seats.end(), [&](const auto& value) { return value.seat_id == observer; });
    if (!entry || frame.auto_resolve_progress->conflict != entry->id || seat == seats.end() || !seat->active
        || (seat->player != entry->invader && seat->player != entry->defender))
        return frame.auto_resolve_observer_progress;
    return frame.auto_resolve_progress;
}

class PreparedLiveCampaign::Impl {
public:
    Impl(campaign::CampaignRoot value, std::shared_ptr<const CampaignServiceConfiguration> config)
        : root(std::move(value)), services(std::move(config)) {}
    campaign::CampaignRoot root;
    std::shared_ptr<const CampaignServiceConfiguration> services;
    LiveSaveCampaignSource saved_campaign; // #3035
    std::vector<NativeAiPlayer> native_ai_players;
};
PreparedLiveCampaign::PreparedLiveCampaign(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedLiveCampaign::~PreparedLiveCampaign() = default;
core::Result<std::unique_ptr<PreparedLiveCampaign>> PreparedLiveCampaign::create(
    std::shared_ptr<const vfs::Vfs> filesystem_owner, const data::Catalog& catalog, const data::galactic::GalacticData& data,
    const units::GalacticTypeTable& types, std::string_view name,
    campaign::CampaignSetupOptions options, std::string_view difficulty, bool install_native_ai,
    std::array<std::uint64_t, 6> ai_service_periods) {
    using Result = core::Result<std::unique_ptr<PreparedLiveCampaign>>;
    if (!filesystem_owner) return Result::failure(diagnostic("EAWR-SAVE-0104", "Missing campaign filesystem owner."));
    const auto& filesystem = *filesystem_owner;
    const std::shared_ptr<const vfs::Vfs> scoring_files = filesystem_owner;
    if (auto seats = campaign::validate_live_campaign_seats(data, options.players, options.seats); !seats)
        return Result::failure(seats.error());
    // #3035: the authored names a save needs to prepare this campaign again.
    LiveSaveCampaignSource saved_campaign;
    if (const auto* authored = data.campaign(name)) {
        saved_campaign.campaign = authored->name; saved_campaign.campaign_set = authored->campaign_set;
    } else saved_campaign.campaign = std::string(name);
    saved_campaign.difficulty = std::string(difficulty);
    saved_campaign.multiplayer = options.mode == campaign::CampaignSetupMode::multiplayer;
    saved_campaign.custom = options.custom;
    for (const auto& player : options.players) saved_campaign.factions.push_back({player.player.player_id, player.faction});
    auto plan = campaign::plan_live_campaign_setup(filesystem, data, types, name, std::move(options), difficulty);
    if (!plan) return Result::failure(plan.error());
    auto services = campaign::prepare_live_campaign_services(filesystem, catalog, data, types, plan.value(), difficulty);
    if (!services) return Result::failure(services.error());
    auto root = campaign::CampaignRoot::create(std::move(plan).value(), services.value());
    if (!root) return Result::failure(root.error());
    auto provider = campaign::prepare_stock_space_entry(filesystem, catalog, data, types, *root.value().plan());
    if (!provider) return Result::failure(provider.error());
    if (auto configured = campaign::configure_live_campaign_conflicts(root.value(), filesystem, catalog, data, types, provider.value()); !configured)
        return Result::failure(configured.error());
    auto configuration = std::make_shared<CampaignServiceConfiguration>();
    const auto space = provider.value();
    configuration->restore.content = [space](const auto& conflict) { return space->content(conflict); };
    // The restore adapter retains the immutable mounted VFS independently of presentation.
    // All other provider inputs are copied once; every restore gets fresh deterministic adapters.
    configuration->restore.configure_session = [filesystem = std::move(filesystem_owner), catalog = std::make_shared<data::Catalog>(catalog),
        data = std::make_shared<data::galactic::GalacticData>(data), types = std::make_shared<units::GalacticTypeTable>(types),
        plan = std::make_shared<campaign::CampaignSetupPlan>(*root.value().plan()), space](auto& game) {
        return campaign::configure_live_campaign_conflicts(game, *filesystem, *catalog, *data, *types, *plan, space);
    };
    configuration->presentation = [space](const auto& conflict) {
        auto content = space->content(conflict);
        if (!content) return core::Result<std::shared_ptr<const PreparedCampaignBattle>>::failure(content.error());
        return space->presentation(conflict, content.value());
    };
    configuration->loading_descriptor = [plan = std::make_shared<campaign::CampaignSetupPlan>(*root.value().plan()),
        data = std::make_shared<data::galactic::GalacticData>(data)](const auto& conflict, const auto& snapshot) {
        CampaignLoadingDescriptor descriptor;
        descriptor.conflict = conflict.id; descriptor.planet = conflict.planet; descriptor.layer = conflict.layer;
        descriptor.invader = conflict.invader; descriptor.defender = conflict.defender;
        descriptor.selected_map = conflict.map_identity;
        descriptor.kind = conflict.corruption_mission ? CampaignLoadingKind::corruption
            : conflict.tutorial ? CampaignLoadingKind::tutorial
            : !conflict.story_plot.empty() ? CampaignLoadingKind::story : CampaignLoadingKind::ordinary;
        for (const auto& planet : snapshot.planets()) if (planet.id == conflict.planet) descriptor.actual_owner = planet.owner;
        bool destroyed = false;
        if (const auto* control = snapshot.control()) for (const auto& planet : control->planets) if (planet.planet == conflict.planet) {
            destroyed = planet.destroyed;
            descriptor.base_level = conflict.layer == campaign::BattleLayer::space ? planet.starbase.level : planet.ground_base.level;
        }
        for (const auto& location : plan->locations()) if (location.planet && location.planet->id == conflict.planet) {
            descriptor.location_type = location.original_type;
            if (const auto* planet = data->planet(location.original_type)) {
                // Authored model input only: live animation/clone phase remains
                // unavailable (WGP-51/52, loading provider legacy #1115).
                const auto& model = destroyed ? planet->destroyed_galactic_model_name : planet->galactic_model_name;
                if (!model.empty()) descriptor.authored_model = model;
                descriptor.model_scale = planet->scale_factor;
                descriptor.basic_income = destroyed ? planet->planet_destroyed_credit_value : planet->planet_credit_value;
            }
        }
        // Dialog/original map, current runtime type and queued script are not
        // reconstructible from the selected child map (WGP-161/170).
        return descriptor;
    };
    configuration->tactical_capability = [space](const auto& conflict) { return space->availability(conflict); };
    // WGP-37/111, GHUD-S48: authored score text, pictures and movie names are copied from
    // the local and winning factions before shutdown. The scoring half stays absent
    // until the scoring owner binds its provider (title and efficiencies show WGP-97's `0%`).
    {
        // GHUD-S61: the authored scoring module is mounted at the end boundary only, from the
        // saved ledger; everything it reads is copied here, once.
        auto scoring = std::make_shared<CampaignScoringSource>();
        scoring->files = scoring_files;
        scoring->catalog = std::make_shared<const data::Catalog>(catalog);
        scoring->types = std::make_shared<const units::GalacticTypeTable>(types);
        scoring->campaign_name = std::string(root.value().plan()->campaign_name());
        for (const auto& binding : root.value().plan()->type_bindings())
            scoring->type_names.emplace(binding.id, binding.original_type);
        for (const auto& location : root.value().plan()->locations())
            if (location.planet) scoring->planet_types.emplace(location.planet->id, location.original_type);
        const auto scoring_catalog = scoring->catalog;
        configuration->campaign_end_inputs = campaign_end_inputs_provider(scoring_catalog, std::move(scoring));
    }
    // Resolve effective mod-authored flags once, outside the publication loop.
    std::map<galactic::TypeId, bool> prevented_types;
    for (const auto& type : types.types) {
        auto effective = catalog.resolve(type.name, data::Category::game_object);
        if (!effective) return Result::failure(effective.error());
        bool prevents = false;
        if (const auto* value = effective.value().value("Prevents_Story_Campaign_Autoresolve")) {
            data::tag_trace::used(value->value);
            prevents = data::retail::boolean(value->value.raw_text).value_or(false);
        }
        prevented_types.emplace(assets::object_type_crc(type.name), prevents);
    }
    configuration->retreat_policy = [plan = std::make_shared<campaign::CampaignSetupPlan>(*root.value().plan()),
        data = std::make_shared<data::galactic::GalacticData>(data), prevented_types = std::move(prevented_types)](
        const campaign::ConflictRecord& conflict) -> std::optional<CampaignRetreatPolicy> {
        const auto* rules = data->campaign(plan->campaign_name());
        if (!rules) return std::nullopt;
        CampaignRetreatPolicy policy; policy.conflict = conflict.id;
        policy.multiplayer = plan->mode() == campaign::CampaignSetupMode::multiplayer;
        const auto location = std::find_if(plan->locations().begin(), plan->locations().end(),
            [&](const auto& row) { return row.planet && row.planet->id == conflict.planet; });
        if (location == plan->locations().end()) return std::nullopt;
        const auto equal = [](std::string_view left, std::string_view right) {
            return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
                [](unsigned char a, unsigned char b) { return core::ascii_upper(a) == core::ascii_upper(b); });
        };
        policy.planet_excluded = rules->is_autoresolve_allowed == false
            || std::any_of(rules->autoresolve_exclusion_locations.begin(), rules->autoresolve_exclusion_locations.end(),
                [&](const auto& name) { return equal(name, location->original_type); });
        policy.story_prevented = campaign_story_prevention(rules->is_story_campaign.value_or(false), conflict, prevented_types);
        return policy;
    };
    const auto& campaign_plan = *root.value().plan();
    const bool has_ai = std::any_of(campaign_plan.galaxy().seats.begin(), campaign_plan.galaxy().seats.end(),
        [&](const auto& seat) {
            if (!seat.active || seat.kind != galactic::SeatKind::ai) return false;
            const auto binding = std::find_if(campaign_plan.player_bindings().begin(), campaign_plan.player_bindings().end(),
                [&](const auto& player) { return player.active && player.player.player_id == seat.player; });
            const auto* faction = binding == campaign_plan.player_bindings().end() ? nullptr : data.faction(binding->faction);
            return faction && !faction->is_neutral.value_or(false);
        });
    std::vector<NativeAiPlayer> native_ai_players;
    if (install_native_ai && has_ai) {
        const auto* fog = data.constant("AIUsesFogOfWarGalactic");
        const auto uses_fog = fog ? data::retail::boolean(fog->text) : std::nullopt;
        if (!uses_fog) return Result::failure(diagnostic(galactic::diagnostic_codes::invalid_setup, "Missing authored galactic AI fog policy."));
        // Startup/reconstruction uses an inline adapter; live frame phases use
        // the session's actual worker adapter through the existing runtime seam.
        // These are resource ceilings, not gameplay pacing or stock-data limits.
        auto runtime = campaign::prepare_stock_galactic_ai_runtime(filesystem, catalog, data, types, campaign_plan, {},
            std::make_shared<sim::InlineExecutor>(), *uses_fog, galactic::galactic_max_objects, galactic::galactic_max_objects, difficulty,
            ai_service_periods);
        if (!runtime) return Result::failure(runtime.error());
        for (const auto& selected : runtime.value()->lifecycle.players)
            native_ai_players.push_back({selected.player, selected.player_type});
        auto usability = campaign::prepare_stock_galactic_ai_usability(data, campaign_plan, runtime.value()->perception);
        if (!usability) return Result::failure(usability.error());
        campaign::GalacticAiStockQueryFactory queries = [content = usability.value()] {
            auto owner = std::make_shared<campaign::GalacticAiUsabilityProvider>(content);
            campaign::GalacticAiStockQueries out;
            out.usability = [owner](auto player, const auto& snapshot, auto planet) { return owner->usability(player, snapshot, planet); };
            out.connectivity_publication = [owner](const auto& parent, auto& world, auto& engine, const auto& workers) { return owner->connectivity_publication(parent, world, engine, workers); };
            out.movement = [owner](auto player, const auto& snapshot, const auto& world, const auto& engine, const auto& workers) { return owner->movement(player, snapshot, world, engine, workers); };
            out.external.emplace("GALACTIC_USABILITY", std::move(owner));
            return core::Result<campaign::GalacticAiStockQueries>::success(std::move(out));
        };
        auto factory = campaign::stock_galactic_ai_providers(runtime.value(), services.value().stock_starting_forces, space, std::move(queries));
        if (!factory) return Result::failure(factory.error());
        if (auto configured = campaign::configure_galactic_ai_runtime(*configuration, runtime.value(), std::move(factory).value()); !configured)
            return Result::failure(configured.error());
    }
    // Install before publishing the prepared snapshot: owner transfer must not
    // reconstruct its galaxy. The off path skips all native AI preparation,
    // validation and restore hooks; native AI saves require the explicit opt-in.
    if (install_native_ai && configuration->configure_native_ai) {
        auto configured = configuration->configure_native_ai(*root.value().game());
        if (!configured) return Result::failure(configured.error());
    }
    // The prepared owner already completed startup; restore uses its independent
    // reconstruction factory rather than rerunning this fresh-campaign callback.
    configuration->configure_native_ai = {};
    auto impl = std::make_unique<Impl>(std::move(root).value(), std::move(configuration));
    impl->saved_campaign = std::move(saved_campaign);
    impl->native_ai_players = std::move(native_ai_players);
    return Result::success(std::unique_ptr<PreparedLiveCampaign>(new PreparedLiveCampaign(std::move(impl))));
}
const campaign::CampaignSetupPlan& PreparedLiveCampaign::plan() const noexcept { return *impl_->root.plan(); }
const galactic::GalacticSetup& PreparedLiveCampaign::layout() const noexcept { return impl_->root.game()->galaxy().setup(); }
std::shared_ptr<const galactic::GalacticSnapshot> PreparedLiveCampaign::snapshot() const { return impl_->root.game()->galaxy().snapshot(); }
const std::vector<PreparedLiveCampaign::NativeAiPlayer>& PreparedLiveCampaign::native_ai_players() const noexcept { return impl_->native_ai_players; }
bool PreparedLiveCampaign::native_ai_installed() const noexcept { return impl_->root.game()->native_ai_installed(); }

core::Result<std::vector<std::uint8_t>> encode_auto_summary_hint(
    const LiveAutoResolveOutcome& summary, std::string planet_type) {
    campaign::AutoResolveOutcomeApplication value;
    value.planet = summary.planet; value.invader = summary.invader; value.defender = summary.defender;
    value.land = summary.land; value.result = summary.result; value.planet_type = std::move(planet_type);
    for (const auto& row : summary.record) value.record.push_back({row.original_type, row.owner});
    return campaign::encode_auto_resolve_outcome(value);
}
core::Result<std::shared_ptr<const LiveAutoResolveOutcome>> decode_auto_summary_hint(
    const std::vector<std::uint8_t>& bytes) {
    using Result = core::Result<std::shared_ptr<const LiveAutoResolveOutcome>>;
    auto decoded = campaign::decode_auto_resolve_outcome(bytes);
    if (!decoded) return Result::failure(decoded.error());
    return Result::success(outcome_view(decoded.value()));
}
class PreparedLiveRestore::Impl {
public:
    checkpoint::GameRestore restored;
    save_detail::Input input;
    std::string state_sha256;
    std::shared_ptr<const CampaignServiceConfiguration> services;
    LiveSaveOptions saves;
    Impl(checkpoint::GameRestore value, save_detail::Input queued,
        std::shared_ptr<const CampaignServiceConfiguration> config, LiveSaveOptions repository)
        : restored(std::move(value)), input(std::move(queued)), state_sha256(restored.session.state_sha256()),
          services(std::move(config)), saves(std::move(repository)) {}
};
PreparedLiveRestore::PreparedLiveRestore(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedLiveRestore::~PreparedLiveRestore() = default;
const galactic::GalacticSetup& PreparedLiveRestore::layout() const noexcept { return impl_->restored.session.galaxy().setup(); }
std::uint64_t PreparedLiveRestore::completed_tick() const noexcept { return impl_->restored.session.completed_tick(); }
const std::vector<std::uint8_t>& PreparedLiveRestore::presentation_hints() const noexcept { return impl_->restored.presentation_hints; }
const std::string& PreparedLiveRestore::state_sha256() const noexcept { return impl_->state_sha256; }

class LiveGameSession::Impl {
public:
    class Feed;
    Impl(campaign::GameSession session, Options options)
        : owner_(std::move(session)), session_(&std::get<campaign::GameSession>(owner_)), options_(std::move(options)) {
        initialize();
    }
    Impl(campaign::CampaignRoot root, Options options)
        : owner_(std::move(root)), session_(std::get<campaign::CampaignRoot>(owner_).game()), options_(std::move(options)) {
        initialize();
    }
    void initialize() {
        if (options_.interactive_seat.value) {
            auto configured = session_->set_interactive_seat(options_.interactive_seat);
            if (!configured) throw std::invalid_argument(configured.error().message);
        }
        const auto snapshot = session_->galaxy().snapshot();
        frame_ = {snapshot, snapshot, Clock::now(), session_->completed_tick(), {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, false, {}, {}, {}, {}};
        frame_.summary_hold = session_->battle_summary_hold();
        target_ = session_->completed_tick();
        if (const auto* result = session_->latest_tactical_result()) frame_.result = *result;
        if (const auto* context = session_->latest_tactical_context()) {
            frame_.result_conflict = std::make_shared<const campaign::ConflictRecord>(*context);
            const auto observer = snapshot->observer() ? snapshot->observer()->local_player : 0;
            frame_.invasion_offer = invasion_publication_.publish(session_->latest_tactical_result(), context, *snapshot,
                observer, session_->galaxy().native_invasion_available(), invasion_selection_revision_, session_->battle_summary_hold() == context->id);
        }
        // Retained selections are private owner state; presentation receives
        // them only through its observer-filtered fact, not the raw result.
        if (frame_.result) frame_.result->invasion_offers.clear();
        for (const auto& command : session_->record().commands) {
            if (command.key.tick >= session_->completed_tick()) continue;
            sequences_[command.key.seat] = std::max(sequences_[command.key.seat], command.key.sequence);
            if (const auto* request = std::get_if<galactic::SaveRequest>(&command.payload); request && request->slot > 0)
                save_token_ = std::max(save_token_, static_cast<std::uint64_t>(request->slot));
        }
        if (auto pending = session_->pending_conflict_view())
            frame_.pending_conflict = std::make_shared<const campaign::PendingConflictView>(std::move(*pending));
        if (const auto* conflict = session_->conflict()) {
            active_conflict_ = std::make_shared<const campaign::ConflictRecord>(*conflict);
            frame_.conflict = active_conflict_;
            frame_.battle = session_->battle()->snapshot();
            frame_.battle_lifetime = session_->battle_lifetime();
            battle_frame_ = {frame_.battle, frame_.battle, Clock::now()};
            battle_snapshots_.push_back(frame_.battle);
            if (const auto* cells = session_->battle()->fog_cells())
                for (std::size_t player = 0; player < session_->battle()->players().size(); ++player) {
                    const auto owner = session_->battle()->players()[player].player_id;
                    battle_fog_[owner].push_back(std::make_shared<const LiveFog>(LiveFog{
                        frame_.battle->completed_tick(), cells->rules(), owner, cells->value_rows(player)}));
                }
        }
        if (const auto progress = session_->auto_resolve_progress()) publish_progress(frame_, *progress);
        for (const auto& planet : snapshot->planets()) published_owners_.emplace(planet.id, planet.owner);
        session_->set_state_hasher(std::make_shared<ThreadStateHasher>());
        session_->set_technology_notice_handler([this](const auto& notice) {
            // SND-68/WGEP-130: the setter has already qualified a new unlock
            // batch and its local recipient. Never infer it from build choices.
            if (notice.kind == galactic::GalacticSession::TechnologyNotice::Kind::new_options) {
                const std::lock_guard lock(mutex_);
                LiveGalacticEvent published{};
                published.cursor = ++galactic_cursor_;
                published.construction_options_player = notice.player;
                galactic_events_.push_back(std::move(published));
                if (galactic_events_.size() > 4096) galactic_events_.pop_front();
            }
            return core::Result<void>::success();
        });
        session_->set_feedback_notice_handler([this](const auto& notice) {
            // Simulation-thread staging: failed steps publish no feedback, and
            // WGP-53's ownership notice stays before its credited reward cue.
            pending_feedback_.push_back(notice);
        });
        publish_preparation();
    }

    void publish_preparation(std::unique_lock<std::mutex>* publication_lock = nullptr) {
        if (!frame_.conflict) { loading_.clear(); prepared_battle_.reset(); prepared_conflict_ = 0; }
        if (!options_.services) return;
        const auto& services = *options_.services;
        frame_.retreat_policy.reset();
        if (frame_.conflict && services.retreat_policy) {
            frame_.retreat_policy = services.retreat_policy(*frame_.conflict);
            if (frame_.retreat_policy) frame_.retreat_policy->conflict = frame_.conflict->id;
        }
        if (frame_.pending_conflict && services.tactical_capability) {
            const auto capability = services.tactical_capability(frame_.pending_conflict->conflict);
            frame_.tactical_available = bool(capability);
            if (!capability) {
                frame_.tactical_refusal = capability.error().message;
                frame_.tactical_refusal_code = capability.error().code;
            }
        }
        if (frame_.conflict && (services.presentation || services.loading_presentation)) {
            if (prepared_conflict_ != frame_.conflict->id) {
                const auto conflict = frame_.conflict;
                CampaignLoadingDescriptor descriptor;
                if (services.loading_descriptor && frame_.latest) descriptor = services.loading_descriptor(*conflict, *frame_.latest);
                else descriptor.kind = conflict->corruption_mission ? CampaignLoadingKind::corruption
                    : conflict->tutorial ? CampaignLoadingKind::tutorial
                    : !conflict->story_plot.empty() ? CampaignLoadingKind::story : CampaignLoadingKind::ordinary;
                // The owner fixes identity; a descriptor callback cannot redirect it.
                descriptor.conflict = conflict->id; descriptor.planet = conflict->planet; descriptor.layer = conflict->layer;
                descriptor.invader = conflict->invader; descriptor.defender = conflict->defender;
                descriptor.selected_map = conflict->map_identity;
                loading_.begin(std::move(descriptor));
                prepared_conflict_ = conflict->id; prepared_battle_.reset();
                frame_.battle_presentation.reset();
                published_.notify_all();
                // Keep owning session state locked, but allow read-only UI access
                // to the retained descriptor while platform preparation runs.
                if (publication_lock) publication_lock->unlock();
                const CampaignLoadingReporter report = [this, id = conflict->id, concurrent = publication_lock != nullptr](auto progress) {
                    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
                    if (concurrent) lock.lock();
                    const auto view = loading_.view();
                    if (view && view->descriptor.conflict == id) loading_.report(progress);
                    published_.notify_all();
                };
                try {
                    auto prepared = services.loading_presentation ? services.loading_presentation(*conflict, report)
                        : services.presentation(*conflict);
                    if (publication_lock) publication_lock->lock();
                    if (prepared && prepared.value()) {
                        prepared_battle_ = std::move(prepared).value(); loading_.finish();
                    } else {
                        const auto error = prepared ? diagnostic(galactic::diagnostic_codes::invalid_setup,
                            "Campaign presentation provider returned no prepared content.") : prepared.error();
                        loading_.finish(error);
                        frame_.tactical_refusal = error.message; frame_.tactical_refusal_code = error.code;
                    }
                } catch (...) {
                    if (publication_lock && !publication_lock->owns_lock()) publication_lock->lock();
                    loading_.finish(diagnostic(galactic::diagnostic_codes::worker_failure,
                        "Campaign presentation preparation threw an exception."));
                    throw;
                }
            }
            frame_.battle_presentation = prepared_battle_;
        }
        if (frame_.pending_conflict && !frame_.tactical_available && frame_.tactical_refusal.empty()) {
            frame_.tactical_refusal = "Tactical content is unavailable for this conflict.";
            frame_.tactical_refusal_code = galactic::diagnostic_codes::invalid_setup;
        }
    }

    ~Impl() { stop(); }
    void start() {
        disk_thread_ = std::thread([this] { disk_loop(); });
        thread_ = std::thread([this] { run(); });
    }
    void stop() {
        const std::lock_guard join_lock(join_mutex_);
        stop_locked();
    }
    void stop_locked() {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        published_.notify_all();
        if (thread_.joinable()) thread_.join();
        { const std::lock_guard lock(mutex_); disk_stopping_ = true; }
        disk_wake_.notify_all();
        if (disk_thread_.joinable()) disk_thread_.join();
        {
            const std::lock_guard lock(mutex_);
            for (auto& [token, outcome] : save_outcomes_) {
                (void)token;
                if (outcome.phase == LiveSavePhase::requested) {
                    outcome.phase = LiveSavePhase::failed;
                    outcome.diagnostic = diagnostic("EAWR-SAVE-0102", "Session stopped before save admission.");
                }
            }
        }
        published_.notify_all();
    }

    core::Result<std::shared_ptr<const CampaignEndView>> consume_campaign_result(
        galactic::SeatId observer, bool dialog_active, bool movie_playing) {
        using Result = core::Result<std::shared_ptr<const CampaignEndView>>;
        // The presentation caller owns outer admission. Never self-join the
        // simulation thread or insert a guessed frame-service position.
        const std::lock_guard join_lock(join_mutex_);
        std::map<galactic::SeatId, std::shared_ptr<const CampaignEndView>> staged;
        if (thread_.get_id() == std::this_thread::get_id()) return Result::failure(diagnostic(
            galactic::diagnostic_codes::invalid_setup, "Campaign result requires its outer presentation owner."));
        {
            const std::lock_guard state_lock(state_mutex_);
            {
                const std::lock_guard lock(mutex_);
                if (const auto at = campaign_end_publications_.find(observer); at != campaign_end_publications_.end())
                    return Result::success(at->second);
                if (stopping_ || ended_ || !session_) return Result::failure(diagnostic(
                    galactic::diagnostic_codes::invalid_setup, "Campaign result has no live owner."));
            }
            const auto snapshot = session_->galaxy().snapshot();
            const auto seats = snapshot->seats();
            const auto local = std::find_if(seats.begin(), seats.end(), [observer](const auto& row) { return row.seat_id == observer; });
            if (local == seats.end() || !local->active || !local->player || local->kind != galactic::SeatKind::human)
                return Result::failure(diagnostic(galactic::diagnostic_codes::invalid_setup, "Campaign result observer is not an active human seat."));
            const auto monitor = session_->galaxy().victory();
            if (!monitor || session_->mode() != campaign::SessionMode::galaxy) return Result::success({});
            const auto result = galactic::campaign_result(*monitor, local->player, dialog_active, movie_playing);
            if (!result) return Result::success({});
            const auto winner = std::find_if(monitor->context.players.begin(), monitor->context.players.end(),
                [&](const auto& row) { return row.player == result->condition.player; });
            const auto make_view = [&](galactic::PlayerId player) {
                CampaignEndView view;
                view.condition = result->condition; view.local_player = player;
                view.local_won = view.condition.player == player;
                if (winner != monitor->context.players.end()) view.winner_faction = winner->faction;
                view.route = monitor->context.multiplayer ? CampaignEndRoute::multiplayer_quit : CampaignEndRoute::score;
                view.ending_mode = monitor->context.multiplayer ? CampaignEndingMode::other_multiplayer : CampaignEndingMode::solo;
                if (options_.services && monitor->context.multiplayer
                    && options_.services->ending_mode == CampaignEndingMode::lan) view.ending_mode = CampaignEndingMode::lan;
                return view;
            };
            if (result->route == galactic::CampaignResultRoute::multiplayer_quit) {
                // WGS-177: deduplicate this local intent, independent of winner.
                auto view = std::make_shared<const CampaignEndView>(make_view(local->player));
                const std::lock_guard lock(mutex_);
                for (const auto& seat : seats) if (seat.active && seat.kind == galactic::SeatKind::human && seat.player == local->player)
                    campaign_end_publications_.emplace(seat.seat_id, view);
                if (snapshot->observer() && snapshot->observer()->local_player == local->player) default_end_ = view;
                return Result::success(std::move(view));
            }
            // WGP-111: bounded local-player copies occur once at the end boundary,
            // never during ordinary frames. Co-op seats share the same local copy.
            std::map<galactic::PlayerId, std::shared_ptr<const CampaignEndView>> players;
            for (const auto& seat : seats) {
                if (!seat.active || !seat.player || seat.kind != galactic::SeatKind::human) continue;
                auto at = players.find(seat.player);
                if (at == players.end()) {
                    auto view = make_view(seat.player);
                    if (options_.services && options_.services->campaign_end_inputs) {
                        auto local_result = *result; local_result.local_won = view.local_won;
                        auto inputs = options_.services->campaign_end_inputs(*session_, local_result, seat.player);
                        if (!inputs) return Result::failure(inputs.error());
                        view.inputs = std::move(inputs).value();
                        view.provider_seam = view.inputs->scoring_seam;
                    } else view.provider_seam = "campaign scoring controls and authored faction/movie inputs are unbound";
                    at = players.emplace(seat.player, std::make_shared<const CampaignEndView>(std::move(view))).first;
                }
                staged.emplace(seat.seat_id, at->second);
            }
            ended_record_ = session_->record();
            { const std::lock_guard lock(mutex_); stopping_ = true; }
        }
        // WGS-176: finish admitted disk work and join all workers before releasing
        // AI/script ownership. End admission itself requests no autosave.
        stop_locked();
        {
            const std::lock_guard state_lock(state_mutex_);
            owner_.emplace<std::monostate>(); session_ = nullptr;
            options_.services.reset(); options_.command_source = {}; options_.entries.clear();
            const std::lock_guard lock(mutex_);
            battle_source_ = {}; active_conflict_.reset(); prepared_battle_.reset();
            order_inbox_.clear(); command_inbox_.clear();
            frame_.conflict.reset(); frame_.battle.reset(); frame_.battle_presentation.reset();
            frame_.pending_conflict.reset(); frame_.invasion_offer.reset();
            campaign_end_publications_ = std::move(staged);
            const auto default_player = frame_.latest->observer() ? frame_.latest->observer()->local_player : 0;
            for (const auto& [seat, view] : campaign_end_publications_) {
                (void)seat; if (view->local_player == default_player) { default_end_ = view; break; }
            }
        }
        published_.notify_all();
        return Result::success(campaign_end_publications_.at(observer));
    }

    void write_disk_save(const auto& job) {
        try {
            const auto written = save_detail::write(options_.saves.directory, job.request.name, job.bytes, job.request.overwrite);
            const std::lock_guard lock(mutex_);
            auto& outcome = save_outcomes_.at(job.token);
            outcome.phase = written ? LiveSavePhase::completed : LiveSavePhase::failed;
            if (!written) outcome.diagnostic = written.error();
        } catch (const std::exception& error) {
            const std::lock_guard lock(mutex_);
            auto& outcome = save_outcomes_.at(job.token); outcome.phase = LiveSavePhase::failed;
            outcome.diagnostic = diagnostic("EAWR-SAVE-0101", error.what());
        }
        published_.notify_all();
    }

    void publish_galactic_events(core::Result<campaign::GameTick>& stepped) {
        // Session controls can replace the published snapshot
        // during a child while the galaxy's tick stays frozen.
        if (stepped.value().galaxy != frame_.latest) {
            std::size_t event_index{}, feedback_index{};
            const auto publish_feedback = [&] {
                while (feedback_index < pending_feedback_.size()
                    && pending_feedback_[feedback_index].preceding_events <= event_index) {
                    LiveGalacticEvent published{};
                    const auto notice = pending_feedback_[feedback_index++];
                    if (notice.kind == galactic::GalacticFeedbackNotice::Kind::enemy_approaching) {
                        const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now().time_since_epoch()).count());
                        if (!approach_throttle_.admit(notice.player, now)) continue;
                    }
                    published.cursor = ++galactic_cursor_;
                    published.feedback = notice;
                    galactic_events_.push_back(std::move(published));
                    if (galactic_events_.size() > 4096) galactic_events_.pop_front();
                }
            };
            for (const auto& event : stepped.value().galaxy->events()) {
                publish_feedback();
                LiveGalacticEvent published{++galactic_cursor_, event, std::nullopt};
                if (event.kind == galactic::GalacticEventKind::conflict_pending) {
                    const auto alert = stepped.value().pending_alerts.find(event.subject);
                    if (alert != stepped.value().pending_alerts.end())
                        published.pending_alert = LiveGalacticEvent::PendingAlert{
                            alert->second.invader, alert->second.defender, alert->second.land};
                }
                if (event.kind == galactic::GalacticEventKind::planet_owner_changed) {
                    if (const auto* control = stepped.value().galaxy->control()) {
                        const auto owner = std::find_if(control->players.begin(), control->players.end(),
                            [&](const auto& player) { return player.player == event.value; });
                        published.new_owner_playable = owner != control->players.end() && owner->playable;
                    }
                    if (const auto found = published_owners_.find(event.subject); found != published_owners_.end()) {
                        published.previous_owner = found->second;
                        found->second = static_cast<galactic::PlayerId>(event.value);
                    }
                }
                galactic_events_.push_back(std::move(published));
                // A presentation resource bound, not a gameplay limit.
                if (galactic_events_.size() > 4096) galactic_events_.pop_front();
                ++event_index;
            }
            publish_feedback();
            pending_feedback_.clear();
        }
    }

    void publish_story_notices(core::Result<campaign::GameTick>& stepped, const galactic::PlayerId observer) {
        story_notice_publications_.clear();
        frame_.story_notices.reset();
        if (!stepped.value().story_notices.empty()) {
            std::map<galactic::PlayerId, std::shared_ptr<const std::vector<campaign::GameStoryNotice>>> by_player;
            for (const auto& seat : frame_.latest->seats()) {
                if (!seat.active || seat.kind != galactic::SeatKind::human || !seat.player) continue;
                auto at = by_player.find(seat.player);
                if (at == by_player.end()) {
                    auto notices = stepped.value().observer_story_notices(seat.player);
                    if (notices.empty()) continue;
                    at = by_player.emplace(seat.player,
                        std::make_shared<const std::vector<campaign::GameStoryNotice>>(std::move(notices))).first;
                }
                story_notice_publications_.emplace(seat.seat_id, at->second);
                if (seat.player == observer) frame_.story_notices = at->second;
            }
        }
    }

    void publish_battle_frame(core::Result<campaign::GameTick>& stepped) {
        if (const auto* conflict = session_->conflict()) {
            if (!active_conflict_ || active_conflict_->id != conflict->id) {
                active_conflict_ = std::make_shared<const campaign::ConflictRecord>(*conflict);
                battle_frame_ = {};
                battle_snapshots_.clear();
                battle_hashes_.clear();
                battle_events_ = LiveEventLog{LiveEventLog::Bounds{}};
                battle_fog_.clear();
                ready_conflict_ = 0;
                battle_paused_ = true;
                battle_source_ = {};
                battle_base_types_.clear();
            }
            frame_.conflict = active_conflict_;
        }
        if (frame_.battle) {
            const auto snapshot = frame_.battle;
            battle_frame_ = {battle_frame_.latest ? battle_frame_.latest : snapshot, snapshot, Clock::now()};
            battle_snapshots_.push_back(snapshot);
            if (battle_snapshots_.size() > 64) battle_snapshots_.pop_front();
            battle_events_.record(*snapshot, stepped.value().asteroid_impacts, battle_base_types_);
            if (const auto* world = session_->battle()) {
                if (const auto* cells = world->fog_cells()) {
                    for (std::size_t player = 0; player < world->players().size(); ++player) {
                        const auto owner = world->players()[player].player_id;
                        battle_fog_[owner].push_back(std::make_shared<const LiveFog>(LiveFog{
                            snapshot->completed_tick(), cells->rules(), owner, cells->value_rows(player)}));
                        if (battle_fog_[owner].size() > 64) battle_fog_[owner].pop_front();
                    }
                }
            }
            if (snapshot->completed_tick() != 0) battle_hashes_.push_back(stepped.value().battle_state_hash);
        }
    }

    void publish_completed_tick(core::Result<campaign::GameTick>& stepped, const Clock::time_point step_started,
        const Clock::time_point source_done, const Clock::time_point step_done,
        const std::vector<LiveGameOrder>& orders, const std::vector<galactic::GameCommand>& commands,
        std::vector<LiveBattleChoiceResponse>& choice_responses, std::unique_lock<std::mutex>& lock) {
        {
            const auto ms = [](const Clock::time_point from, const Clock::time_point to) {
                return std::chrono::duration<double, std::milli>(to - from).count();
            };
            LiveTickCost cost{stepped.value().completed_tick, ms(step_started, step_done),
                {{"command-source", ms(step_started, source_done)}, {"session-step", ms(source_done, step_done)}}};
            costs_.push_back(std::move(cost));
            if (costs_.size() > tick_cost_history) costs_.pop_front();
        }
        capture_saves(stepped.value(), orders, commands);
        hashes_.push_back(std::move(stepped.value().state_hash));
        const auto previous_result = frame_.result;
        const auto previous_result_context = frame_.result_conflict;
        publish_galactic_events(stepped);
        for (const auto& response : stepped.value().invasion_accept_responses) {
            // The command already ran at its tick on the agreed state; this only publishes its outcome.
            ++invasion_selection_revision_; // a partial refusal can also consume a fleet selection
            if (!response.refusal.empty())
                rejected_.push_back(diagnostic(galactic::diagnostic_codes::invalid_command, response.refusal));
            else if (response.summary_closed) { summary_closed_ = response.conflict; summary_closed_seat_ = response.seat; }
        }
        frame_ = {frame_.latest, stepped.value().galaxy, Clock::now(), 0, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, false, {}, {}, {}, {}};
        frame_.session_tick = stepped.value().completed_tick;
        frame_.summary_hold = session_->battle_summary_hold();
        frame_.summary_closed = summary_closed_; frame_.summary_closed_seat = summary_closed_seat_;
        // Presentation may be slower than the real-time parent pacer.
        frame_.result = stepped.value().result ? stepped.value().result : previous_result;
        frame_.result_conflict = previous_result_context;
        if (stepped.value().result) {
            ++invasion_selection_revision_;
            frame_.result_conflict.reset();
            if (const auto* context = session_->latest_tactical_context(); context && context->id == stepped.value().result->conflict)
                frame_.result_conflict = std::make_shared<const campaign::ConflictRecord>(*context);
        }
        frame_.battle = stepped.value().battle;
        const auto observer = frame_.latest->observer() ? frame_.latest->observer()->local_player : 0;
        publish_story_notices(stepped, observer);
        frame_.invasion_offer = invasion_publication_.publish(session_->latest_tactical_result(),
            session_->latest_tactical_context(), *frame_.latest, observer,
            session_->galaxy().native_invasion_available(), invasion_selection_revision_,
            session_->latest_tactical_context() && session_->battle_summary_hold() == session_->latest_tactical_context()->id);
        if (frame_.result) frame_.result->invasion_offers.clear();
        frame_.battle_lifetime = session_->battle_lifetime();
        if (const auto progress = session_->auto_resolve_progress())
            publish_progress(frame_, *progress);
        const auto outcome = session_->latest_auto_resolve_outcome();
        if (outcome != published_outcome_) {
            published_outcome_ = outcome;
            published_outcome_view_ = outcome ? outcome_view(*outcome) : nullptr;
        }
        frame_.auto_resolve_outcome = published_outcome_view_;
        for (const auto& response : stepped.value().battle_choice_responses)
            choice_responses.push_back({response.conflict, 0, response.seat, response.refusal});
        if (auto pending = session_->pending_conflict_view())
            frame_.pending_conflict = std::make_shared<const campaign::PendingConflictView>(std::move(*pending));
        publish_battle_frame(stepped);
        publish_preparation(&lock);
    }

    void submit_pending_input(const std::uint64_t tick, const std::vector<galactic::GameCommand>& due_commands,
        const std::vector<std::size_t>& command_order, std::vector<LiveGameOrder>& due_orders,
        const auto& reject, bool& accepted) {
        for (const auto index : command_order) {
            const auto& command = due_commands[index];
            if (auto submitted = session_->submit(command); !submitted) reject(command.key.seat, command.payload, submitted.error());
            else {
                sequences_[command.key.seat] = std::max(sequences_[command.key.seat], command.key.sequence);
                accepted = true;
            }
        }
        for (auto& order : due_orders) {
            auto& sequence = sequences_[order.seat];
            if (sequence == std::numeric_limits<std::uint64_t>::max()) {
                reject(order.seat, order.payload, diagnostic(galactic::diagnostic_codes::resource_limit,
                    "live game seat sequence exhausted"));
            } else {
                const galactic::GameCommand command{{order.tick.value_or(tick), order.seat, sequence + 1}, std::move(order.payload)};
                if (auto submitted = session_->submit(command); !submitted) reject(command.key.seat, command.payload, submitted.error());
                else { ++sequence; accepted = true; }
            }
        }
    }

    bool publish_input_outcomes(const std::vector<core::Diagnostic>& rejected,
        const std::vector<LiveBattleChoiceResponse>& choice_responses) {
        {
            const std::lock_guard lock(mutex_);
            rejected_.insert(rejected_.end(), rejected.begin(), rejected.end());
            for (auto response : choice_responses) {
                response.revision = ++choice_response_revision_;
                choice_responses_[response.seat] = std::move(response);
            }
            frame_.battle_choice_responses.clear();
            for (const auto& [seat, response] : choice_responses_) {
                (void)seat;
                frame_.battle_choice_responses.push_back(response);
            }
            if (failure_) {
                for (auto& [token, outcome] : save_outcomes_) {
                    (void)token;
                    if (outcome.phase == LiveSavePhase::requested) {
                        outcome.phase = LiveSavePhase::failed; outcome.diagnostic = failure_;
                    }
                }
                published_.notify_all();
                return false;
            }
        }
        return true;
    }

    void advance_schedule(const bool accepted, const bool paced, const auto& clock,
        const std::uint32_t battle_rate, LivePaceSchedule& schedule) {
        // Pace on a fixed-rate schedule so neither wake-up latency nor simulation work adds to every
        // interval; a backlog beyond max_catch_up is dropped. Resume and speed commands start a
        // fresh interval; pacing remains outside deterministic state (ADR-013).
        if (accepted || paced) {
            const auto after = session_->galaxy().clock();
            const bool changed = after.pause_depth != clock.pause_depth || after.strategic_speed != clock.strategic_speed;
            const auto interval = session_->mode() == campaign::SessionMode::battle ? LiveSession::tick_interval(battle_rate)
                : tick_interval(after.strategic_speed);
            if (changed || interval != schedule.interval()) schedule.rebase(Clock::now(), interval);
            else schedule.stepped();
        }
    }

    bool await_tick_input(const std::uint64_t tick, const bool in_battle, bool& paused,
        const auto& due_order, const auto& due_command, std::vector<LiveGameOrder>& orders,
        std::vector<galactic::GameCommand>& commands, LivePaceSchedule& schedule,
        std::uint32_t& battle_rate, bool& commanded, bool& paced) {
        {
            std::unique_lock lock(mutex_);
            // ADR-019: creation publishes tick zero, but presentation may still
            // be loading. This wait belongs only to the live pacer.
            if (in_battle) {
                const auto id = session_->conflict()->id;
                if (!wake_.wait_for(lock, options_.readiness_timeout, [&] { return stopping_ || ready_conflict_ == id; })) {
                    failure_ = diagnostic(galactic::diagnostic_codes::worker_failure,
                        "viewer readiness timed out for conflict " + std::to_string(id) + " at child tick zero");
                    published_.notify_all();
                    return false;
                }
                wake_.wait(lock, [&] { return stopping_ || !battle_paused_ || !save_requests_.empty(); });
                if (stopping_) return false;
                paused = battle_paused_;
            }
            const auto ready = [&] {
                return stopping_ || !order_inbox_.empty() || !command_inbox_.empty()
                    || (paused && (std::any_of(orders.begin(), orders.end(), due_order)
                        || std::any_of(commands.begin(), commands.end(), due_command)))
                    || (!paused && options_.pacing == Pacing::driven && target_ > tick);
            };
            if (paused || options_.pacing == Pacing::driven) wake_.wait(lock, ready);
            else wake_.wait_until(lock, schedule.next_due(), ready);
            if (stopping_) return false;
            battle_rate = battle_rate_;
            std::move(order_inbox_.begin(), order_inbox_.end(), std::back_inserter(orders));
            std::move(command_inbox_.begin(), command_inbox_.end(), std::back_inserter(commands));
            order_inbox_.clear();
            command_inbox_.clear();
            commanded = std::any_of(orders.begin(), orders.end(), due_order)
                || std::any_of(commands.begin(), commands.end(), due_command);
            paced = !paused && (options_.pacing == Pacing::driven ? target_ > tick : Clock::now() >= schedule.next_due());
            // Too slow for real time: drop the missed wall time rather than spiral.
            if (paced && options_.pacing == Pacing::real_time) schedule.drop_backlog(Clock::now(), options_.max_catch_up);
        }
        return true;
    }

    void disk_loop() noexcept {
        for (;;) {
            DiskSave job;
            {
                std::unique_lock lock(mutex_);
                disk_wake_.wait(lock, [&] { return disk_stopping_ || !disk_saves_.empty(); });
                if (disk_saves_.empty()) return;
                job = std::move(disk_saves_.front()); disk_saves_.pop_front();
            }
            write_disk_save(job);
        }
    }

    // Called under both session and publication locks, after the admitted save tick.
    void capture_saves(const campaign::GameTick& tick,
        const std::vector<LiveGameOrder>& orders, const std::vector<galactic::GameCommand>& commands) {
        for (const auto& event : tick.save_events) {
            const auto found = save_requests_.find(event.value);
            if (found == save_requests_.end() || found->second.seat != event.producer) continue;
            const auto token = found->first; auto request = std::move(found->second); save_requests_.erase(found);
            auto& outcome = save_outcomes_.at(token); outcome.completed_tick = tick.completed_tick;
            if (event.kind == galactic::GalacticEventKind::save_rejected) {
                outcome.phase = LiveSavePhase::rejected;
                outcome.diagnostic = diagnostic("EAWR-SAVE-0103", "Save request refused by session admission.");
                continue;
            }
            auto identities = checkpoint::game_identities(session_->galaxy().setup());
            identities.mod_list = options_.saves.mod_list_identity;
            auto captured = checkpoint::save_game_checkpoint(*session_, identities, request.presentation_hints);
            if (captured) {
                auto saved = checkpoint::parse_checkpoint(captured.value());
                if (!saved) {
                    outcome.phase = LiveSavePhase::failed; outcome.diagnostic = saved.error();
                    continue;
                }
                save_detail::Input input{orders, commands};
                input.orders.insert(input.orders.end(), order_inbox_.begin(), order_inbox_.end());
                input.commands.insert(input.commands.end(), command_inbox_.begin(), command_inbox_.end());
                // Already-late transport packets can only produce local refusals; they
                // are not admitted future input and claim no authoritative cursor.
                std::erase_if(input.orders, [&](const auto& order) { return order.tick && *order.tick < tick.completed_tick; });
                std::erase_if(input.commands, [&](const auto& command) { return command.key.tick < tick.completed_tick; });
                bool extended = false;
                if (!input.orders.empty() || !input.commands.empty()) {
                    auto encoded = save_detail::encode_input(session_->galaxy().setup(), tick.completed_tick, input);
                    if (!encoded) captured = core::Result<std::vector<std::uint8_t>>::failure(encoded.error());
                    else { saved.value().sections.push_back(std::move(encoded).value()); extended = true; }
                }
                // #3035: ascending after the queued input section; outside every state hash.
                if (const auto identity = save_detail::save_campaign(options_.saves, session_->galaxy().setup(), request.seat);
                    captured && identity) { saved.value().sections.push_back(save_detail::encode_campaign(*identity)); extended = true; }
                if (captured && extended) captured = checkpoint::write_checkpoint(saved.value());
            }
            if (!captured) { outcome.phase = LiveSavePhase::failed; outcome.diagnostic = captured.error(); }
            else { outcome.phase = LiveSavePhase::captured;
                disk_saves_.push_back({token, std::move(request), std::move(captured).value()}); disk_wake_.notify_one(); }
        }
    }

    void run() noexcept {
        try {
            loop();
        } catch (const std::exception& error) {
            fail(error.what());
        } catch (...) {
            fail("non-standard exception");
        }
        if (options_.step_driver) options_.step_driver->session_ended();
        {
            const std::lock_guard lock(mutex_);
            ended_ = true;
        }
        published_.notify_all();
    }

    void fail(const char* message) noexcept {
        try {
            const std::lock_guard lock(mutex_);
            failure_ = diagnostic(galactic::diagnostic_codes::worker_failure,
                std::string("live game simulation thread stopped: ") + message);
        } catch (...) {
            // Even allocation failure wakes waiters through ended_.
        }
    }

    // WGS-99/142/143/145: choosing the tactical battle of a pending single-player space conflict
    // first writes the reserved autosave. The save request runs in this tick while the conflict is
    // still pending (the capture is the pre-battle state); the choice itself follows next tick. A
    // refused or failed save never holds the battle back. Skirmish, multiplayer and the first Begin
    // after restoring the autosave itself do not produce one (WGS-142/143).
    void request_prebattle_autosave(const std::uint64_t tick, std::vector<LiveGameOrder>& due_orders,
        std::vector<LiveGameOrder>& kept_orders) {
        const auto* pending = session_->pending_conflict();
        if (!pending || session_->mode() == campaign::SessionMode::battle || options_.saves.directory.empty()) return;
        if (pending->layer != campaign::BattleLayer::space || pending->tactical.skirmish) return;
        const auto& seats = session_->galaxy().setup().seats;
        // WGS-33/143: multiplayer mode writes no autosave. The single human seat is this producer's own
        // precondition (the one local chooser), no longer the multiplayer test.
        if (session_->galaxy().multiplayer()) return;
        if (std::count_if(seats.begin(), seats.end(), [](const auto& seat) { return seat.kind == galactic::SeatKind::human; }) != 1) return;
        for (auto at = due_orders.begin(); at != due_orders.end(); ++at) {
            const auto* choice = std::get_if<galactic::BattleChoice>(&at->payload);
            if (!choice || !choice->tactical || choice->conflict != pending->id) continue;
            if (prebattle_saved_ == pending->id) return; // second pass: the save already ran
            prebattle_saved_ = pending->id;
            if (std::exchange(skip_next_prebattle_autosave_, false)) return;
            LiveSaveRequest request;
            request.seat = at->seat; request.name = "Auto Save"; request.overwrite = true;
            request.autosave = true; request.prebattle = true;
            LiveGameOrder save{at->seat, galactic::SaveRequest{0, false, true, true}, tick};
            {
                const std::lock_guard lock(mutex_);
                const bool busy = std::any_of(save_outcomes_.begin(), save_outcomes_.end(), [](const auto& row) {
                    return row.second.phase == LiveSavePhase::requested || row.second.phase == LiveSavePhase::captured;
                });
                if (busy || stopping_ || ended_ || save_token_ >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return;
                const auto token = ++save_token_;
                std::get<galactic::SaveRequest>(save.payload).slot = static_cast<std::int64_t>(token);
                save_outcomes_.emplace(token, LiveSaveOutcome{token, frame_.session_tick, request.name, LiveSavePhase::requested, {}});
                save_requests_.emplace(token, std::move(request));
            }
            at->tick = tick + 1; // the choice waits one tick behind the save
            kept_orders.push_back(std::move(*at));
            due_orders.erase(at);
            due_orders.push_back(std::move(save));
            return;
        }
    }

    // GCMP-03 (#4359): the lockstep variant of loop(). The agreed frontier, never a local stamp or clock, decides which
    // tick runs: local orders go to the driver as untimed seat inputs, the driver steps exactly what the frontier
    // releases (including while the galaxy is paused: pause and speed are agreed commands, and a paused session still
    // has to execute its resume). The schedule below only decides WHEN a step may be requested (ADR-013), from the
    // agreed galaxy clock. Singleplayer never enters this function.
    void lockstep_loop() {
        const ThreadWorkerAdapter executor(options_.workers, ThreadWorkerAdapter::Dispatch::by_cost);
        // A paused lockstep session keeps ticking at the normal rate so the resume can be agreed.
        const auto interval_for = [&](const auto& clock) {
            return tick_interval(clock.pause_depth != 0 ? sim::math::Fixed::from_raw(sim::math::Fixed::scale)
                                                        : clock.strategic_speed);
        };
        // The child battle's Begin/return barrier is GCMP-05 (#4361); until then a battle must not run on a local pacer.
        const auto child_refusal = [&]() -> std::optional<core::Diagnostic> {
            if (session_->mode() != campaign::SessionMode::battle && !session_->conflict()) return std::nullopt;
            return diagnostic("EAWR-PLATFORM-0600",
                "A child battle opened in a lockstep game; its agreed Begin/return barrier is not available yet (GCMP-05, #4361).");
        };
        LivePaceSchedule schedule;
        schedule.rebase(Clock::now(), interval_for(session_->galaxy().clock()));
        std::vector<LiveGameOrder> orders;
        const std::vector<galactic::GameCommand> no_commands;
        const std::vector<LiveGameOrder> no_orders; // lockstep captures no local input into a save yet (GCMP-17)
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                // Like the tactical step driver, poll about every 2 ms so the transport is serviced while no tick is due.
                wake_.wait_for(lock, std::chrono::milliseconds(2), [&] {
                    return stopping_ || !order_inbox_.empty() || !command_inbox_.empty();
                });
                if (stopping_) return;
                std::move(order_inbox_.begin(), order_inbox_.end(), std::back_inserter(orders));
                order_inbox_.clear();
                // A pre-stamped command is a local stamp; under lockstep only the sealed batches stamp.
                for (const auto& command : command_inbox_) {
                    (void)command;
                    rejected_.push_back(diagnostic("EAWR-PLATFORM-0600", "A lockstep live game does not accept pre-stamped commands."));
                }
                command_inbox_.clear();
            }
            if (const auto refusal = child_refusal()) {
                { const std::lock_guard lock(mutex_); failure_ = *refusal; }
                publish_input_outcomes({}, {});
                return;
            }
            const auto tick = session_->completed_tick();
            bool due = false;
            {
                const auto now = Clock::now();
                const auto interval = interval_for(session_->galaxy().clock());
                if (interval != schedule.interval()) schedule.rebase(now, interval);
                if (options_.pacing == Pacing::driven) {
                    const std::lock_guard lock(mutex_);
                    due = target_ > tick;
                } else {
                    due = now >= schedule.next_due();
                    if (due) schedule.drop_backlog(now, options_.max_catch_up);
                }
            }
            std::vector<core::Diagnostic> rejected;
            std::vector<LiveBattleChoiceResponse> choice_responses;
            std::chrono::nanoseconds source_time{};
            LiveGameStepContext context{*session_, executor, due, {}, [&](core::Diagnostic error) { rejected.push_back(std::move(error)); }};
            if (options_.command_source)
                context.command_source = [&](const std::uint64_t next_tick) {
                    const auto began = Clock::now();
                    auto sourced = options_.command_source(next_tick);
                    source_time += Clock::now() - began;
                    return sourced;
                };
            {
                const std::lock_guard state_lock(state_mutex_);
                { const std::lock_guard lock(mutex_); if (stopping_) return; }
                const auto step_started = Clock::now();
                auto driven = options_.step_driver->step(context, orders, static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(step_started.time_since_epoch()).count()));
                const auto step_done = Clock::now();
                if (!driven) {
                    const std::lock_guard lock(mutex_);
                    failure_ = driven.error();
                } else if (driven.value()) {
                    if (const auto refusal = child_refusal()) {
                        const std::lock_guard lock(mutex_);
                        failure_ = *refusal;
                    } else {
                        auto stepped = core::Result<campaign::GameTick>::success(std::move(*driven.value()));
                        std::unique_lock lock(mutex_);
                        publish_completed_tick(stepped, step_started, step_started + source_time, step_done,
                            no_orders, no_commands, choice_responses, lock);
                        schedule.stepped();
                    }
                }
            }
            orders.clear();
            if (!publish_input_outcomes(rejected, choice_responses)) return;
            published_.notify_all();
        }
    }

    void loop() {
        {
            const auto recording = session_->recording();
            // The newest restore prologue decides: this session came from the reserved autosave.
            const auto* load = recording.load_fixups.empty() ? nullptr
                : std::get_if<galactic::PostLoadFixup>(&recording.load_fixups.back().payload);
            skip_next_prebattle_autosave_ = load && load->autosave;
        }
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !options_.await_presentation || presentation_ready_; });
            if (stopping_) return;
        }
        if (options_.step_driver) { lockstep_loop(); return; }
        const ThreadWorkerAdapter executor(options_.workers, ThreadWorkerAdapter::Dispatch::by_cost);
        // ADR-013: speed only sets the interval of this wall-clock schedule (LivePaceSchedule).
        LivePaceSchedule schedule;
        schedule.rebase(Clock::now(), tick_interval(frame_.latest->clock().strategic_speed));
        std::vector<LiveGameOrder> orders = std::move(restored_input_.orders);
        std::vector<galactic::GameCommand> commands = std::move(restored_input_.commands);
        for (;;) {
            const auto tick = session_->completed_tick();
            const auto clock = session_->galaxy().clock();
            const bool in_battle = session_->mode() == campaign::SessionMode::battle;
            // The conflict manager can service its resolver under its own
            // nested pause. GameSession keeps movement and fiscal time frozen;
            // only the ordered outer session clock advances (GFD-26).
            const bool resolver_service = !in_battle && session_->auto_resolve_needs_service();
            bool paused = clock.pause_depth != 0 && !resolver_service;
            const auto due_order = [tick](const LiveGameOrder& order) { return !order.tick || *order.tick <= tick; };
            const auto due_command = [tick](const galactic::GameCommand& command) { return command.key.tick <= tick; };
            bool commanded = false;
            bool paced = false;
            std::uint32_t battle_rate = 30;
            if (!await_tick_input(tick, in_battle, paused, due_order, due_command, orders, commands,
                schedule, battle_rate, commanded, paced)) return;
            if (!commanded && !paced) continue;

            std::vector<core::Diagnostic> rejected;
            std::vector<LiveBattleChoiceResponse> choice_responses;
            const auto reject = [&](galactic::SeatId seat, const galactic::CommandPayload& payload, const core::Diagnostic& error) {
                rejected.push_back(error);
                if (const auto* choice = std::get_if<galactic::BattleChoice>(&payload))
                    choice_responses.push_back({choice->conflict, 0, seat, error.message});
                if (const auto* save = std::get_if<galactic::SaveRequest>(&payload)) {
                    const std::lock_guard lock(mutex_);
                    const auto token = static_cast<std::uint64_t>(save->slot);
                    const auto request = save_requests_.find(token);
                    if (request != save_requests_.end() && request->second.seat == seat) {
                        auto& outcome = save_outcomes_.at(token); outcome.phase = LiveSavePhase::rejected;
                        outcome.diagnostic = error; save_requests_.erase(request);
                    }
                }
            };
            bool accepted = false;
            const bool current_input = std::any_of(orders.begin(), orders.end(), [tick](const auto& order) {
                return !order.tick || *order.tick == tick;
            }) || std::any_of(commands.begin(), commands.end(), [tick](const auto& command) {
                return command.key.tick == tick;
            });
            // No polling a callback while paused: explicit input is the wake-up mechanism.
            const bool stepping = paced || (paused && current_input);
            const auto step_started = Clock::now();
            if (stepping && options_.command_source) {
                auto sourced = options_.command_source(tick);
                std::move(sourced.begin(), sourced.end(), std::back_inserter(commands));
            }
            const auto source_done = Clock::now();
            std::function<std::vector<sim::tactical::PlayerCommand>(std::uint64_t)> battle_source;
            const std::lock_guard source_lock(source_mutex_);
            {
                const std::lock_guard lock(mutex_);
                if (in_battle) battle_source = battle_source_;
            }
            if (stepping && battle_source) {
                const auto child_tick = session_->battle()->completed_tick();
                for (auto& command : battle_source(child_tick)) {
                    auto payload = tactical_payload(*session_->conflict(), command);
                    orders.push_back({battle_seat_, std::move(payload), tick});
                }
            }
            std::vector<galactic::GameCommand> due_commands;
            std::vector<galactic::GameCommand> kept_commands;
            for (auto& command : commands) {
                const bool due = due_command(command) && (stepping || command.key.tick < tick);
                (due ? due_commands : kept_commands).push_back(std::move(command));
            }
            commands = std::move(kept_commands);
            // Sort indices, not variant payloads: avoids the compiler's variant-swap
            // warning and moves each queued command only once through this boundary.
            std::vector<std::size_t> command_order(due_commands.size());
            std::iota(command_order.begin(), command_order.end(), std::size_t{0});
            std::stable_sort(command_order.begin(), command_order.end(), [&](const auto a, const auto b) {
                return due_commands[a].key < due_commands[b].key;
            });
            std::vector<LiveGameOrder> due_orders;
            std::vector<LiveGameOrder> kept_orders;
            for (auto& order : orders) {
                const bool due = due_order(order) && (stepping || (order.tick && *order.tick < tick));
                (due ? due_orders : kept_orders).push_back(std::move(order));
            }
            orders = std::move(kept_orders);
            if (stepping && !in_battle) request_prebattle_autosave(tick, due_orders, orders);
            {
                const std::lock_guard state_lock(state_mutex_);
                { const std::lock_guard lock(mutex_); if (stopping_) return; }
                submit_pending_input(tick, due_commands, command_order, due_orders, reject, accepted);
                if (accepted || paced) {
                    if (!in_battle) {
                        for (const auto& entry : options_.entries) {
                            if (entry.tick != tick) continue;
                            if (auto staged = session_->stage_battle(entry.conflict, entry.content, entry.script_requirement); !staged) {
                                const std::lock_guard lock(mutex_);
                                failure_ = staged.error();
                                return;
                            }
                        }
                    }
                    auto stepped = session_->step(executor);
                    const auto step_done = Clock::now();
                    std::unique_lock lock(mutex_);
                    if (!stepped) failure_ = stepped.error();
                    else {
                        publish_completed_tick(stepped, step_started, source_done, step_done, orders, commands, choice_responses, lock);
                    }
                }
            }
            if (!publish_input_outcomes(rejected, choice_responses)) return;
            published_.notify_all();
            advance_schedule(accepted, paced, clock, battle_rate, schedule);
        }
    }

    std::variant<std::monostate, campaign::GameSession, campaign::CampaignRoot> owner_;
    campaign::GameSession* session_;
    std::optional<galactic::GalacticReplay> ended_record_;
    std::map<galactic::SeatId, std::shared_ptr<const CampaignEndView>> campaign_end_publications_;
    std::shared_ptr<const CampaignEndView> default_end_;
    Options options_;
    mutable std::mutex mutex_;
    mutable std::mutex state_mutex_;
    std::mutex join_mutex_;
    std::mutex source_mutex_;
    mutable std::condition_variable published_;
    std::condition_variable wake_;
    LiveGameFrame frame_;
    std::map<galactic::SeatId, std::shared_ptr<const std::vector<campaign::GameStoryNotice>>> story_notice_publications_;
    std::shared_ptr<const PreparedCampaignBattle> prepared_battle_;
    sim::EntityId prepared_conflict_{};
    CampaignLoadingPublication loading_;
    std::shared_ptr<const campaign::AutoResolveOutcomeApplication> published_outcome_;
    std::shared_ptr<const LiveAutoResolveOutcome> published_outcome_view_;
    std::map<sim::SeatId, LiveBattleChoiceResponse> choice_responses_;
    sim::EntityId prebattle_saved_{};            // simulation thread only (WGS-142)
    bool skip_next_prebattle_autosave_{};        // restored from the autosave itself (WGS-142)
    std::uint64_t choice_response_revision_{};
    LiveFrame battle_frame_;
    std::shared_ptr<const campaign::ConflictRecord> active_conflict_;
    campaign::InvasionOfferPublication invasion_publication_;
    std::uint64_t invasion_selection_revision_{};
    sim::EntityId summary_closed_{};
    galactic::SeatId summary_closed_seat_{};
    std::deque<std::shared_ptr<const sim::tactical::TacticalSnapshot>> battle_snapshots_;
    LiveEventLog battle_events_{LiveEventLog::Bounds{}};
    std::uint64_t galactic_cursor_{};
    std::deque<LiveGalacticEvent> galactic_events_;
    std::vector<galactic::GalacticFeedbackNotice> pending_feedback_; // simulation thread only
    GalacticApproachThrottle approach_throttle_;
    std::map<galactic::PlanetId, galactic::PlayerId> published_owners_;
    std::map<sim::tactical::PlayerId, std::deque<std::shared_ptr<const LiveFog>>> battle_fog_;
    std::vector<sim::StateHash> battle_hashes_;
    std::vector<sim::tactical::TypeId> battle_base_types_;
    sim::EntityId ready_conflict_{};
    sim::galactic::SeatId battle_seat_{};
    bool battle_paused_{true};
    std::uint32_t battle_rate_{30};
    std::deque<LiveTickCost> costs_;
    std::function<std::vector<sim::tactical::PlayerCommand>(std::uint64_t)> battle_source_;
    std::vector<LiveGameOrder> order_inbox_;
    std::vector<galactic::GameCommand> command_inbox_;
    std::map<galactic::SeatId, std::uint64_t> sequences_;
    std::vector<sim::StateHash> hashes_;
    std::vector<core::Diagnostic> rejected_;
    std::optional<core::Diagnostic> failure_;
    std::uint64_t target_{};
    bool stopping_{};
    bool ended_{};
    bool presentation_ready_{};
    std::thread thread_;
    struct DiskSave { std::uint64_t token{}; LiveSaveRequest request; std::vector<std::uint8_t> bytes; };
    std::thread disk_thread_;
    std::condition_variable disk_wake_;
    bool disk_stopping_{};
    std::deque<DiskSave> disk_saves_;
    std::uint64_t save_token_{};
    std::map<std::uint64_t, LiveSaveRequest> save_requests_;
    std::map<std::uint64_t, LiveSaveOutcome> save_outcomes_;
    save_detail::Input restored_input_;

    static galactic::TacticalLayerCommand tactical_payload(const campaign::ConflictRecord& conflict,
        const sim::tactical::PlayerCommand& command) {
        sim::tactical::TacticalReplay replay;
        replay.setup.players = conflict.tactical.players;
        replay.final_tick_count = command.key.tick + 1;
        replay.commands.push_back(command);
        const auto encoded = sim::tactical::write_replay(replay);
        if (!encoded) throw std::runtime_error(core::format_diagnostic(encoded.error()));
        const auto& bytes = encoded.value();
        const auto header = static_cast<std::size_t>(bytes[10]) | (static_cast<std::size_t>(bytes[11]) << 8U);
        const auto offset = header + replay.setup.players.size() * 24 + 4;
        return {conflict.id, bytes[offset + 20], {bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end()}};
    }
};

class LiveGameSession::Impl::Feed final : public TacticalFeed {
public:
    Feed(std::shared_ptr<Impl> owner, std::shared_ptr<const campaign::ConflictRecord> conflict,
        galactic::SeatId seat, sim::tactical::PlayerId player)
        : owner_(std::move(owner)), conflict_(std::move(conflict)), seat_(seat), player_(player) {}
    bool current() const { return owner_->active_conflict_ && owner_->active_conflict_->id == conflict_->id; }
    bool running() const { return !owner_->stopping_ && current() && owner_->frame_.conflict && owner_->frame_.conflict->id == conflict_->id; }
    void submit(LiveOrder order) override {
        const std::lock_guard lock(owner_->mutex_);
        if (!running() || player_ == 0 || order.issuer != player_) return;
        const auto tick = owner_->battle_frame_.latest->completed_tick();
        if (order.tick && *order.tick < tick) {
            owner_->rejected_.push_back(diagnostic(galactic::diagnostic_codes::late_command, "child tick already executed"));
            return;
        }
        sim::tactical::PlayerCommand command{{order.tick.value_or(tick), player_, 1}, std::move(order.units), std::move(order.payload)};
        // Untimed input belongs to the owner's next inbox drain. The published
        // tick may already be executing when presentation submits an order.
        // Explicit child ticks retain their exact parent-clock mapping.
        const std::optional<std::uint64_t> due = order.tick
            ? std::optional(owner_->frame_.session_tick + (command.key.tick > tick ? command.key.tick - tick : 0))
            : std::nullopt;
        owner_->order_inbox_.push_back({seat_, tactical_payload(*conflict_, command), due});
        owner_->wake_.notify_all();
    }
    void advance_to(std::uint64_t tick) override {
        const std::lock_guard lock(owner_->mutex_);
        if (!running()) return;
        const auto child_tick = owner_->battle_frame_.latest->completed_tick();
        if (tick > child_tick) owner_->target_ = std::max(owner_->target_, owner_->frame_.session_tick + tick - child_tick);
        owner_->wake_.notify_all();
    }
    void set_paused(bool paused) override {
        { const std::lock_guard lock(owner_->mutex_); if (running()) owner_->battle_paused_ = paused
            && !(owner_->battle_frame_.latest && owner_->battle_frame_.latest->outcome()); }
        owner_->wake_.notify_all();
    }
    bool paused() const override {
        const std::lock_guard lock(owner_->mutex_);
        return running() && owner_->battle_paused_;
    }
    void set_target_rate(std::uint32_t rate) override {
        const std::lock_guard lock(owner_->mutex_);
        if (running()) owner_->battle_rate_ = std::clamp(rate, 1U, 1000U);
    }
    // The child ending barrier belongs to GameSession; the view can freeze its
    // results frame while the owner performs the one deterministic return tick.
    void halt_at(std::uint64_t) override {}
    std::optional<std::uint64_t> halt_tick() const override { return {}; }
    bool wait_for(std::uint64_t tick, std::chrono::milliseconds timeout) const override {
        std::unique_lock lock(owner_->mutex_);
        owner_->published_.wait_for(lock, timeout, [&] {
            return !running() || owner_->failure_ || owner_->stopping_
                || (owner_->battle_frame_.latest && owner_->battle_frame_.latest->completed_tick() >= tick);
        });
        return current() && owner_->battle_frame_.latest && owner_->battle_frame_.latest->completed_tick() >= tick;
    }
    LiveFrame frame() const override {
        const std::lock_guard lock(owner_->mutex_);
        return current() ? owner_->battle_frame_ : LiveFrame{};
    }
    std::shared_ptr<const sim::tactical::TacticalSnapshot> snapshot_at(std::uint64_t tick) const override {
        const std::lock_guard lock(owner_->mutex_);
        if (current()) for (const auto& snapshot : owner_->battle_snapshots_) if (snapshot->completed_tick() == tick) return snapshot;
        return {};
    }
    std::shared_ptr<const LiveFog> fog_at(std::uint64_t tick) const override {
        const std::lock_guard lock(owner_->mutex_);
        if (current()) if (const auto found = owner_->battle_fog_.find(player_); found != owner_->battle_fog_.end())
            for (const auto& fog : found->second) if (fog->tick == tick) return fog;
        return {};
    }
    LiveEvents events_after(std::uint64_t after, std::uint64_t through) const override {
        const std::lock_guard lock(owner_->mutex_);
        return current() ? owner_->battle_events_.after(after, through) : LiveEvents{};
    }
    std::vector<LiveTickCost> tick_costs_after(std::uint64_t) const override { return {}; }
    std::uint64_t completed_tick() const override { const auto value = frame(); return value.latest ? value.latest->completed_tick() : 0; }
    std::optional<core::Diagnostic> failure() const override { const std::lock_guard lock(owner_->mutex_); return owner_->failure_; }
    std::vector<core::Diagnostic> rejected_orders() const override { const std::lock_guard lock(owner_->mutex_); return owner_->rejected_; }
    std::size_t worker_count() const noexcept override { return owner_->options_.workers; }
    LiveScriptReport script_report() const override { return {}; }
    void stop() override {
        // Wait for a callback already in flight before its view/scheduler dies.
        const std::lock_guard source_lock(owner_->source_mutex_);
        const std::lock_guard lock(owner_->mutex_);
        if (current() && owner_->battle_seat_ == seat_) owner_->battle_source_ = {};
    }
    std::vector<std::string> tick_hashes() const override {
        const std::lock_guard lock(owner_->mutex_);
        std::vector<std::string> result;
        if (current()) for (const auto& hash : owner_->battle_hashes_) result.push_back(hash.get());
        return result;
    }
    sim::tactical::TacticalReplay record() const override {
        const std::lock_guard state_lock(owner_->state_mutex_);
        const std::lock_guard lock(owner_->mutex_);
        if (current() && owner_->session_) if (auto replay = owner_->session_->battle_record()) return *replay;
        return {conflict_->tactical, 0, {}};
    }
    sim::tactical::TacticalReplay failure_record() const override { return record(); }
    std::optional<bool> reinforcement_point(sim::tactical::PlayerId player, sim::tactical::TypeId type,
        const sim::math::Vec3& point, std::optional<sim::math::Fixed> facing) const override {
        std::unique_lock state_lock(owner_->state_mutex_, std::try_to_lock);
        if (!state_lock.owns_lock()) return {};
        const std::lock_guard lock(owner_->mutex_);
        const auto* world = running() ? owner_->session_->battle() : nullptr;
        if (!world || player != player_ || player_ == 0) return false;
        const auto result = world->reinforcement_point(player, type, point, nullptr, facing);
        return result ? std::optional(result.value()) : std::optional(false);
    }
    LiveSession::ProductionView production_view(sim::tactical::PlayerId player, sim::tactical::TypeId type) const override {
        const std::lock_guard state_lock(owner_->state_mutex_);
        const std::lock_guard lock(owner_->mutex_);
        const auto* world = running() ? owner_->session_->battle() : nullptr;
        if (!world || player != player_) return {};
        return {world->production_counts(player, type), world->snapshot()};
    }
    void command_source(std::function<std::vector<sim::tactical::PlayerCommand>(std::uint64_t)> source) override {
        const std::lock_guard source_lock(owner_->source_mutex_);
        const std::lock_guard lock(owner_->mutex_);
        if (running() && player_ != 0) { owner_->battle_seat_ = seat_; owner_->battle_source_ = std::move(source); }
    }
    void configure_notifications(std::vector<sim::tactical::TypeId> types) override {
        const std::lock_guard lock(owner_->mutex_);
        if (running()) owner_->battle_base_types_ = std::move(types);
    }
private:
    std::shared_ptr<Impl> owner_;
    std::shared_ptr<const campaign::ConflictRecord> conflict_;
    galactic::SeatId seat_{};
    sim::tactical::PlayerId player_{};
};

std::shared_ptr<TacticalFeed> LiveGameSession::battle_feed(sim::EntityId conflict, galactic::SeatId seat) {
    const std::lock_guard lock(impl_->mutex_);
    const auto record = impl_->frame_.conflict;
    if (!record || record->id != conflict) return {};
    const auto binding = std::find_if(record->seats.begin(), record->seats.end(), [seat](const auto& row) { return row.seat == seat; });
    if (binding == record->seats.end()) return {};
    return std::make_shared<Impl::Feed>(impl_, record, seat, binding->player);
}
void LiveGameSession::battle_ready(sim::EntityId conflict) {
    { const std::lock_guard lock(impl_->mutex_);
        const auto loading = impl_->loading_.view();
        if (impl_->frame_.conflict && impl_->frame_.conflict->id == conflict
            && (!loading || loading->outcome == CampaignLoadingOutcome::succeeded)) impl_->ready_conflict_ = conflict;
    }
    impl_->wake_.notify_all();
}

std::chrono::nanoseconds LiveGameSession::tick_interval(const sim::math::Fixed speed) noexcept {
    const auto raw = std::clamp(speed.raw(), std::int64_t{1}, galactic::max_speed_factor_raw);
    return std::chrono::nanoseconds(std::max(std::int64_t{1},
        (std::int64_t{1'000'000'000} * sim::math::Fixed::scale) / (galactic::galactic_frames_per_second * raw)));
}

core::Result<std::unique_ptr<LiveGameSession>> LiveGameSession::start(const galactic::GalacticSetup& setup, Options options) {
    using Result = core::Result<std::unique_ptr<LiveGameSession>>;
    if (options.workers == 0 || options.workers > ThreadWorkerAdapter::max_worker_count
        || options.readiness_timeout <= std::chrono::milliseconds::zero()) {
        return Result::failure(diagnostic(galactic::diagnostic_codes::worker_failure, "invalid live game worker count or readiness timeout"));
    }
    std::optional<std::uint64_t> previous_entry;
    std::set<sim::EntityId> conflict_ids;
    for (const auto& entry : options.entries) {
        if (entry.tick >= galactic::galactic_max_ticks || (previous_entry && entry.tick <= *previous_entry)
            || !conflict_ids.insert(entry.conflict.id).second) {
            return Result::failure(diagnostic(galactic::diagnostic_codes::invalid_setup,
                "live conflict entries need unique IDs and strictly increasing in-range ticks"));
        }
        previous_entry = entry.tick;
    }
    if (const auto refusal = lockstep_options_refusal(options)) return Result::failure(*refusal);
    auto created = campaign::GameSession::create(setup);
    if (!created) return Result::failure(created.error());
    try {
        if (options.services && options.services->configure) {
            auto configured = options.services->configure(created.value());
            if (!configured) return Result::failure(configured.error());
        }
        if (options.services && options.services->configure_native_ai) {
            auto configured = options.services->configure_native_ai(created.value());
            if (!configured) return Result::failure(configured.error());
        }
        auto impl = std::make_shared<Impl>(std::move(created).value(), std::move(options));
        impl->start();
        return Result::success(std::unique_ptr<LiveGameSession>(new LiveGameSession(std::move(impl))));
    } catch (const std::exception& error) {
        return Result::failure(diagnostic(galactic::diagnostic_codes::worker_failure, error.what()));
    } catch (...) {
        return Result::failure(diagnostic(galactic::diagnostic_codes::worker_failure, "live game initialization threw"));
    }
}

core::Result<std::unique_ptr<LiveGameSession>> LiveGameSession::start(campaign::CampaignRoot root, Options options) {
    using Result = core::Result<std::unique_ptr<LiveGameSession>>;
    if (!root.game() || !root.plan() || root.game()->completed_tick() != 0)
        return Result::failure(diagnostic(galactic::diagnostic_codes::invalid_setup, "live campaign needs an initialized tick-zero root"));
    if (options.workers == 0 || options.workers > ThreadWorkerAdapter::max_worker_count
        || options.readiness_timeout <= std::chrono::milliseconds::zero())
        return Result::failure(diagnostic(galactic::diagnostic_codes::worker_failure, "invalid live game worker count or readiness timeout"));
    std::optional<std::uint64_t> previous_entry;
    std::set<sim::EntityId> conflict_ids;
    for (const auto& entry : options.entries) {
        if (entry.tick >= galactic::galactic_max_ticks || (previous_entry && entry.tick <= *previous_entry)
            || !conflict_ids.insert(entry.conflict.id).second)
            return Result::failure(diagnostic(galactic::diagnostic_codes::invalid_setup,
                "live conflict entries need unique IDs and strictly increasing in-range ticks"));
        previous_entry = entry.tick;
    }
    if (const auto refusal = lockstep_options_refusal(options)) return Result::failure(*refusal);
    try {
        if (options.services && options.services->configure) {
            auto configured = options.services->configure(*root.game());
            if (!configured) return Result::failure(configured.error());
        }
        if (options.services && options.services->configure_native_ai) {
            auto configured = options.services->configure_native_ai(*root.game());
            if (!configured) return Result::failure(configured.error());
        }
        auto impl = std::make_shared<Impl>(std::move(root), std::move(options));
        impl->start();
        return Result::success(std::unique_ptr<LiveGameSession>(new LiveGameSession(std::move(impl))));
    } catch (const std::exception& error) {
        return Result::failure(diagnostic(galactic::diagnostic_codes::worker_failure, error.what()));
    } catch (...) {
        return Result::failure(diagnostic(galactic::diagnostic_codes::worker_failure, "live game initialization threw"));
    }
}

core::Result<std::unique_ptr<LiveGameSession>> LiveGameSession::start(
    std::unique_ptr<PreparedLiveCampaign> prepared, Options options) {
    if (!prepared) return core::Result<std::unique_ptr<LiveGameSession>>::failure(
        diagnostic(galactic::diagnostic_codes::invalid_setup, "live campaign owner is missing"));
    if (!options.services) options.services = prepared->impl_->services;
    if (!options.saves.campaign) options.saves.campaign = std::move(prepared->impl_->saved_campaign);
    return start(std::move(prepared->impl_->root), std::move(options));
}
LiveGameSession::LiveGameSession(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
core::Result<std::unique_ptr<LiveGameSession>> LiveGameSession::start(
    std::unique_ptr<PreparedLiveRestore> prepared, Options options) {
    using Result = core::Result<std::unique_ptr<LiveGameSession>>;
    if (!prepared || options.workers == 0 || options.workers > ThreadWorkerAdapter::max_worker_count
        || options.readiness_timeout <= std::chrono::milliseconds::zero() || !options.entries.empty())
        return Result::failure(diagnostic(galactic::diagnostic_codes::invalid_setup, "Invalid restored live owner/options."));
    // Agreed multiplayer saves and resume are GCMP-17; a restored session keeps its local pacer.
    if (options.step_driver)
        return Result::failure(diagnostic("EAWR-PLATFORM-0600", "A restored live game cannot run under a lockstep driver yet."));
    try {
        // Restore already installed fresh adapters and applied the explicit player prologue.
        options.services = prepared->impl_->services;
        if (options.saves.directory.empty()) options.saves = prepared->impl_->saves;
        if (!options.saves.campaign) options.saves.campaign = prepared->impl_->saves.campaign; // #3035
        auto impl = std::make_shared<Impl>(std::move(prepared->impl_->restored.session), std::move(options));
        impl->restored_input_ = std::move(prepared->impl_->input);
        impl->start();
        return Result::success(std::unique_ptr<LiveGameSession>(new LiveGameSession(std::move(impl))));
    } catch (const std::exception& error) {
        return Result::failure(diagnostic(galactic::diagnostic_codes::worker_failure, error.what()));
    }
}
bool LiveGameSession::battle_paused(sim::EntityId conflict) const {
    const std::lock_guard lock(impl_->mutex_);
    return impl_->frame_.conflict && impl_->frame_.conflict->id == conflict && impl_->battle_paused_;
}
bool LiveGameSession::battle_is_ready(sim::EntityId conflict) const {
    const std::lock_guard lock(impl_->mutex_);
    return impl_->frame_.conflict && impl_->frame_.conflict->id == conflict && impl_->ready_conflict_ == conflict;
}
void LiveGameSession::reconcile_battle_summary(galactic::SeatId observer, sim::EntityId acknowledged) {
    const std::lock_guard lock(impl_->mutex_);
    if (!impl_->options_.await_presentation || impl_->presentation_ready_ || !acknowledged
        || impl_->frame_.summary_hold != acknowledged) return;
    impl_->order_inbox_.push_back({observer, galactic::BattleSummaryAcknowledge{acknowledged}, {}});
}
void LiveGameSession::presentation_ready() {
    { const std::lock_guard lock(impl_->mutex_); impl_->presentation_ready_ = true; }
    impl_->wake_.notify_all();
}
LiveGameSession::~LiveGameSession() { stop(); }
std::uint64_t LiveGameSession::request_save(LiveSaveRequest request) {
    const std::lock_guard lock(impl_->mutex_);
    if (impl_->save_token_ >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return 0;
    const auto token = ++impl_->save_token_;
    LiveSaveOutcome outcome{token, impl_->frame_.session_tick, request.name, LiveSavePhase::requested, {}};
    const bool busy = std::any_of(impl_->save_outcomes_.begin(), impl_->save_outcomes_.end(), [](const auto& row) {
        return row.second.phase == LiveSavePhase::requested || row.second.phase == LiveSavePhase::captured;
    });
    if (impl_->options_.step_driver) {
        outcome.phase = LiveSavePhase::rejected;
        outcome.diagnostic = diagnostic("EAWR-PLATFORM-0600",
            "Saving a lockstep live game requires agreed checkpoint input (GCMP-17, #4369).");
    } else if (impl_->stopping_ || impl_->ended_ || busy || impl_->options_.saves.directory.empty() || !save_detail::valid_name(request.name)) {
        outcome.phase = LiveSavePhase::rejected;
        outcome.diagnostic = diagnostic("EAWR-SAVE-0103", "Save unavailable, already in progress, or name invalid.");
    } else {
        impl_->order_inbox_.push_back({request.seat, galactic::SaveRequest{static_cast<std::int64_t>(token),
            request.quick, request.autosave, request.prebattle}, {}});
        impl_->save_requests_.emplace(token, std::move(request));
    }
    impl_->save_outcomes_.emplace(token, std::move(outcome));
    // Bound completed outcome history; never discard an in-flight request.
    while (impl_->save_outcomes_.size() > 128) {
        const auto old = std::find_if(impl_->save_outcomes_.begin(), impl_->save_outcomes_.end(), [](const auto& row) {
            return row.second.phase != LiveSavePhase::requested && row.second.phase != LiveSavePhase::captured;
        });
        if (old == impl_->save_outcomes_.end()) break;
        impl_->save_outcomes_.erase(old);
    }
    impl_->wake_.notify_all(); impl_->published_.notify_all(); return token;
}
std::vector<LiveSaveOutcome> LiveGameSession::save_outcomes() const {
    const std::lock_guard lock(impl_->mutex_); std::vector<LiveSaveOutcome> result;
    for (const auto& [token, outcome] : impl_->save_outcomes_) { (void)token; result.push_back(outcome); }
    return result;
}
bool LiveGameSession::wait_save(std::uint64_t token, std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mutex_);
    const auto terminal = [&] {
        const auto found = impl_->save_outcomes_.find(token);
        return found != impl_->save_outcomes_.end() && found->second.phase != LiveSavePhase::requested
            && found->second.phase != LiveSavePhase::captured;
    };
    impl_->published_.wait_for(lock, timeout, [&] { return terminal() || impl_->failure_; }); return terminal();
}
core::Result<std::vector<LiveSaveSlot>> LiveGameSession::save_slots() const {
    return save_detail::list(impl_->options_.saves.directory);
}
core::Result<void> LiveGameSession::delete_save(std::string_view name) {
    return save_detail::erase(impl_->options_.saves.directory, name);
}
core::Result<std::unique_ptr<PreparedLiveRestore>> LiveGameSession::prepare_load(std::string_view name, bool player_load) const {
    using Result = core::Result<std::unique_ptr<PreparedLiveRestore>>;
    try {
        std::vector<std::uint8_t> bytes;
        auto read = save_detail::read(impl_->options_.saves.directory, name);
        if (!read) { return Result::failure(read.error()); }
        bytes = std::move(read).value();
        auto container = checkpoint::parse_checkpoint(bytes);
        if (!container) return Result::failure(container.error());
        auto input = save_detail::decode_input(container.value());
        if (!input) return Result::failure(input.error());
        checkpoint::Identities identities;
        std::shared_ptr<const CampaignServiceConfiguration> services;
        std::vector<Options::Entry> entries;
        { const std::lock_guard lock(impl_->state_mutex_);
            if (!impl_->session_) return Result::failure(diagnostic("EAWR-SAVE-0104", "Ended campaign has no live load owner."));
            identities = checkpoint::game_identities(impl_->session_->galaxy().setup());
            services = impl_->options_.services; entries = impl_->options_.entries; }
        identities.mod_list = impl_->options_.saves.mod_list_identity;
        auto options = services ? services->restore : checkpoint::GameLoadOptions{};
        if (!options.configure_native_ai && services)
            options.configure_native_ai = services->configure_native_ai;
        if (options.content == nullptr && !entries.empty()) {
            if (std::all_of(entries.begin(), entries.end(), [](const auto& entry) {
                return entry.script_requirement == campaign::ScriptRequirement::none;
            })) options.script_requirement = campaign::ScriptRequirement::none;
            options.content = [entries](const campaign::ConflictRecord& conflict) {
                const auto at = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.conflict.id == conflict.id; });
                if (at == entries.end()) return core::Result<sim::tactical::TacticalContent>::failure(
                    diagnostic("EAWR-SAVE-0104", "Saved child content is unavailable."));
                return core::Result<sim::tactical::TacticalContent>::success(at->content);
            };
        }
        options.player_load = player_load;
        options.reserved_autosave = player_load && name == "Auto Save"; // WGS-64/142: marks the restore prologue
        const ThreadWorkerAdapter executor(impl_->options_.workers);
        auto restored = checkpoint::load_game_checkpoint(bytes, identities, options, executor, name);
        if (!restored) return Result::failure(restored.error());
        auto retained = services ? std::make_shared<CampaignServiceConfiguration>(*services)
            : std::make_shared<CampaignServiceConfiguration>();
        retained->restore = std::move(options);
        return Result::success(std::unique_ptr<PreparedLiveRestore>(new PreparedLiveRestore(std::make_unique<PreparedLiveRestore::Impl>(
            std::move(restored).value(), std::move(input).value(), std::move(retained), impl_->options_.saves))));
    } catch (const std::exception& error) { return Result::failure(diagnostic("EAWR-SAVE-0104", error.what())); }
}
void LiveGameSession::submit(LiveGameOrder order) {
    { const std::lock_guard lock(impl_->mutex_); if (impl_->stopping_ || impl_->ended_) return; impl_->order_inbox_.push_back(std::move(order)); }
    impl_->wake_.notify_all();
}
void LiveGameSession::submit(galactic::GameCommand command) {
    { const std::lock_guard lock(impl_->mutex_); if (impl_->stopping_ || impl_->ended_) return; impl_->command_inbox_.push_back(std::move(command)); }
    impl_->wake_.notify_all();
}
void LiveGameSession::advance_to(const std::uint64_t tick) {
    { const std::lock_guard lock(impl_->mutex_); impl_->target_ = std::max(impl_->target_, tick); }
    impl_->wake_.notify_all();
}
bool LiveGameSession::wait_for(const std::uint64_t tick, const std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mutex_);
    impl_->published_.wait_for(lock, timeout, [&] { return impl_->frame_.session_tick >= tick || impl_->ended_ || impl_->stopping_ || impl_->failure_; });
    return impl_->frame_.session_tick >= tick;
}
LiveGameFrame LiveGameSession::frame() const {
    const std::lock_guard lock(impl_->mutex_);
    auto result = impl_->frame_; result.campaign_end = impl_->default_end_; return result;
}
LiveGameFrame LiveGameSession::frame(galactic::SeatId observer) const {
    const std::lock_guard lock(impl_->mutex_);
    auto result = impl_->frame_;
    result.campaign_end.reset();
    if (const auto at = impl_->campaign_end_publications_.find(observer); at != impl_->campaign_end_publications_.end())
        result.campaign_end = at->second;
    result.story_notices.reset();
    if (const auto at = impl_->story_notice_publications_.find(observer); at != impl_->story_notice_publications_.end())
        result.story_notices = at->second;
    if (result.auto_resolve_progress) result.auto_resolve_progress = auto_resolve_progress_for_observer(result, observer);
    return result;
}
std::shared_ptr<const CampaignLoadingView> LiveGameSession::loading(sim::galactic::SeatId seat) const {
    const std::lock_guard lock(impl_->mutex_);
    const auto& frame = impl_->frame_;
    if (!frame.latest || !frame.conflict) return {};
    const auto seats = frame.latest->seats();
    const auto local = std::find_if(seats.begin(), seats.end(), [seat](const auto& row) { return row.seat_id == seat; });
    if (local == seats.end() || !local->active || local->kind != galactic::SeatKind::human) return {};
    const auto binding = std::find_if(frame.conflict->seats.begin(), frame.conflict->seats.end(),
        [seat](const auto& row) { return row.seat == seat; });
    if (binding == frame.conflict->seats.end() || !binding->player || binding->player != local->player) return {};
    return impl_->loading_.view();
}
LiveSaveMenuFacts LiveGameSession::save_menu_facts() const {
    const std::lock_guard lock(impl_->mutex_);
    return {static_cast<bool>(impl_->frame_.pending_conflict), static_cast<bool>(impl_->frame_.conflict)};
}
std::optional<LiveSaveOutcome> LiveGameSession::terminal_save_outcome(std::uint64_t token) const {
    const std::lock_guard lock(impl_->mutex_);
    const auto found = impl_->save_outcomes_.find(token);
    if (found == impl_->save_outcomes_.end() || found->second.phase == LiveSavePhase::requested
        || found->second.phase == LiveSavePhase::captured) return std::nullopt;
    return found->second;
}
LiveGalacticEvents LiveGameSession::galactic_events_after(const std::uint64_t cursor) const {
    const std::lock_guard lock(impl_->mutex_);
    LiveGalacticEvents result;
    result.through = impl_->galactic_cursor_;
    result.incomplete = !impl_->galactic_events_.empty() && cursor < impl_->galactic_events_.front().cursor - 1;
    for (const auto& event : impl_->galactic_events_) if (event.cursor > cursor) result.events.push_back(event);
    return result;
}
std::uint64_t LiveGameSession::completed_tick() const { return frame().session_tick; }

std::vector<LiveTickCost> LiveGameSession::tick_costs_after(const std::uint64_t after) const {
    const std::lock_guard lock(impl_->mutex_);
    std::vector<LiveTickCost> costs;
    for (auto it = impl_->costs_.rbegin(); it != impl_->costs_.rend() && it->tick > after; ++it) costs.push_back(*it);
    std::reverse(costs.begin(), costs.end());
    return costs;
}
std::optional<core::Diagnostic> LiveGameSession::failure() const { const std::lock_guard lock(impl_->mutex_); return impl_->failure_; }
std::vector<core::Diagnostic> LiveGameSession::rejected_orders() const { const std::lock_guard lock(impl_->mutex_); return impl_->rejected_; }
std::size_t LiveGameSession::worker_count() const noexcept { return impl_->options_.workers; }
std::weak_ptr<const void> LiveGameSession::lifetime() const noexcept { return impl_; }
void LiveGameSession::stop() { impl_->stop(); }
core::Result<std::shared_ptr<const CampaignEndView>> LiveGameSession::consume_campaign_result(
    galactic::SeatId observer, bool dialog_active, bool movie_playing) {
    try { return impl_->consume_campaign_result(observer, dialog_active, movie_playing); }
    catch (const std::exception& error) { return core::Result<std::shared_ptr<const CampaignEndView>>::failure(
        diagnostic(galactic::diagnostic_codes::worker_failure, error.what())); }
}
std::vector<std::string> LiveGameSession::tick_hashes() const {
    const std::lock_guard lock(impl_->state_mutex_);
    std::vector<std::string> result;
    result.reserve(impl_->hashes_.size());
    for (const auto& hash : impl_->hashes_) result.push_back(hash.get());
    return result;
}
galactic::GalacticReplay LiveGameSession::record() const {
    const std::lock_guard lock(impl_->state_mutex_);
    return impl_->ended_record_ ? *impl_->ended_record_ : impl_->session_->record();
}

} // namespace eawr::platform
