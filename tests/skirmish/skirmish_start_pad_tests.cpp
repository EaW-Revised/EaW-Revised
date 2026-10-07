#include "skirmish_start_support.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"

namespace skirmish_start_test_support {

void foc_pad_capture_build(const skirmish::SkirmishStart& start, const skirmish::StartInputs& inputs,
                           const eawr::units::UnitTables& tables) {
    auto rules = skirmish::economy_rules(start, inputs, tables);
    auto health = eawr::units::durability_table(tables);
    expect(rules && health, "WBP-01/11: real pad economy and durability bind");
    if (!rules || !health) return;
    // WBP-33/34: all merchant factions retain the ordinary queue and authored prices/times/population.
    struct MerchantOption { const char* faction; const char* name; int price; int seconds; unsigned population; };
    const MerchantOption merchant_options[] = {
        {"Rebel", "Rebel_Pirate_IPV", 1200, 10, 2},
        {"Rebel", "Rebel_Pirate_Frigate", 2200, 14, 3},
        {"Rebel", "Rebel_Pirate_Fighter_Squadron", 350, 8, 1},
        {"Rebel", "Z95_Headhunter_Rebel_Squadron", 200, 8, 1},
        {"Empire", "Empire_Pirate_IPV", 1200, 10, 2},
        {"Empire", "Empire_Pirate_Frigate", 2200, 14, 3},
        {"Empire", "Empire_Pirate_Fighter_Squadron", 350, 8, 1},
        {"Empire", "Z95_Headhunter_Empire_Squadron", 200, 8, 1},
        {"Underworld", "Underworld_Pirate_IPV", 1200, 8, 2},
        {"Underworld", "Jedi_Cruiser_U", 2100, 20, 1},
        {"Underworld", "Underworld_Pirate_Fighter_Squadron", 350, 6, 1},
        {"Underworld", "Z95_Headhunter_Underworld_Squadron", 250, 5, 1},
    };
    const auto merchant = skirmish::type_id("Skirmish_Merchant_Dock");
    const auto* capture = rules.value().pads.point(merchant);
    expect(capture && !capture->build_pad && capture->ownership_sticks && capture->community_property,
        "WBP-33 merchant is a sticky community producer, not a construction pad");
    expect(rules.value().stream(merchant) == nullptr, "WBP-33 merchant has no invented income stream");
    for (const auto& row : merchant_options) {
        const auto* menu = rules.value().menu(merchant, skirmish::faction_id(row.faction));
        const auto* option = menu ? menu->find(skirmish::type_id(row.name)) : nullptr;
        expect(menu && menu->station_producer && menu->options.size() == 4,
            "WBP-34 each merchant faction has four ordinary production options");
        expect(option && option->kind == tactical::BuildKind::unit && option->queue == tactical::BuildQueue::units
            && option->price == whole(row.price) && option->build_frames == static_cast<unsigned>(row.seconds) * 30
            && option->population == row.population && option->requirements.prerequisites.empty(),
            "WBP-34 stock merchant costs, durations and population come from the unit tables");
        expect(option && option->available == skirmish::roster_disabled_reason(row.name).empty(),
            "WPR-33 merchant has no added tech, power or base-level purchase gate");
    }
    struct MineUpgrade {
        const char* mine;
        const char* faction;
        const char* low;
        const char* high;
        int low_price;
        int high_price;
        int low_seconds;
        int high_seconds;
        const char* low_percentage;
        const char* high_percentage;
    };
    const MineUpgrade mine_upgrades[] = {
        {"UC_Empire_Mineral_Extractor", "Empire", "ES_Increased_Supplies_L1_Upgrade", "ES_Increased_Supplies_L2_Upgrade", 250, 500, 40, 65, "0.2", "0.4"},
        {"UC_Rebel_Mineral_Extractor", "Rebel", "RS_Increased_Supplies_L1_Upgrade", "RS_Increased_Supplies_L2_Upgrade", 250, 500, 40, 65, "0.2", "0.4"},
        {"UC_Underworld_Mineral_Extractor", "Underworld", "UL_Extort_Cash_L1_Upgrade", "UL_Extort_Cash_L2_Upgrade", 400, 700, 20, 30, "0.15", "0.45"},
    };
    for (const auto& row : mine_upgrades) {
        const auto* uc = tables.find(row.mine);
        expect(uc && !uc->constructed_type.empty(), "WBP-24 faction mine UC resolves its completed type");
        if (!uc) continue;
        const auto* menu = rules.value().menu(skirmish::type_id(uc->constructed_type), skirmish::faction_id(row.faction));
        expect(menu && menu->station_producer, "WBP-24 completed mine admits the ordinary producer queue");
        if (!menu) continue;
        for (const bool second : {false, true}) {
            const auto type = skirmish::type_id(second ? row.high : row.low);
            const auto* option = menu->find(type);
            const auto* upgrade = rules.value().upgrade(type);
            expect(option && option->available && option->kind == tactical::BuildKind::upgrade
                && option->queue == tactical::BuildQueue::upgrades
                && option->price == whole(second ? row.high_price : row.low_price)
                && option->build_frames == static_cast<std::uint32_t>(second ? row.high_seconds : row.low_seconds) * 30
                && option->requirements.current_allies == 1U,
                "WBP-24 all six stock levels retain costs, times, upgrades queue and allied current limit");
            expect(option && option->requirements.prerequisites == (second
                ? std::vector<tactical::TypeId>{skirmish::type_id(row.low)} : std::vector<tactical::TypeId>{})
                && upgrade && upgrade->removes_previous == (second ? skirmish::type_id(row.low) : 0),
                "WBP-24 stock L2 requires and replaces L1; L1 has neither prerequisite nor previous level");
            expect(upgrade && upgrade->income_modifiers.size() == 1,
                "WBP-25 each stock space level admits one source modifier, excluding its ground ability");
            if (upgrade && upgrade->income_modifiers.size() == 1) {
                const auto& modifier = upgrade->income_modifiers.front();
                expect(modifier.target_source == menu->station && modifier.stacking_category == 1
                    && modifier.all_allies && !modifier.reverse && modifier.additive == Fixed{}
                    && modifier.interval_percentage == Fixed{}
                    && modifier.percentage == Fixed::from_decimal(second ? row.high_percentage : row.low_percentage).value(),
                    "WBP-25/44 stock modifiers match authored target, category and independent percentage lists");
            }
        }
    }
    auto ground_only = tables;
    const auto ground_level = std::find_if(ground_only.units.begin(), ground_only.units.end(), [](const auto& type) {
        return type.id == "RS_Increased_Supplies_L1_Upgrade";
    });
    if (ground_level != ground_only.units.end()) {
        for (auto& modifier : ground_level->production.income_bonuses) modifier.activation_style = "Ground_Automatic";
        const auto excluded = skirmish::economy_rules(start, inputs, ground_only);
        const auto* uc = tables.find("UC_Rebel_Mineral_Extractor");
        const auto* excluded_menu = excluded && uc ? excluded.value().menu(skirmish::type_id(uc->constructed_type),
            skirmish::faction_id("Rebel")) : nullptr;
        const auto* excluded_option = excluded_menu ? excluded_menu->find(skirmish::type_id("RS_Increased_Supplies_L1_Upgrade")) : nullptr;
        const auto* excluded_upgrade = excluded ? excluded.value().upgrade(skirmish::type_id("RS_Increased_Supplies_L1_Upgrade")) : nullptr;
        expect(excluded_option && !excluded_option->available && excluded_upgrade && excluded_upgrade->income_modifiers.empty(),
            "WBP-25 explicit ground activation never admits a space upgrade or modifier");
    }
    const auto disabled = skirmish::roster_disabled_units();
    auto gated_tables = tables;
    const auto gated_child = std::find_if(gated_tables.units.begin(), gated_tables.units.end(), [](const auto& type) {
        return type.id == "UC_Rebel_Mineral_Extractor";
    });
    expect(!disabled.empty() && gated_child != gated_tables.units.end(), "WBP-36/RG-04 pad gate fixture resolves");
    if (!disabled.empty() && gated_child != gated_tables.units.end()) {
        gated_child->constructed_type = disabled.front().unit;
        const auto gated_rules = skirmish::economy_rules(start, inputs, gated_tables);
        expect(static_cast<bool>(gated_rules), "WBP-36/RG-04 gated final-type pad menu binds");
        if (gated_rules) {
            const auto* menu = gated_rules.value().menu(skirmish::type_id("Mineral_Extractor_Pad"), skirmish::faction_id("Rebel"));
            const auto* option = menu ? menu->find(skirmish::type_id("UC_Rebel_Mineral_Extractor")) : nullptr;
            expect(option && !option->available && option->disabled_reason == skirmish::roster_disabled_reason(disabled.front().unit),
                   "WBP-36/RG-04 a disabled completed type disables its UC entry through the shared policy");
        }
    }
    const auto ship = std::find_if(start.units.begin(), start.units.end(), [](const auto& unit) {
        return unit.type == "Corellian_Corvette" && unit.state.owner == 1;
    });
    const auto* child = rules.value().pads.child(skirmish::type_id("UC_Rebel_Mineral_Extractor"));
    expect(ship != start.units.end() && child != nullptr, "Polus has the fleet and authored mine choice");
    if (ship == start.units.end() || child == nullptr) return;
    const skirmish::StartUnit* pad = nullptr;
    long double nearest = 0;
    for (const auto& unit : start.units) {
        if (unit.type != "Mineral_Extractor_Pad") continue;
        const auto dx = static_cast<long double>(unit.state.position.x.raw()) - ship->state.position.x.raw();
        const auto dy = static_cast<long double>(unit.state.position.y.raw()) - ship->state.position.y.raw();
        const auto distance = dx * dx + dy * dy;
        if (pad == nullptr || distance < nearest) { pad = &unit; nearest = distance; }
    }
    expect(pad != nullptr, "Polus has a mining pad");
    if (pad == nullptr) return;
    // Keep the real map surroundings: reducing this to a ship and pad hides
    // neutral hazard props incorrectly entering the capture/build queries.
    {
        auto surrounding = start.setup;
        const auto approaching = std::find_if(surrounding.units.begin(), surrounding.units.end(),
            [&](const auto& unit) { return unit.entity_id == ship->state.entity_id; });
        approaching->position = pad->state.position;
        approaching->position.x = Fixed::from_raw(approaching->position.x.raw() + whole(350).raw());
        bool nearby_hazard = false;
        for (const auto& unit : surrounding.units) {
            const auto obstacle = std::find_if(tables.obstacles.begin(), tables.obstacles.end(),
                [&](const auto& type) { return skirmish::type_id(type.id) == unit.type_id; });
            if (obstacle != tables.obstacles.end() && obstacle->footprint.hazard.asteroid_field
                && tactical::within_range(pad->state.position, unit.position,
                    rules.value().pads.point(pad->state.type_id)->radius, tactical::RangeMetric::spatial)) {
                nearby_hazard = true;
            }
        }
        expect(nearby_hazard, "the installed mining-pad contract retains a nearby real asteroid field");
        const auto completed_frame = 321 + static_cast<std::uint64_t>(child->seconds) * 30;
        std::vector<std::string> full_map_hashes;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter pool(workers);
            auto full_map = tactical::TacticalSession::create(surrounding, skirmish::sensor_table(tables),
                health.value(), {}, {}, {}, {}, {}, rules.value());
            expect(static_cast<bool>(full_map), "installed full-map pad session binds");
            if (!full_map) return;
            expect(static_cast<bool>(full_map.value().submit({{320, 1, 0}, {pad->state.entity_id},
                tactical::PadBuildPayload{child->type}})),
                "full-map mining purchase queues through the ordinary command interface");
            Fixed previous_credits;
            Fixed station_income;
            for (std::uint64_t frame = 1; frame <= completed_frame + 1; ++frame) {
                const auto stepped = full_map.value().step(pool);
                expect(static_cast<bool>(stepped), "installed full-map mining frame succeeds");
                if (!stepped) return;
                if (workers == 1) full_map_hashes.push_back(stepped.value().state_sha256);
                else expect(stepped.value().state_sha256 == full_map_hashes[frame - 1],
                    "full-map mining hashes match on 1/2/4/8 workers");
                const auto credits = full_map.value().ledgers().front().credits;
                if (frame == 2) station_income = Fixed::from_raw(credits.raw() - previous_credits.raw());
                if (frame == 300) {
                    const auto captured_units = full_map.value().units();
                    const auto captured = std::find_if(captured_units.begin(), captured_units.end(),
                        [&](const auto& unit) { return unit.entity_id == pad->state.entity_id; });
                    expect(captured != captured_units.end() && captured->owner == 1,
                        "WHZ-41: a real neutral asteroid field does not contest the ten-second pad capture");
                    std::vector<tactical::CaptureCandidate> candidates;
                    for (const auto& unit : full_map.value().units()) candidates.push_back({
                        {unit.entity_id, unit.owner, unit.position}, unit.type_id});
                    expect(tactical::pad_construction_allowed(*rules.value().pads.point(pad->state.type_id),
                        full_map.value().pads().at(pad->state.entity_id), 1, surrounding.players, candidates,
                        rules.value().pads, pad->state.entity_id, pad->state.position),
                        "WBP-07: a real neutral asteroid field does not block opening the mine palette");
                }
                if (frame == 321) expect(full_map.value().pads().at(pad->state.entity_id).under_construction != 0,
                    "WBP-13: the captured full-map pad immediately creates the requested mine construction");
                if (frame >= completed_frame) {
                    expect(full_map.value().pads().at(pad->state.entity_id).constructed != 0,
                        "WBP-16: the captured full-map mining facility completes");
                    const auto* income = rules.value().stream(child->constructed);
                    expect(income && credits.raw() - previous_credits.raw() == station_income.raw() + income->per_frame.raw(),
                        "WBP-22: full-map mine income starts on completion and continues on the following frame");
                }
                previous_credits = credits;
            }
        }
    }
    auto attachment_tables = tables;
    const auto attachment_type = std::find_if(attachment_tables.units.begin(), attachment_tables.units.end(),
        [&](const auto& type) { return type.id == pad->type; });
    if (attachment_type != attachment_tables.units.end()) {
        attachment_type->build_attachment.position = eawr::sim::math::Vec3{whole(2), whole(3), whole(4)};
        const auto attachment_rules = skirmish::economy_rules(start, inputs, attachment_tables);
        expect(static_cast<bool>(attachment_rules), "WBP-13 attachment rules bind");
        if (attachment_rules) {
            const auto scale = attachment_type->scale_factor.value_or(whole(1));
            expect(attachment_rules.value().pads.point(pad->state.type_id)->attachment == eawr::sim::math::Vec3{
                eawr::sim::math::multiply(whole(-3), scale).value(),
                eawr::sim::math::multiply(whole(2), scale).value(),
                eawr::sim::math::multiply(whole(4), scale).value()},
                "WBP-13/R-ROT-01 attachment applies scale and model quarter-turn before facing");
        }
    }
    for (const auto seconds : {whole(0), whole(-1), Fixed::from_raw(Fixed::scale + Fixed::scale / 4)}) {
        auto timed_tables = tables;
        const auto type = std::find_if(timed_tables.units.begin(), timed_tables.units.end(),
            [&](const auto& unit) { return unit.id == pad->type; });
        if (type == timed_tables.units.end()) continue;
        type->tactical_respawn_seconds = seconds;
        const auto timed_rules = skirmish::economy_rules(start, inputs, timed_tables);
        expect(timed_rules.has_value(), "WHZ-52 authored delay variants bind");
        if (!timed_rules) continue;
        const auto* timed = timed_rules.value().pads.replacement(pad->state.type_id);
        expect(seconds.raw() <= 0 ? timed == nullptr : timed && timed->frames == 38,
            "WHZ-52 nonpositive delays schedule nothing; 1.25 seconds rounds 37.5 frames to 38");
    }
    auto setup = start.setup;
    setup.squadrons.clear();
    setup.free_garrisons.clear(); // only the isolated ship and pad remain
    setup.units = {ship->state, pad->state};
    setup.units.front().position = pad->state.position;
    setup.units.front().position.x = Fixed::from_raw(setup.units.front().position.x.raw() + whole(350).raw());
    std::sort(setup.units.begin(), setup.units.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
    const auto construction_id = setup.units.back().entity_id + 1;
    auto combat = eawr::units::combat_table(tables);
    expect(static_cast<bool>(combat), "Polus combat profiles resolve for the pad projectile boundary");
    const auto* pad_type = tables.find(pad->type);
    expect(pad_type && !pad_type->selectable && pad_type->mouse_sensitive && !pad_type->bar_admitted,
        "WSU-13/50: stock Polus empty mining satellite has a pad contact, no ordinary selection or health bar");
    if (combat) {
        const auto* authored = combat.value().find(pad->state.type_id);
        expect(authored && !authored->living_projectile_collision && authored->collision,
            "WHZ-51: Polus mining pad retains authored living-projectile rejection");
        if (authored && authored->collision) {
            // Isolate the permission from mesh shape: use the loaded collision bounds for both runs.
            auto profile = *authored;
            profile.meshes.clear();
            profile.mesh_bounds.reset();
            tactical::detail::CombatWorld world;
            std::vector<tactical::SnapshotPlayer> relationships{{1, 0, false}, {pad->state.owner, 2, true}};
            std::sort(relationships.begin(), relationships.end(), [](const auto& left, const auto& right) {
                return left.player_id < right.player_id;
            });
            world.relationships = relationships;
            tactical::detail::CombatUnit candidate;
            candidate.id = pad->state.entity_id;
            candidate.owner = pad->state.owner;
            candidate.team = 2;
            candidate.position = pad->state.position;
            candidate.transform = eawr::sim::math::to_matrix(pad->state.rotation, pad->state.position).value();
            candidate.profile = &profile;
            world.units.push_back(candidate);
            world.index = tactical::SpaceIndex::build(std::array{tactical::SpaceBody{
                candidate.id, candidate.owner, candidate.position}}).value();
            tactical::detail::CollectionTrees collidables;
            collidables.update({{candidate.id, candidate.owner,
                {candidate.position, candidate.position}}}, 0);
            world.projectile_collection = &collidables;
            tactical::Projectile projectile;
            projectile.owner = 1;
            projectile.position = candidate.position;
            projectile.position.x = Fixed::from_raw(projectile.position.x.raw() - whole(1000).raw());
            projectile.step.x = whole(2000);
            projectile.max_travel = whole(4000);
            tactical::detail::ProjectileScratch scratch;
            const auto passing = tactical::detail::step_projectile(world, projectile, scratch);
            expect(passing && !passing.value().hit && scratch.exact_count == 0,
                "WHZ-51: a projectile passes the neutral Polus mining pad without a damage contact");
            profile.living_projectile_collision = true;
            scratch = {};
            const auto still_neutral = tactical::detail::step_projectile(world, projectile, scratch);
            expect(still_neutral && !still_neutral.value().hit && scratch.exact_count == 0,
                "WHZ-51: a permissive collision flag cannot make the neutral Polus pad absorb shots");
            relationships.back().neutral = false;
            const auto permitted = tactical::detail::step_projectile(world, projectile, scratch);
            expect(permitted && permitted.value().hit == candidate.id,
                "captured pad projectile control produces contact with hostile ownership and collision permission");
        }
        auto combat_setup = setup;
        combat_setup.units.front().rotation = {Fixed{}, Fixed{}, whole(1), Fixed{}};
        auto idle = tactical::TacticalSession::create(combat_setup, skirmish::sensor_table(tables),
            health.value(), {}, {}, combat.value());
        expect(static_cast<bool>(idle), "Polus neutral-pad combat-only session binds without economy");
        if (idle) {
            expect(static_cast<bool>(idle.value().submit({{0, 1, 0}, {ship->state.entity_id},
                tactical::AttackPayload{pad->state.entity_id}})), "Polus neutral-pad explicit attack queues");
            eawr::sim::InlineExecutor pool;
            for (std::size_t frame = 0; frame < 60; ++frame) {
                const auto stepped = idle.value().step(pool);
                expect(static_cast<bool>(stepped), "Polus neutral-pad combat frame succeeds");
                if (!stepped) break;
                if (frame == 0) {
                    expect(std::any_of(stepped.value().snapshot->events().begin(), stepped.value().snapshot->events().end(),
                        [](const auto& event) { return event.reason == tactical::RejectReason::target_not_hostile; }),
                        "WHZ-51: explicit attacks reject the stock neutral Polus pad");
                }
                expect(std::none_of(stepped.value().snapshot->combat_events().begin(), stepped.value().snapshot->combat_events().end(),
                    [&](const auto& event) { return event.kind == tactical::CombatEventKind::weapon_fired
                        && event.target == pad->state.entity_id; }),
                    "WHZ-51: automatic weapons do not deliberately target the neutral mining pad");
            }
        }
        // R-08 and WHZ-51 are independent: ownership cannot override the stock
        // bare pad's collision rejection, and collision cannot override neutrality.
        for (const bool collidable : {false, true}) {
            auto collision_control = combat.value();
            const auto pad_profile = std::find_if(collision_control.profiles.begin(), collision_control.profiles.end(),
                [&](const auto& profile) { return profile.type_id == pad->state.type_id; });
            expect(pad_profile != collision_control.profiles.end(), "Polus pad collision control resolves");
            if (pad_profile == collision_control.profiles.end()) continue;
            pad_profile->living_projectile_collision = collidable;
            for (const bool neutral : {true, false}) {
                auto owned_setup = combat_setup;
                owned_setup.units.back().owner = neutral ? pad->state.owner : 2;
                auto owned_pad = tactical::TacticalSession::create(owned_setup, skirmish::sensor_table(tables),
                    health.value(), {}, {}, collision_control);
                expect(static_cast<bool>(owned_pad), "Polus pad targeting control binds");
                bool fired_at_pad = false;
                if (owned_pad) {
                    eawr::sim::InlineExecutor pool;
                    for (std::size_t frame = 0; frame < 60; ++frame) {
                        const auto stepped = owned_pad.value().step(pool);
                        expect(static_cast<bool>(stepped), "Polus pad targeting control frame succeeds");
                        if (!stepped) break;
                        const auto events = stepped.value().snapshot->combat_events();
                        fired_at_pad |= std::any_of(events.begin(), events.end(), [&](const auto& event) {
                            return event.kind == tactical::CombatEventKind::weapon_fired && event.target == pad->state.entity_id;
                        });
                    }
                }
                expect(fired_at_pad == (collidable && !neutral),
                    "R-08 / WHZ-51: only the explicitly collidable hostile pad is fired at");
            }
        }
        // WBP-18/21: a completed mine is a separate object with its own profile.
        // The stock Rebel mine inherits living collision permission from its base.
        const auto* mine_profile = combat.value().find(child->constructed);
        expect(mine_profile && mine_profile->living_projectile_collision,
            "WBP-21 / R-08: the loaded completed mine permits living projectile collision");
        if (mine_profile && mine_profile->living_projectile_collision) {
            combat_setup.units.back().owner = 2;
            auto mine = combat_setup.units.back();
            mine.entity_id = construction_id;
            mine.type_id = child->constructed;
            combat_setup.units.push_back(mine);
            auto occupied = tactical::TacticalSession::create(combat_setup, skirmish::sensor_table(tables),
                health.value(), {}, {}, combat.value());
            expect(static_cast<bool>(occupied), "Polus completed-mine combat control binds");
            bool fired_at_mine = false;
            if (occupied) {
                eawr::sim::InlineExecutor pool;
                for (std::size_t frame = 0; frame < 60; ++frame) {
                    const auto stepped = occupied.value().step(pool);
                    expect(static_cast<bool>(stepped), "Polus completed-mine combat frame succeeds");
                    if (!stepped) break;
                    const auto events = stepped.value().snapshot->combat_events();
                    fired_at_mine |= std::any_of(events.begin(), events.end(), [&](const auto& event) {
                        return event.kind == tactical::CombatEventKind::weapon_fired && event.target == mine.entity_id;
                    });
                    expect(std::none_of(events.begin(), events.end(), [&](const auto& event) {
                        return event.kind == tactical::CombatEventKind::weapon_fired && event.target == pad->state.entity_id;
                    }), "R-08: the hostile bare pad remains untargeted beside its completed mine");
                }
            }
            expect(fired_at_mine, "WBP-21 / R-08: automatic weapons engage the stock hostile completed mine");
        }
    }
    constexpr std::uint64_t build_tick = 320;
    const auto completion_tick = build_tick + static_cast<std::uint64_t>(child->seconds) * 30 + 1;
    const auto death_tick = completion_tick + 299;
    const auto* respawn = rules.value().pads.replacement(pad->state.type_id);
    expect(respawn && respawn->frames == 38 * 30, "WHZ-52 mining pad uses authored 38-second delay");
    const auto* laser_respawn = rules.value().pads.replacement(skirmish::type_id("Defense_Satellite_Laser_Pad"));
    const auto* merchant_respawn = rules.value().pads.replacement(skirmish::type_id("Skirmish_Merchant_Dock"));
    expect(laser_respawn && merchant_respawn && laser_respawn->frames == 45 * 30 && merchant_respawn->frames == 80 * 30,
        "WHZ-52 laser and merchant use authored 45/80-second delays");
    if (!respawn) return;
    const auto respawn_tick = death_tick + respawn->frames;
    const auto final_tick = respawn_tick + 1;
    const auto* stream = rules.value().stream(child->constructed);
    expect(stream && stream->split_with_allies && stream->full_amount_to_everyone, "WBP-23 stock mine full allied stream");
    if (!stream) return;
    tactical::TacticalReplay record{setup, final_tick, {
        {{build_tick, 1, 0}, {pad->state.entity_id}, tactical::PadBuildPayload{child->type}},
        {{build_tick + 10, 1, 1}, {construction_id}, tactical::DamagePayload{whole(10), tactical::attack_hull}},
        {{death_tick, 1, 2}, {construction_id + 1}, tactical::DamagePayload{whole(10000), tactical::attack_hull}}}};
    const auto bytes = tactical::write_replay(record);
    const auto parsed = bytes ? tactical::parse_replay(bytes.value())
                             : eawr::core::Result<tactical::TacticalReplay>::failure(bytes.error());
    expect(parsed && parsed.value() == record, "Polus pad replay round trip");
    if (!parsed) return;
    std::vector<std::string> hashes;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto session = tactical::TacticalSession::from_replay(parsed.value(), skirmish::sensor_table(tables),
            health.value(), {}, {}, {}, {}, {}, rules.value());
        expect(static_cast<bool>(session), "Polus pad session binds for each worker count");
        if (!session) return;
        session.value().scramble_storage_for_testing();
        std::size_t frame = 0;
        while (session.value().completed_tick() < final_tick) {
            const auto result = session.value().step(pool);
            expect(static_cast<bool>(result), "Polus pad logical frame succeeds");
            if (!result) return;
            const auto hash = session.value().state_sha256();
            if (workers == 1) hashes.push_back(hash);
            else expect(hash == hashes[frame], "Polus pad per-frame hashes match 1/2/4/8 workers");
            ++frame;
            if (frame == 300) {
                const auto owner_units = session.value().units();
                const auto owner = std::find_if(owner_units.begin(), owner_units.end(),
                    [&](const auto& unit) { return unit.entity_id == pad->state.entity_id; });
                expect(owner != owner_units.end() && owner->owner == 1,
                    "WHZ-40/46: authored ten-second mining-pad capture completes at frame 300");
            }
            if (frame == build_tick + 1) {
                expect(session.value().pads().at(pad->state.entity_id).under_construction == construction_id,
                    "WBP-13: Polus UC exists immediately");
                const auto state = session.value().durability_state(construction_id);
                expect(state && state->hull == tactical::initial_construction_hull,
                    "WBP-14: authored UC starts at 0.01 hull");
            }
            if (frame == completion_tick) {
                const auto completed = session.value().pads().at(pad->state.entity_id).constructed;
                const auto final_health = session.value().durability_state(completed);
                expect(completed != eawr::sim::invalid_entity_id && session.value().construction().empty()
                    && final_health && final_health->hull < health.value().find(child->constructed)->max_hull,
                    "WBP-16/18/19: damaged real-data mine replaces UC on its fixed deadline");
                expect(session.value().ledgers().front().credits.raw()
                    == rules.value().players.front().credits.raw() - child->price.raw() + stream->per_frame.raw(), "WBP-10/22: real mine costs the builder once and pays on completion");
            }
            if (frame > death_tick && frame <= respawn_tick) {
                expect(session.value().pads().empty(), "WBP-27/29 Polus pad stays destroyed until its deadline");
            }
        }
        const auto replacement = session.value().units().back();
        expect(replacement.type_id == pad->state.type_id && replacement.owner == rules.value().pads.neutral
            && replacement.position == pad->state.position && replacement.rotation == pad->state.rotation
            && session.value().pads().at(replacement.entity_id).constructed == 0,
            "WBP-29 Polus returns a neutral mining pad with authored placement/facing");
        expect(session.value().ledgers().front().credits.raw() == rules.value().players.front().credits.raw()
            - child->price.raw() + 300 * stream->per_frame.raw(), "WBP-22/26 exactly 300 payments, none after death");
    }
    if (const auto out = environment("EAWR_PAD_SCENARIO_OUT")) {
        std::filesystem::create_directories(*out);
        std::ofstream replay(std::filesystem::path(*out) / "polus-pad-build.eawr-replay", std::ios::binary);
        replay.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
        std::ofstream metadata(std::filesystem::path(*out) / "polus-pad-build.json");
        const auto real = [](const Fixed value) { return static_cast<double>(value.raw()) / Fixed::scale; };
        metadata << "{\"ship\":" << ship->state.entity_id << ",\"pad\":" << pad->state.entity_id
            << ",\"target\":[" << real(setup.units.front().position.x) << ',' << real(setup.units.front().position.y)
            << ',' << real(setup.units.front().position.z) << "],\"pad_position\":[" << real(pad->state.position.x)
            << ',' << real(pad->state.position.y) << ',' << real(pad->state.position.z)
            << "],\"completion_tick\":" << completion_tick << ",\"death_tick\":" << death_tick
            << ",\"respawn_tick\":" << respawn_tick << ",\"mine\":" << construction_id + 1
            << ",\"replacement_pad\":" << construction_id + 2 << ",\"final_tick\":" << final_tick << ",\"final_hash\":\"" << hashes.back() << "\"}\n";
    }
}


} // namespace skirmish_start_test_support
