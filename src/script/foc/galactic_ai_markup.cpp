#include "galactic_ai_markup.hpp"
#include "galactic_ai_world.hpp"

namespace eawr::script::foc::ai {
namespace {
template<class T> core::Result<T> invalid(std::string message) {
    return core::Result<T>::failure(detail::make_error(authoritative::codes::invalid_request, std::move(message)));
}
bool finite(Real value) { return numeric::binary64::is_finite(value.repr); }
thread_local const GalacticMarkup* query_owner{};
thread_local const GalacticMarkupState* query_state{};
}
GalacticMarkup::GalacticMarkup(std::map<tactical::PlayerId, GalacticMarkupHints> hints,
    std::set<tactical::PlayerId> players, std::set<tactical::PlayerId> ai_players,
    std::set<sim::galactic::PlanetId> planets)
    : players_(std::move(players)), ai_players_(std::move(ai_players)), planets_(std::move(planets)) {
    state_.hints = std::move(hints);
}
bool GalacticMarkup::alive(const GalacticMarkupKey& key) const {
    return players_.contains(key.observer) && (key.player_target
        ? players_.contains(static_cast<tactical::PlayerId>(key.target))
            && key.target <= std::numeric_limits<tactical::PlayerId>::max()
        : planets_.contains(key.target));
}
Real GalacticMarkup::value(const GalacticMarkupKey& key) const {
    if (query_owner == this && query_state) {
        const auto row = query_state->values.find(key);
        return row == query_state->values.end() ? Real{} : row->second;
    }
    std::scoped_lock lock(mutex_);
    const auto row = state_.values.find(key); return row == state_.values.end() ? Real{} : row->second;
}
core::Result<authoritative::ValueList> GalacticMarkup::query_with_state(const GalacticMarkupState& state,
    const std::function<core::Result<authoritative::ValueList>()>& query) const {
    struct Scope {
        const GalacticMarkup* old_owner{query_owner};
        const GalacticMarkupState* old_state{query_state};
        ~Scope() { query_owner = old_owner; query_state = old_state; }
    } scope;
    query_owner = this; query_state = &state; return query();
}
std::optional<Real> GalacticMarkup::hint(tactical::PlayerId player, std::string_view planet, std::string_view name) const {
    std::scoped_lock lock(mutex_);
    const auto row = state_.hints.find(player);
    if (row == state_.hints.end() || row->second.set.empty()) return {};
    const auto at = row->second.planets.find(upper_case(planet));
    const auto key = upper_case(name);
    if (at != row->second.planets.end()) if (const auto found = at->second.find(key); found != at->second.end()) return found->second;
    const auto found = row->second.defaults.find(key);
    return found == row->second.defaults.end() ? std::nullopt : std::optional{found->second};
}
GalacticMarkupState GalacticMarkup::state() const { std::scoped_lock lock(mutex_); return state_; }
std::shared_ptr<const GalacticMarkupState> GalacticMarkup::snapshot() const {
    std::scoped_lock lock(mutex_);
    if (!snapshot_) snapshot_ = std::make_shared<const GalacticMarkupState>(state_);
    return snapshot_;
}
std::uint64_t GalacticMarkup::revision() const noexcept { std::scoped_lock lock(mutex_); return revision_; }
core::Result<void> GalacticMarkup::validate(const GalacticMarkupState& input) const {
    std::scoped_lock lock(mutex_);
    if (!input.next_handle) return invalid<void>("invalid markup handle frontier");
    const auto hints_valid = [](const auto& hints) {
        for (const auto& [name, value] : hints) if (name.empty() || name != upper_case(name) || !finite(value)) return false;
        return true;
    };
    for (const auto& [player, hints] : input.hints) {
        if (!players_.contains(player) || hints.set.empty() || hints.set != upper_case(hints.set)
            || !hints_valid(hints.defaults)) return invalid<void>("invalid authored markup hints");
        for (const auto& [planet, values] : hints.planets)
            if (planet.empty() || planet != upper_case(planet) || !hints_valid(values)) return invalid<void>("invalid planet hint table");
    }
    for (const auto& [key, value] : input.values) if (!alive(key) || !finite(value)) return invalid<void>("invalid retained markup value");
    for (const auto& [id, handle] : input.handles) {
        if (!id || id >= input.next_handle || !finite(handle.value) || to_single(handle.value) != handle.value)
            return invalid<void>("invalid retained markup handle");
        for (const auto& key : handle.targets) if (!alive(key) || !input.values.contains(key)) return invalid<void>("invalid marked record reference");
    }
    return core::Result<void>::success();
}
void GalacticMarkup::replace(GalacticMarkupState state) {
    std::scoped_lock lock(mutex_);
    // A restore/write preserves the numeric payload bits, including signed zero.
    state_ = std::move(state); snapshot_.reset(); ++revision_;
}
void GalacticMarkup::subtract_contribution(GalacticMarkupState& state, const GalacticMarkupHandle& handle) const {
    for (const auto& key : handle.targets) if (alive(key)) state.values[key] = state.values[key] - handle.value;
}
bool GalacticMarkup::destroy(GalacticMarkupState& state, std::uint64_t id) const {
    std::scoped_lock lock(mutex_);
    if (!state.handles.contains(id)) return false;
    auto staged = state;
    const auto found = staged.handles.find(id);
    subtract_contribution(staged, found->second); // WGA-469: repetitions retain subtraction order.
    staged.handles.erase(found);
    state = std::move(staged); return true;
}
core::Result<std::uint64_t> GalacticMarkup::apply(GalacticMarkupState& state, tactical::PlayerId player,
    std::span<const GalacticMarkupKey> targets, Real value, std::optional<std::uint64_t> previous) const {
    std::scoped_lock lock(mutex_);
    if (!ai_players_.contains(player) || !finite(value) || !finite(to_single(value)))
        return invalid<std::uint64_t>("Apply_Markup requires an AI player and finite number");
    for (const auto& target : targets) if (target.observer != player || !alive(target))
        return invalid<std::uint64_t>("Apply_Markup requires live perception records");
    if (previous && !state.handles.contains(*previous)) return invalid<std::uint64_t>("Apply_Markup handle is unavailable");
    if (!previous && state.next_handle == std::numeric_limits<std::uint64_t>::max()) return invalid<std::uint64_t>("markup handle frontier exhausted");
    auto staged = state;
    const auto id = previous.value_or(staged.next_handle);
    if (!previous) ++staged.next_handle;
    auto& handle = staged.handles[id];
    subtract_contribution(staged, handle);
    handle = {to_single(value), {targets.begin(), targets.end()}};
    for (const auto& key : targets) {
        staged.values[key] = staged.values[key] + handle.value;
        if (!finite(staged.values[key])) return invalid<std::uint64_t>("markup accumulation is nonfinite");
    }
    state = std::move(staged); return core::Result<std::uint64_t>::success(id);
}
void GalacticMarkup::retain_planets(std::span<const sim::galactic::Planet> rows) {
    std::set<sim::galactic::PlanetId> live;
    for (const auto& row : rows) live.insert(row.id);
    std::scoped_lock lock(mutex_);
    if (live == planets_) return;
    planets_ = std::move(live);
    std::erase_if(state_.values, [&](const auto& row) { return !alive(row.first); });
    for (auto& [id, handle] : state_.handles) {
        static_cast<void>(id); std::erase_if(handle.targets, [&](const auto& key) { return !alive(key); });
    }
    ++revision_;
    snapshot_.reset();
}

namespace {
using auth = authoritative::ImmediateEffectState;
struct MarkupEffects final : auth {
    std::unique_ptr<auth> native;
    std::shared_ptr<GalacticMarkup> owner;
    std::shared_ptr<const GalacticMarkupState> original, state;
    std::shared_ptr<const GalacticWorld> world;
    std::shared_ptr<const Engine> engine;
    MarkupEffects(std::unique_ptr<auth> input, std::shared_ptr<GalacticMarkup> store)
        : native(std::move(input)), owner(std::move(store)), original(owner->snapshot()), state(original) {}
    std::unique_ptr<auth> clone() const override {
        auto out = std::make_unique<MarkupEffects>(native ? native->clone() : nullptr, owner);
        out->original = original; out->state = state; out->world = world; out->engine = engine; return out;
    }
    core::Result<authoritative::ValueList> apply(std::string_view action, const authoritative::ValueList& args) override {
        if (action == "galactic.markup.release") {
            const auto* handle = args.size() == 1 ? detail::as_handle(args[0], handle_galactic_markup) : nullptr;
            if (!handle) return invalid<authoritative::ValueList>("invalid markup release handle");
            auto changed = *state;
            if (owner->destroy(changed, handle->id)) state = std::make_shared<const GalacticMarkupState>(std::move(changed));
            return detail::none();
        }
        if (action != "galactic.markup") return native ? native->apply(action, args) : invalid<authoritative::ValueList>("native action unavailable");
        if (args.size() != 4) return invalid<authoritative::ValueList>("invalid markup effect arguments");
        const auto* player = detail::as_handle(args[0], handle_player);
        const auto* targets = std::get_if<std::vector<authoritative::Value>>(&args[1].data);
        const auto* value = std::get_if<Real>(&args[2].data);
        if (!player || player->id > std::numeric_limits<tactical::PlayerId>::max() || !targets || !value) return invalid<authoritative::ValueList>("invalid markup effect types");
        std::vector<GalacticMarkupKey> keys;
        for (const auto& target : *targets) {
            const auto* key = std::get_if<authoritative::Handle>(&target.data);
            if (!key || (key->kind != handle_game_object && key->kind != handle_player)) return invalid<authoritative::ValueList>("invalid markup target kind");
            keys.push_back({static_cast<tactical::PlayerId>(player->id), key->kind == handle_player, key->id});
        }
        const auto* old = detail::as_handle(args[3], handle_galactic_markup);
        auto changed = *state;
        auto id = owner->apply(changed, static_cast<tactical::PlayerId>(player->id), keys, *value, old ? std::optional{old->id} : std::nullopt);
        if (id) state = std::make_shared<const GalacticMarkupState>(std::move(changed));
        return id ? detail::one(detail::handle(handle_galactic_markup, id.value())) : core::Result<authoritative::ValueList>::failure(id.error());
    }
    core::Result<authoritative::ValueList> query(std::string_view name, const authoritative::ValueList& args) const override {
        if (name == "galactic.markup.inventory") {
            authoritative::ValueList handles;
            for (const auto& [id, handle] : state->handles) {
                static_cast<void>(handle); handles.push_back(detail::handle(handle_galactic_markup, id));
            }
            return core::Result<authoritative::ValueList>::success(std::move(handles));
        }
        if (name == "galactic.markup_perception" && world && engine) {
            if (state == original) return detail::none();
            return owner->query_with_state(*state, [&]() -> core::Result<authoritative::ValueList> {
                const auto* equation = args.empty() ? nullptr : detail::as_text(args[0]);
                const auto* player = args.size() < 2 ? nullptr : detail::as_handle(args[1], handle_player);
                if (!equation || !player || player->id > std::numeric_limits<tactical::PlayerId>::max())
                    return invalid<authoritative::ValueList>("invalid galactic perception arguments");
                const Target* target{};
                if (args.size() > 2) {
                    if (const auto* object = detail::as_handle(args[2], handle_game_object)) target = engine->target_of_object(object->id);
                    else if (const auto* at = detail::as_handle(args[2], handle_ai_target)) target = engine->target(at->id);
                }
                const auto value = engine->evaluate(*equation, static_cast<tactical::PlayerId>(player->id), target);
                return value ? detail::one(detail::number(*value)) : detail::one(authoritative::Value{});
            });
        }
        return native ? native->query(name, args) : invalid<authoritative::ValueList>("native query unavailable");
    }
};
struct MarkupHost final : authoritative::ImmediateEffectHost {
    std::shared_ptr<authoritative::ImmediateEffectHost> native;
    std::shared_ptr<GalacticMarkup> owner;
    std::shared_ptr<const GalacticWorld> world;
    std::shared_ptr<const Engine> engine;
    MarkupHost(std::shared_ptr<authoritative::ImmediateEffectHost> input, std::shared_ptr<GalacticMarkup> store)
        : native(std::move(input)), owner(std::move(store)) {}
    core::Result<std::unique_ptr<auth>> capture() const override {
        auto input = native ? native->capture() : core::Result<std::unique_ptr<auth>>::success(nullptr);
        if (!input) return core::Result<std::unique_ptr<auth>>::failure(input.error());
        auto out = std::make_unique<MarkupEffects>(std::move(input).value(), owner);
        out->world = world; out->engine = engine;
        return core::Result<std::unique_ptr<auth>>::success(std::move(out));
    }
    core::Result<void> publish(std::unique_ptr<auth> input) override {
        auto* shadow = dynamic_cast<MarkupEffects*>(input.get());
        if (!shadow || shadow->owner != owner || shadow->original != owner->snapshot()) return invalid<void>("markup publication owner changed");
        if (shadow->state != shadow->original) if (auto valid = owner->validate(*shadow->state); !valid) return valid;
        std::optional<GalacticMarkupState> marked;
        if (shadow->state != shadow->original) marked = *shadow->state;
        if (native) if (auto published = native->publish(std::move(shadow->native)); !published) return published;
        if (marked) owner->replace(std::move(*marked));
        return core::Result<void>::success();
    }
};
}
std::shared_ptr<authoritative::ImmediateEffectHost> markup_immediate_host(
    std::shared_ptr<authoritative::ImmediateEffectHost> native, std::shared_ptr<GalacticMarkup> owner,
    std::shared_ptr<const GalacticWorld> world, std::shared_ptr<const Engine> engine) {
    auto out = std::make_shared<MarkupHost>(std::move(native), std::move(owner));
    out->world = std::move(world); out->engine = std::move(engine); return out;
}
} // namespace eawr::script::foc::ai
