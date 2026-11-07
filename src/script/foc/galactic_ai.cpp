#include "galactic_ai_internal.hpp"

#include "galactic_ai_lifecycle_state.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/script/foc/ai_diagnostics.hpp"

#include <algorithm>
#include <limits>
#include <exception>

namespace eawr::script::foc::galactic_ai {

Manager::Manager(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Manager::~Manager() = default;
struct Manager::PhaseSavepoint::Saved {
    Manager* owner{};
    Clocks clocks;
    Admission admission;
    ServicePeriods periods;
    std::map<gc::PlayerId, PlayerLifecycle> players;
    std::vector<ServiceTrace> traces;
    bool settled{};
};
Manager::PhaseSavepoint::PhaseSavepoint(std::unique_ptr<Saved> saved) : saved_(std::move(saved)) {}
Manager::PhaseSavepoint::~PhaseSavepoint() {
    if (!saved_ || saved_->settled) return;
    saved_->owner->impl_->phase = nullptr; saved_->owner->impl_->phase_abandoned = true;
    saved_->owner->mark_state_changed();
}
Manager::PhaseSavepoint::Work Manager::PhaseSavepoint::work() const noexcept {
    return saved_ ? Work{saved_->players.size(), saved_->traces.size()} : Work{};
}
core::Result<void> Manager::PhaseSavepoint::commit() {
    if (!saved_ || saved_->settled || saved_->owner->impl_->servicing)
        return core::Result<void>::failure(lifecycle_error("lifecycle phase requires its unsettled idle owner"));
    auto& impl = *saved_->owner->impl_;
    if (impl.failure) return core::Result<void>::failure(*impl.failure);
    if (impl.service_threw) return core::Result<void>::failure(lifecycle_error("lifecycle callback threw before publication"));
    impl.phase = nullptr; saved_->settled = true;
    for (auto& row : saved_->traces) append_ai_diagnostic(impl.traces, impl.traces_dropped, std::move(row));
    saved_->players.clear(); saved_->traces.clear();
    return core::Result<void>::success();
}
core::Result<void> Manager::PhaseSavepoint::rollback() {
    if (!saved_ || saved_->settled || saved_->owner->impl_->servicing)
        return core::Result<void>::failure(lifecycle_error("lifecycle rollback requires its unsettled idle owner"));
    auto& impl = *saved_->owner->impl_;
    for (auto& player : impl.state.players) if (auto old = saved_->players.find(player.selected.player); old != saved_->players.end())
        player = std::move(old->second);
    impl.state.clocks = saved_->clocks; impl.state.admission = saved_->admission; impl.state.service_periods = saved_->periods;
    impl.policy_restored(); impl.reset_records(); impl.failure.reset(); impl.service_threw = false; impl.faulted_provider.reset();
    impl.phase = nullptr; saved_->settled = true; saved_->owner->mark_state_changed();
    saved_->players.clear(); saved_->traces.clear();
    return core::Result<void>::success();
}
core::Result<std::unique_ptr<Manager::PhaseSavepoint>> Manager::begin_phase() {
    using Return = core::Result<std::unique_ptr<PhaseSavepoint>>;
    if (impl_->phase || impl_->phase_abandoned || impl_->servicing || impl_->service_threw || impl_->failure)
        return Return::failure(lifecycle_error("lifecycle phase requires its healthy idle owner"));
    for (const auto& [player, state] : impl_->player_state) {
        static_cast<void>(player);
        if (!state.traces.empty()) return Return::failure(lifecycle_error("lifecycle phase has pending traces"));
    }
    impl_->faulted_provider.reset();
    auto saved = std::make_unique<PhaseSavepoint::Saved>(); saved->owner = this;
    saved->clocks = impl_->state.clocks; saved->admission = impl_->state.admission; saved->periods = impl_->state.service_periods;
    auto phase = std::unique_ptr<PhaseSavepoint>(new PhaseSavepoint(std::move(saved)));
    impl_->phase = phase->saved_.get(); return Return::success(std::move(phase));
}
void Manager::Impl::retain_player(const PlayerLifecycle& player) {
    if (phase) phase->players.try_emplace(player.selected.player, player);
}

void Manager::Impl::policy_changed(gc::PlayerId player) {
    auto& cache = player_state.at(player).policy;
    if (++cache.revision == 0) std::terminate();
    cache.snapshot.reset();
}
void Manager::Impl::policy_restored() {
    for (auto& entry : player_state) policy_changed(entry.first);
}
std::shared_ptr<const TemplatePolicy> Manager::Impl::policy_snapshot(const PlayerLifecycle& player) {
    auto& slice = player_state.at(player.selected.player);
    auto& cache = slice.policy;
    if (cache.snapshot && cache.built_revision == cache.revision) {
#ifndef NDEBUG
        if (*cache.snapshot != player.policy) std::terminate(); // a policy write skipped policy_changed.
#endif
        ++slice.policy_stats.reuses;
        return cache.snapshot;
    }
    gc_profile::Scope profile_scope(gc_profile::Phase::policy_snapshot);
    ++slice.policy_stats.builds;
    cache.snapshot = std::make_shared<const TemplatePolicy>(player.policy);
    cache.built_revision = cache.revision;
    return cache.snapshot;
}

core::Result<std::shared_ptr<Manager>> Manager::create(LifecycleSetup setup, Capabilities capabilities) {
    using Result = core::Result<std::shared_ptr<Manager>>;
    if (!capabilities.queries || !capabilities.services) return Result::failure(lifecycle_error("galactic AI requires real query and service capabilities"));
    if (static_cast<bool>(capabilities.immediate_effects) != (capabilities.immediate_instance_limit != 0))
        return Result::failure(lifecycle_error("immediate galactic effects require their explicit instance budget"));
    auto data = load_data(setup);
    if (!data) return Result::failure(data.error());
    auto impl = std::make_unique<Impl>();
    if (std::any_of(setup.service_periods.begin(), setup.service_periods.end(), [](auto frames) { return frames == 0; }))
        return Result::failure(lifecycle_error("galactic AI service periods must be positive synchronization frames"));
    impl->state.service_periods = setup.service_periods;
    impl->data = std::move(data).value();
    impl->capabilities = std::move(capabilities);
    impl->plans = std::move(setup.plans);
    for (auto& plan : impl->plans) {
        plan.name = ai::upper_case(plan.name);
        for (auto& goal : plan.goals) goal = ai::upper_case(goal);
    }
    impl->configuration_digest = lifecycle_configuration_digest(setup, impl->data, impl->plans);
    std::sort(setup.players.begin(), setup.players.end(), [](const auto& a, const auto& b) { return a.player < b.player; });
    for (auto& selected : setup.players) {
        if (!selected.player || (!impl->state.players.empty() && impl->state.players.back().selected.player == selected.player)) return Result::failure(lifecycle_error("galactic AI player identities must be nonzero and unique"));
        selected.player_type = ai::upper_case(selected.player_type);
        selected.difficulty = ai::upper_case(selected.difficulty);
        const auto type = impl->data.players.find(selected.player_type);
        if (type == impl->data.players.end()) return Result::failure(lifecycle_error("selected galactic AI type is missing: " + selected.player_type));
        const auto adjustment = type->second.difficulty.find(selected.difficulty);
        if (adjustment == type->second.difficulty.end() || !impl->data.difficulties.contains(adjustment->second)) return Result::failure(lifecycle_error("selected galactic difficulty is missing: " + selected.difficulty));
        for (const auto& name : type->second.galactic_templates) if (!impl->data.templates.contains(name)) return Result::failure(lifecycle_error("selected galactic template is missing: " + name));
        PlayerLifecycle player;
        player.selected = std::move(selected);
        player.active_templates.resize(type->second.galactic_templates.size());
        impl->state.players.push_back(std::move(player));
        impl->player_state.try_emplace(impl->state.players.back().selected.player);
    }
    impl->reset_records();
    return Result::success(std::shared_ptr<Manager>(new Manager(std::move(impl))));
}

core::Result<void> Manager::service(Clocks clocks, Admission admission, authoritative::ScriptScheduler& scripts) {
    auto& impl = *impl_;
    if (impl.phase_abandoned || impl.servicing || impl.service_threw) return core::Result<void>::failure(lifecycle_error("lifecycle service requires its live nonreentrant owner"));
    if (impl.failure) return core::Result<void>::failure(*impl.failure);
    struct ServiceScope {
        Impl& impl; int exceptions{std::uncaught_exceptions()};
        ~ServiceScope() { if (std::uncaught_exceptions() > exceptions) impl.service_threw = true; impl.servicing = false; }
    } scope{impl};
    impl.servicing = true; impl.faulted_provider.reset();
    const auto abort = [&](core::Diagnostic error) {
        impl.failure = std::move(error);
        return core::Result<void>::failure(*impl.failure);
    };
    mark_state_changed();
    impl.state.admission = admission; impl.update_record();
    if (admission.active_mode != Mode::galactic) return core::Result<void>::success();
    if (impl.capabilities.immediate_effects) {
        if (auto bound = scripts.set_immediate_effect_host(impl.capabilities.immediate_effects,
            impl.capabilities.immediate_instance_limit); !bound) return abort(bound.error());
    }
    if (clocks.synchronization < impl.state.clocks.synchronization || clocks.mode < impl.state.clocks.mode) return abort(lifecycle_error("galactic manager clocks went backwards"));
    const auto world = impl.capabilities.queries->copy_world();
    if (!world || world->clocks != clocks) return abort(lifecycle_error("galactic manager requires a copied view at its two clocks"));
    impl.state.clocks = clocks; impl.update_record();
    for (auto& player : impl.state.players) {
        if (provider_retired(player.selected.player)) continue;
        struct RecordWrite {
            Impl& owner; gc::PlayerId player{}; int exceptions{std::uncaught_exceptions()};
            ~RecordWrite() {
                if (owner.failure || std::uncaught_exceptions() > exceptions) owner.faulted_provider = player;
                owner.publish_traces(player); owner.update_record(player);
            }
        } record_write{impl, player.selected.player};
        if (!player.initialized) {
            impl.retain_player(player);
            if (auto initialized = impl.initialize(player); !initialized) return abort(initialized.error());
        }
        for (std::size_t index = 0; index < service_count; ++index) {
            const auto service_kind = static_cast<Service>(index);
            if (!admission.processing && !(admission.auto_resolve && service_kind == Service::planning)) continue;
            if (player.deadlines[index] > clocks.synchronization) continue;
            const auto delay = impl.state.service_periods[index];
            if (clocks.synchronization > std::numeric_limits<std::uint64_t>::max() - delay) return abort(lifecycle_error("galactic service deadline overflow"));
            impl.retain_player(player);
            player.deadlines[index] = clocks.synchronization + delay;
            impl.player_state.at(player.selected.player).traces.push_back(ServiceTrace{player.selected.player, service_kind, clocks});
            if (service_kind == Service::templates) {
                if (auto changed = impl.templates(player); !changed) return abort(changed.error());
            }
            const ServiceInput input{player.selected.player, service_kind, clocks, world, impl.policy_snapshot(player)};
            if (auto served = impl.capabilities.services->run(input, scripts); !served) return abort(served.error());
        }
    }
    return core::Result<void>::success();
}

core::Result<authoritative::ServiceOptions> Manager::before_service(const sim::PartitionExecutor& executor,
    const gc::GalacticSession& world, const gc::GalacticTick& tick, authoritative::ScriptScheduler& scripts) {
    using Result = core::Result<authoritative::ServiceOptions>;
    if (impl_->phase_abandoned || impl_->service_threw) return Result::failure(lifecycle_error("unsettled lifecycle phase"));
    if (impl_->capabilities.prepare) if (auto prepared = impl_->capabilities.prepare(executor, world, tick); !prepared) return Result::failure(prepared.error());
    if (auto serviced = service({tick.completed_tick, world.clock().galactic_frame}, impl_->state.admission, scripts); !serviced) return Result::failure(serviced.error());
    if (auto finished = impl_->capabilities.services->finish_barrier(scripts); !finished) return Result::failure(finished.error());
    authoritative::ServiceOptions options;
    options.host_paced = [](std::uint64_t) { return true; }; // plan/freestore pumps belong to their admitted services.
    options.time_tick = world.clock().galactic_frame;
    return Result::success(std::move(options));
}

bool Manager::admits_service(const gc::GalacticTick& tick) const noexcept {
    const auto& admission = impl_->state.admission;
    return admission.active_mode == Mode::galactic &&
        (tick.work.serviced || (!admission.processing && admission.auto_resolve));
}
bool Manager::perception_due(std::uint64_t synchronization) const noexcept {
    if (!impl_->state.admission.processing || impl_->state.admission.active_mode != Mode::galactic) return false;
    return std::any_of(impl_->state.players.begin(), impl_->state.players.end(), [&](const auto& player) {
        return !provider_retired(player.selected.player) && player.deadlines[static_cast<std::size_t>(Service::perception)] <= synchronization;
    });
}
bool Manager::service_due(gc::PlayerId player, Service service, std::uint64_t synchronization) const noexcept {
    const auto& admission = impl_->state.admission;
    if (!admission.processing || admission.active_mode != Mode::galactic || provider_retired(player)) return false;
    const auto found = std::find_if(impl_->state.players.begin(), impl_->state.players.end(),
        [&](const auto& entry) { return entry.selected.player == player; });
    return found != impl_->state.players.end() && found->initialized
        && found->deadlines[static_cast<std::size_t>(service)] <= synchronization;
}
ServicePeriods Manager::service_periods() const noexcept { return impl_->state.service_periods; }
core::Result<void> Manager::after_service(authoritative::ServiceReport& report) {
    if (impl_->phase_abandoned || impl_->service_threw || impl_->failure)
        return core::Result<void>::failure(lifecycle_error("lifecycle tail requires its healthy owner"));
    return impl_->capabilities.services->after_service(report);
}

void Manager::set_admission(Admission admission) noexcept {
    if (impl_->state.admission != admission) mark_state_changed();
    impl_->state.admission = admission; impl_->update_record();
}
LifecycleState Manager::lifecycle() const { return impl_->state; }
std::optional<gc::PlayerId> Manager::faulted_provider() const noexcept { return impl_->faulted_provider; }
bool Manager::provider_threw() const noexcept { return impl_->service_threw; }
bool Manager::provider_retired(gc::PlayerId player) const noexcept { return impl_->state.retired_providers.contains(player); }
core::Result<void> Manager::retire_provider(gc::PlayerId player, NativeProviderFault reason) {
    if (impl_->phase || impl_->phase_abandoned || impl_->servicing || impl_->service_threw || impl_->failure
        || (reason != NativeProviderFault::result && reason != NativeProviderFault::deterministic_exception)
        || std::none_of(impl_->state.players.begin(), impl_->state.players.end(), [&](const auto& row) { return row.selected.player == player; })
        || provider_retired(player))
        return core::Result<void>::failure(lifecycle_error("native retirement requires a healthy idle known live provider"));
    impl_->state.retired_providers.emplace(player, reason); impl_->update_record(); mark_state_changed();
    return core::Result<void>::success();
}
std::vector<core::LoadNote> Manager::retirement_notes() const {
    std::vector<core::LoadNote> notes;
    for (const auto& [player, reason] : impl_->state.retired_providers) {
        core::LoadNote note; note.code = "EAWR-GCAI-PROVIDER-0001"; note.phase = core::LoadPhase::runtime;
        note.classification = core::LoadCategory::unknown_degrade; note.action = core::LoadAction::runtime_degraded;
        note.object = "native galactic player " + std::to_string(player); note.tag = "native-provider-retirement";
        note.authored_value = reason == NativeProviderFault::result ? "result" : "deterministic-exception";
        note.effective_value = "retired"; note.diagnostic_code = note.code;
        note.message = "Native galactic provider retired after an unpublished deterministic fault; sibling providers continue.";
        notes.push_back(std::move(note));
    }
    return notes;
}
const std::vector<ServiceTrace>& Manager::trace() const noexcept { return impl_->traces; }
std::uint64_t Manager::trace_dropped() const noexcept { return impl_->traces_dropped; }
PolicySnapshotStats Manager::policy_snapshot_stats() const noexcept {
    PolicySnapshotStats result;
    for (const auto& [player, slice] : impl_->player_state) {
        static_cast<void>(player); result.builds += slice.policy_stats.builds; result.reuses += slice.policy_stats.reuses;
    }
    return result;
}
void Manager::Impl::publish_traces(gc::PlayerId player) {
    auto& pending = player_state.at(player).traces;
    for (auto& row : pending) {
        if (phase) phase->traces.push_back(std::move(row));
        else append_ai_diagnostic(traces, traces_dropped, std::move(row));
    }
    pending.clear();
}

core::Result<void> Manager::restore_lifecycle(const LifecycleState& saved) {
    if (impl_->phase || impl_->phase_abandoned || impl_->servicing)
        return core::Result<void>::failure(lifecycle_error("cannot restore an active or unsettled lifecycle phase"));
    if (saved.version != 2 || std::any_of(saved.service_periods.begin(), saved.service_periods.end(), [](auto frames) { return frames == 0; })
        || saved.players.size() != impl_->state.players.size() ||
        static_cast<unsigned>(saved.admission.active_mode) > static_cast<unsigned>(Mode::land)) return core::Result<void>::failure(lifecycle_error("galactic lifecycle schema/player count/mode differs"));
    if (saved.retired_providers.size() > saved.players.size()) return core::Result<void>::failure(lifecycle_error("too many retired native providers"));
    for (const auto& [player, reason] : saved.retired_providers)
        if ((reason != NativeProviderFault::result && reason != NativeProviderFault::deterministic_exception)
            || std::none_of(saved.players.begin(), saved.players.end(), [&](const auto& row) { return row.selected.player == player; }))
            return core::Result<void>::failure(lifecycle_error("invalid retired native provider"));
    for (std::size_t index = 0; index < saved.players.size(); ++index) {
        const auto& player = saved.players[index];
        const auto& current = impl_->state.players[index];
        if (!player.initialized && (player.policy != TemplatePolicy{} || player.template_cursor != 0 ||
            !player.activation_order.empty() || std::any_of(player.active_templates.begin(), player.active_templates.end(), [](bool active) { return active; }) ||
            std::any_of(player.deadlines.begin(), player.deadlines.end(), [](std::uint64_t deadline) { return deadline != 0; }))) return core::Result<void>::failure(lifecycle_error("uninitialized galactic lifecycle contains serviced state"));
        if (player.selected != current.selected || player.active_templates.size() != current.active_templates.size() ||
            (!player.active_templates.empty() && player.template_cursor >= player.active_templates.size()) ||
            (player.active_templates.empty() && player.template_cursor != 0) ||
            (player.initialized && !player.active_templates.empty() && !player.active_templates.front())) return core::Result<void>::failure(lifecycle_error("galactic lifecycle identity/template references differ"));
        std::vector<bool> seen(player.active_templates.size());
        for (const auto active : player.activation_order) {
            if (active >= seen.size() || seen[active] || !player.active_templates[active]) return core::Result<void>::failure(lifecycle_error("galactic active template references differ"));
            seen[active] = true;
        }
        if (seen != player.active_templates || (!player.activation_order.empty() && player.activation_order.front() != 0)) return core::Result<void>::failure(lifecycle_error("galactic active template list is incomplete"));
        for (const auto& [category, equation] : player.policy.equations) if (!impl_->data.goal_categories.contains(category) ||
            (!equation.value.empty() && !impl_->data.equations.find(equation.value))) return core::Result<void>::failure(lifecycle_error("galactic restored budget reference is missing"));
        for (const auto& [category, allowance] : player.policy.allowances) {
            static_cast<void>(allowance);
            if (!impl_->data.goal_categories.contains(category)) return core::Result<void>::failure(lifecycle_error("galactic restored allowance category is missing"));
        }
        for (const auto& [name, setting] : player.policy.goal_types) {
            static_cast<void>(setting);
            const auto goal = impl_->data.goals.find(name);
            if (goal == impl_->data.goals.end() || goal->second.mode != "GALACTIC") return core::Result<void>::failure(lifecycle_error("galactic restored goal reference is missing"));
        }
        for (const auto& [name, setting] : player.policy.plan_names) {
            static_cast<void>(setting);
            if (std::none_of(impl_->plans.begin(), impl_->plans.end(), [&](const auto& plan) { return plan.name == name; })) return core::Result<void>::failure(lifecycle_error("galactic restored plan reference is missing"));
        }
    }
    mark_state_changed();
    impl_->state = saved; // deadlines and policy survive; never fresh initialization.
    impl_->policy_restored();
    impl_->reset_records();
    impl_->failure.reset();
    impl_->service_threw = false; impl_->faulted_provider.reset();
    impl_->traces.clear();
    return core::Result<void>::success();
}

std::string_view Manager::freestore(gc::PlayerId player) const noexcept {
    for (const auto& entry : impl_->state.players) if (entry.selected.player == player) {
        const auto& scripts = impl_->data.players.at(entry.selected.player_type).freestore;
        const auto found = scripts.find("GALACTIC");
        if (found != scripts.end()) return found->second;
    }
    return {};
}
std::vector<std::pair<std::string, std::string>> Manager::goal_functions(gc::PlayerId player) const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& entry : impl_->state.players) if (entry.selected.player == player) {
        for (const auto& set : impl_->data.players.at(entry.selected.player_type).function_sets) {
            const auto found = impl_->data.function_sets.find(set);
            if (found == impl_->data.function_sets.end()) continue; // a different mode's closure.
            for (const auto& function : found->second) {
                const auto goal = impl_->data.goals.find(function.goal);
                if (goal != impl_->data.goals.end() && goal->second.mode == "GALACTIC") out.emplace_back(function.goal, function.function);
            }
        }
    }
    return out;
}
bool Manager::goal_enabled(gc::PlayerId player, std::string_view category, std::string_view type) const {
    static_cast<void>(category);
    for (const auto& entry : impl_->state.players) if (entry.selected.player == player) {
        const auto found = entry.policy.goal_types.find(ai::upper_case(type));
        return found != entry.policy.goal_types.end() && found->second.value;
    }
    return false;
}
bool Manager::plan_enabled(gc::PlayerId player, std::string_view category, std::string_view type, std::string_view name) const {
    static_cast<void>(category); static_cast<void>(type);
    for (const auto& entry : impl_->state.players) if (entry.selected.player == player) {
        const auto named = entry.policy.plan_names.find(ai::upper_case(name));
        if (named != entry.policy.plan_names.end()) return named->second.value;
        return false;
    }
    return false;
}
} // namespace eawr::script::foc::galactic_ai
