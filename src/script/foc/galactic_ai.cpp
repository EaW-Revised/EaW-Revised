#include "galactic_ai_internal.hpp"

#include "galactic_ai_lifecycle_state.hpp"

#include <algorithm>
#include <limits>

namespace eawr::script::foc::galactic_ai {

Manager::Manager(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Manager::~Manager() = default;

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
    }
    return Result::success(std::shared_ptr<Manager>(new Manager(std::move(impl))));
}

core::Result<void> Manager::service(Clocks clocks, Admission admission, authoritative::ScriptScheduler& scripts) {
    auto& impl = *impl_;
    if (impl.failure) return core::Result<void>::failure(*impl.failure);
    const auto abort = [&](core::Diagnostic error) {
        impl.failure = std::move(error);
        return core::Result<void>::failure(*impl.failure);
    };
    mark_state_changed();
    impl.state.admission = admission;
    if (admission.active_mode != Mode::galactic) return core::Result<void>::success();
    if (impl.capabilities.immediate_effects) {
        if (auto bound = scripts.set_immediate_effect_host(impl.capabilities.immediate_effects,
            impl.capabilities.immediate_instance_limit); !bound) return abort(bound.error());
    }
    if (clocks.synchronization < impl.state.clocks.synchronization || clocks.mode < impl.state.clocks.mode) return abort(lifecycle_error("galactic manager clocks went backwards"));
    const auto world = impl.capabilities.queries->copy_world();
    if (!world || world->clocks != clocks) return abort(lifecycle_error("galactic manager requires a copied view at its two clocks"));
    impl.state.clocks = clocks;
    for (auto& player : impl.state.players) {
        if (!player.initialized) if (auto initialized = impl.initialize(player); !initialized) return abort(initialized.error());
        for (std::size_t index = 0; index < service_count; ++index) {
            const auto service_kind = static_cast<Service>(index);
            if (!admission.processing && !(admission.auto_resolve && service_kind == Service::planning)) continue;
            if (player.deadlines[index] > clocks.synchronization) continue;
            const auto delay = impl.state.service_periods[index];
            if (clocks.synchronization > std::numeric_limits<std::uint64_t>::max() - delay) return abort(lifecycle_error("galactic service deadline overflow"));
            player.deadlines[index] = clocks.synchronization + delay;
            impl.traces.push_back({player.selected.player, service_kind, clocks});
            if (service_kind == Service::templates) {
                if (auto changed = impl.templates(player); !changed) return abort(changed.error());
            }
            const ServiceInput input{player.selected.player, service_kind, clocks, world, std::make_shared<const TemplatePolicy>(player.policy)};
            if (auto served = impl.capabilities.services->run(input, scripts); !served) return abort(served.error());
        }
    }
    return core::Result<void>::success();
}

core::Result<authoritative::ServiceOptions> Manager::before_service(const sim::PartitionExecutor& executor,
    const gc::GalacticSession& world, const gc::GalacticTick& tick, authoritative::ScriptScheduler& scripts) {
    using Result = core::Result<authoritative::ServiceOptions>;
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
        return player.deadlines[static_cast<std::size_t>(Service::perception)] <= synchronization;
    });
}
ServicePeriods Manager::service_periods() const noexcept { return impl_->state.service_periods; }
core::Result<void> Manager::after_service(authoritative::ServiceReport& report) {
    return impl_->capabilities.services->after_service(report);
}

void Manager::set_admission(Admission admission) noexcept {
    if (impl_->state.admission != admission) mark_state_changed();
    impl_->state.admission = admission;
}
LifecycleState Manager::lifecycle() const { return impl_->state; }
const std::vector<ServiceTrace>& Manager::trace() const noexcept { return impl_->traces; }

core::Result<void> Manager::restore_lifecycle(const LifecycleState& saved) {
    if (saved.version != 2 || std::any_of(saved.service_periods.begin(), saved.service_periods.end(), [](auto frames) { return frames == 0; })
        || saved.players.size() != impl_->state.players.size() ||
        static_cast<unsigned>(saved.admission.active_mode) > static_cast<unsigned>(Mode::land)) return core::Result<void>::failure(lifecycle_error("galactic lifecycle schema/player count/mode differs"));
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
    impl_->failure.reset();
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
