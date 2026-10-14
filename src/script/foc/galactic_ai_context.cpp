#include "galactic_ai_world.hpp"
#include "eawr/campaign/game_session.hpp"

namespace eawr::script::foc::ai {
Context GalacticWorld::goal_context(const PlayerAi& who, const Target* target) const {
    Context out; out.player = who.player; out.target = target;
    for (const auto& other : setup_.players) {
        if (!out.enemy && !other.neutral && !allied(who.player, other.player)) out.enemy = other.player;
        if (!out.human && other.human && !other.neutral) out.human = other.player;
    }
    return out;
}
std::optional<Real> GalacticWorld::evaluate(const Engine& engine, std::string_view name,
    tactical::PlayerId owner, const Target* target) const {
    if (!player(owner)) return {};
    PlayerAi selected; selected.player = owner;
    const auto context = goal_context(selected, target);
    return evaluate(engine, name, context);
}
std::optional<Real> GalacticWorld::evaluate(const Engine& engine, std::string_view name, const Context& context) const {
    if (!player(context.player)) return {};
    const auto* equation = engine.data().equations.find(name); if (!equation) return {};
    AiRandom random;
    random.set_seed(setup_.seed + (context.player + 1U) * 0x83U + 0x12345678U + crc32(equation->name)
        + crc32(context.target ? context.target->name : "")); // PE-12
    return perception(engine, *equation, context, random);
}
std::optional<Real> GalacticWorld::evaluate_read_only(const Engine& engine, std::string_view name,
    tactical::PlayerId owner, const Target* target) const {
    if (!player(owner)) return {};
    PlayerAi selected; selected.player = owner;
    return evaluate_read_only(engine, name, goal_context(selected, target));
}
std::optional<Real> GalacticWorld::evaluate_read_only(const Engine& engine, std::string_view name, const Context& context) const {
    if (!player(context.player)) return {};
    const auto* equation = engine.data().equations.find(name); if (!equation) return {};
    AiRandom random;
    random.set_seed(setup_.seed + (context.player + 1U) * 0x83U + 0x12345678U + crc32(equation->name)
        + crc32(context.target ? context.target->name : "")); // PE-12
    return perception_read_only(engine, *equation, context, random);
}
std::optional<Real> evaluate_in_galactic_context(const GalacticParentContext& parent,
    std::string_view name, tactical::PlayerId owner) {
    if (!parent.campaign_child || !parent.manager || !parent.world || !parent.conflict_planet
        || !parent.manager->target_list(owner)) return {};
    const auto* target = parent.manager->target_of_object(parent.conflict_planet);
    if (!target) return {};
    if (parent.timeline_value) return parent.world->evaluate_with_timeline(*parent.manager, name, owner, target, *parent.timeline_value);
    if (parent.timeline) {
        if (parent.timeline->mode.mode != campaign::SessionMode::battle || !parent.timeline->mode.conflict) return {};
        return parent.world->evaluate_with_timeline(*parent.manager, name, owner, target, parent.timeline->value);
    }
    return parent.world->evaluate_read_only(*parent.manager, name, owner, target); // WGA-65/80/81 no parent publication or service
}
core::Result<Real> GalacticQueries::equation(std::string_view name, const galactic_ai::QueryContext& query) const {
    if (query.manager != galactic_ai::Mode::galactic || !world_.available())
        return core::Result<Real>::failure(detail::make_error(authoritative::codes::invalid_request, "galactic equation requires its retained manager"));
    const auto* equation = manager_.data().equations.find(name);
    const auto* target = query.target ? manager_.target_of_object(*query.target) : nullptr;
    if (!equation || (query.target && !target))
        return core::Result<Real>::failure(detail::make_error(authoritative::codes::invalid_request, "galactic equation or target is absent"));
    PlayerAi selected; selected.player = query.self;
    auto context = world_.goal_context(selected, target);
    if (query.enemy) context.enemy = query.enemy;
    if (query.human) context.human = query.human;
    const auto value = world_.evaluate(manager_, name, context);
    if (value) return core::Result<Real>::success(*value);
    return core::Result<Real>::failure(detail::make_error(authoritative::codes::missing_api, "galactic equation needs an unavailable token or context capability: " + std::string(name)));
}
} // namespace eawr::script::foc::ai
