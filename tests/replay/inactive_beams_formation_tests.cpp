#include "../support/check.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;
using math::Vec3;

Fixed number(const std::string_view text) { return Fixed::from_decimal(text).value(); }
Fixed whole(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }
Vec3 point(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {whole(x), whole(y), whole(z)};
}

// FM-08/FM-10: delayed members must retain their future planning work while
// unrelated, inactive special abilities are serviced. These are synthetic ships.
void mixed_group(eawr::testing::Checker& check, const tactical::AbilityKind kind,
    const tactical::SpecialAbilityKind special_kind) {
    constexpr tactical::TypeId capital = 1, frigate = 2;
    tactical::TacticalSetup setup;
    setup.seed = 4466;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
    setup.units = {
        {1, capital, 1, point(-2500, -400, -180), math::identity_quat(), {}},
        {2, frigate, 1, point(-2400, -150, -110), math::identity_quat(), {}},
        {3, frigate, 1, point(-2900, 150, -110), math::identity_quat(), {}},
        {4, frigate, 1, point(-3300, -100, -110), math::identity_quat(), {}},
    };
    tactical::MotionTable movement;
    movement.rules = {whole(15), whole(300)};
    movement.profiles = {
        {capital, number("1.68"), number("0.024"), number("0.024"), number("0.36"), whole(3)},
        {frigate, number("2.64"), number("0.048"), number("0.048"), number("0.6"), whole(3)},
    };
    movement.avoidance = tactical::AvoidanceRules{whole(24), number("0.2"), whole(100), number("0.8"), whole(15),
        number("0.66"), number("1.2"), number("0.25"), number("1.7"), number("0.5"), number("0.5"),
        3500, 6, 90, 45, whole(50)};
    movement.footprints = {
        {capital, tactical::SpaceLayer::capital, whole(200), whole(380), whole(380), false},
        {frigate, tactical::SpaceLayer::frigate, whole(100), whole(200), whole(200), false},
    };
    tactical::DurabilityTable health;
    for (const auto type : {capital, frigate}) {
        tactical::DurabilityProfile profile;
        profile.type_id = type;
        profile.max_hull = whole(10000);
        health.profiles.push_back(profile);
    }
    tactical::AbilityProfile ordinary;
    ordinary.kind = kind;
    ordinary.gui_activated_ability_name = "idle-beam";
    tactical::SpecialAbilityProfile special;
    special.name = "idle-beam";
    special.kind = special_kind;
    special.style = tactical::SpecialActivationStyle::user_input;
    special.service_interval = 1;
    if (kind == tactical::AbilityKind::leech_shields) {
        ordinary.recharge_at_activation = true;
        ordinary.expiration_frames = 90;
        special.leech_duration_frames = ordinary.expiration_frames;
        special.leech_damage_bonus = whole(1);
    }
    tactical::UnitAbilityProfile holder;
    holder.type_id = frigate;
    holder.abilities = {ordinary};
    holder.special = {special};
    tactical::AbilityTable abilities;
    abilities.profiles = {holder};
    abilities.humans = {1};

    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(setup, {}, health, movement, std::nullopt, {}, {}, abilities);
        check.check(static_cast<bool>(created), "mixed group with inactive beam creates", std::cerr);
        if (!created) { std::cerr << created.error().message << '\n'; continue; }
        auto world = std::move(created).value();
        check.check(static_cast<bool>(world.submit({{0, 1, 0}, {1, 2, 3, 4}, tactical::MovePayload{point(2500, 0)}})),
            "group move submits", std::cerr);
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::array<bool, 4> planned{}, moved{};
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 90; ++tick) {
            const auto stepped = world.step(executor);
            check.check(static_cast<bool>(stepped), "group movement tick succeeds", std::cerr);
            if (!stepped) { std::cerr << stepped.error().message << '\n'; break; }
            hashes.push_back(stepped.value().state_sha256);
            unsigned frigate_paths = 0;
            for (std::size_t index = 0; index < setup.units.size(); ++index) {
                const auto id = setup.units[index].entity_id;
                const auto motion = world.motion_state(id);
                const bool has_path = motion && motion->kind == tactical::MotionKind::path && !motion->nodes.empty();
                planned[index] = planned[index] || has_path;
                if (index != 0 && has_path) ++frigate_paths;
                const auto unit = world.unit_state(id);
                moved[index] = moved[index] || (unit && unit->position != setup.units[index].position);
                if (index != 0) {
                    const auto state = world.ability_state(id);
                    check.check(state && !state->slots.front().active, "beam stays inactive during movement", std::cerr);
                }
            }
            if (tick == 0) check.check(frigate_paths == 1, "first frame plans only the front frigate", std::cerr);
            if (tick == 6) for (const bool found : planned)
                check.check(found, "every staggered group member reaches planning by frame seven", std::cerr);
        }
        for (std::size_t index = 0; index < moved.size(); ++index) {
            check.check(moved[index], "every group member physically moves", std::cerr);
            if (!planned[index] || !moved[index])
                std::cerr << "beam=" << static_cast<unsigned>(kind) << " workers=" << workers
                    << " unit=" << setup.units[index].entity_id << " planned=" << planned[index]
                    << " moved=" << moved[index] << '\n';
        }
        if (workers == 1) reference = hashes;
        else check.check(hashes == reference, "group movement agrees on 1/2/4/8 workers", std::cerr);
    }
}
} // namespace

int main() {
    eawr::testing::Checker check;
    mixed_group(check, tactical::AbilityKind::tractor_beam, tactical::SpecialAbilityKind::tractor_beam);
    mixed_group(check, tactical::AbilityKind::energy_weapon, tactical::SpecialAbilityKind::energy_weapon);
    mixed_group(check, tactical::AbilityKind::leech_shields, tactical::SpecialAbilityKind::leech_shields);
    mixed_group(check, tactical::AbilityKind::super_laser, tactical::SpecialAbilityKind::super_laser);
    return check.finish();
}
