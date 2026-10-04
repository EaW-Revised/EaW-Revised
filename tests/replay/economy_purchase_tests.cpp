#include "economy_support.hpp"

namespace economy_test_support {


// PU-35: the lane table and its sum.
void test_roster_gate() {
    tactical::TypeFlags flags(std::vector<tactical::TypeId>{0U, 0x01020304U, 0x010203ffU, 0xffffffffU, 0x1234567801020304ULL});
    const auto copied = flags;
    flags.clear();
    expect(copied.contains(0U) && copied.contains(0x01020304U) && copied.contains(0x010203ffU)
        && copied.contains(0xffffffffU) && copied.contains(0x1234567801020304ULL)
        && !copied.contains(0x1234567901020304ULL) && !copied.contains(0x01020404U) && !copied.contains(0xfffffffeU)
        && !flags.contains(0U), "indexed gate distinguishes all ID bytes and copies retain immutable flags");
    std::vector<tactical::TypeId> many;
    for (tactical::TypeId id = 0; id < 4096; ++id) many.push_back(id * 0x01010101010101ULL);
    const tactical::TypeFlags large(std::move(many));
    for (const auto id : {0ULL, 0x1234567801020304ULL, 0xffffffffffffffffULL}) {
        std::size_t small_work{}, large_work{};
        static_cast<void>(copied.contains(id, &small_work));
        static_cast<void>(large.contains(id, &large_work));
        expect(small_work <= 8 && large_work <= 8, "RG-04: gate query performs at most eight indexed reads regardless of roster size");
    }
    auto economy = rules();
    economy.disabled_types = {ship_type};
    // Intentionally leave the menu available: the authoritative content gate
    // rejects a stale/alternate station menu as well as a disabled UI button.
    auto world = session(economy);
    if (!world) return;
    expect(!world->build_allowed(ai, ai_station, ship_type), "RG-04: AI production discovery rejects a gated type");
    expect(static_cast<bool>(world->submit(buy(0, human, 0, human_station, ship_type))), "gated human buy submitted");
    expect(static_cast<bool>(world->submit(buy(0, ai, 0, ai_station, ship_type))), "gated AI buy submitted");
    expect(static_cast<bool>(world->submit(reinforce(0, human, 1, ship_type, at(0, 0)))), "gated reinforcement submitted");
    const auto events = step_to(*world, 1);
    expect(events.size() == 3 && std::all_of(events.begin(), events.end(), [](const auto& event) {
        return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::cannot_produce;
    }), "disabled ship cannot be human bought, AI built or reinforced");
    for (const auto& entry : world->ledgers()) {
        expect(entry.queues[0].empty() && entry.pool.empty(), "no gated production or pool entry");
    }
    const auto point = world->reinforcement_point(human, ship_type, at(0, 0));
    expect(point && !point.value(), "gated ship never offers a reinforcement preview");
    tactical::PlayerEconomy state;
    state.credits = units(6000);
    auto option = economy.menus[0].options[0];
    expect(tactical::queue_build(state, economy.players[0], economy, option, human_station, 0)
        == tactical::RejectReason::cannot_produce, "direct production also enforces the gate");
    economy.disabled_types.clear();
    option.available = false;
    expect(tactical::queue_build(state, economy.players[0], economy, option, human_station, 0)
        == tactical::RejectReason::cannot_produce, "unavailable options cannot bypass session admission");
    economy.disabled_types = {ship_type, ship_type};
    expect(!tactical::validate_economy(economy, setup().players), "duplicate gate IDs are rejected");
}


// PC-01, PU-02 to PU-04: 5 credits a second each.
void test_income() {
    auto world = session();
    if (!world) return;
    step_to(*world, 300);
    const auto* mine = ledger(*world, human);
    const auto* theirs = ledger(*world, ai);
    expect(mine != nullptr && std::abs(real(mine->credits) - 6050) < 1e-3, "PC-01: 6000 + 300 frames x 1/6 = 6050");
    expect(theirs != nullptr && mine != nullptr && theirs->credits == mine->credits, "PC-01: each player earns its own station's");
    const auto views = world->snapshot()->economy();
    expect(views.size() == 2 && mine != nullptr && views[0].credits == mine->credits && views[0].population_cap == 25,
        "the snapshot carries the ledger");
    // PU-05: the stream ends with its station.
    expect(static_cast<bool>(world->stage_remove(human_station)), "the human station is removed");
    const auto before = ledger(*world, human)->credits;
    step_to(*world, 310);
    expect(ledger(*world, human)->credits == before, "PU-05: no station, no income");
}

// PC-02, PU-15, PU-16.
void test_team_production() {
    std::vector<std::string> expected_hashes;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto start = setup();
        start.players.push_back({3, 0, rebel, tactical::player_flag_commandable});
        start.players.push_back({4, 1, empire, tactical::player_flag_commandable});
        auto economy = rules();
        economy.players.push_back({3, units(1000), 2, false, Fixed{}});
        economy.players.push_back({4, units(2000), 2, true, Fixed{}});
        economy.max_queue = 1;
        economy.income.front().split_with_allies = true;
        economy.income.front().full_amount_to_everyone = true;
        for (auto& menu : economy.menus) {
            auto& option = menu.options.front();
            option.build_frames = option.ai_build_frames = 2;
            option.requirements.prerequisites = {station_type};
            option.requirements.current_player = 1;
        }
        auto created = tactical::TacticalSession::create(start, sensors, {}, motion(), std::nullopt, {}, {}, {}, economy);
        expect(static_cast<bool>(created), "WPR-33: create four players with two shared producers");
        if (!created) return;
        auto& world = created.value();
        eawr::platform::ThreadWorkerAdapter executor(workers);
        for (const auto player : {1U, 2U, 3U, 4U}) {
            const auto station = player % 2 == 1 ? human_station : ai_station;
            expect(world.build_allowed(player, station, ship_type), "WPR-33: shared station prerequisite qualifies for each teammate");
            expect(static_cast<bool>(world.submit(buy(0, player, 0, station, ship_type))), "WPR-30: each teammate buys independently");
        }
        expect(static_cast<bool>(world.submit(buy(0, 3, 1, ai_station, ship_type))), "WPR-33: enemy station request records");
        expect(static_cast<bool>(world.submit(buy(1, 3, 2, human_station, ship_type))), "WPR-33: duplicate local purchase records");
        std::vector<std::string> hashes;
        const auto advance = [&] {
            const auto result = world.step(executor);
            expect(static_cast<bool>(result), "WPR-30: team production tick succeeds");
            if (result) hashes.push_back(result.value().state_sha256);
        };
        advance();
        for (const auto player : {1U, 2U, 3U, 4U}) {
            const auto* account = ledger(world, player);
            expect(account && account->queues[0].size() == 1, "WPR-30: queue belongs to buyer, not station owner");
        }
        const auto* owner = ledger(world, 1);
        const auto* teammate = ledger(world, 3);
        expect(owner && teammate && owner->credits.raw() - teammate->credits.raw() == units(5000).raw(),
            "WPR-11/30: both pay their own price and receive the full shared income");
        advance();
        advance();
        for (const auto player : {1U, 2U, 3U, 4U})
            expect(ledger(world, player)->pool == std::vector<tactical::TypeId>{ship_type},
                "WPR-22: same-station queues complete independently into each buyer's pool");
        expect(ledger(world, 3)->credits.raw() - units(500).raw() == ledger(world, 1)->credits.raw() - units(5500).raw(),
            "WPR-33: enemy and duplicate requests do not debit the second human");
        expect(static_cast<bool>(world.submit(reinforce(3, 3, 3, ship_type, at(-1000, 500)))),
            "WPR-32: second human deploys its own pooled purchase");
        expect(static_cast<bool>(world.submit(reinforce(3, 4, 1, ship_type, at(1000, -500)))),
            "WPR-32: opposing second AI deploys its own pooled purchase");
        advance();
        const auto deployed = world.units();
        for (const auto player : {3U, 4U}) {
            expect(ledger(world, player)->pool.empty(), "WPR-32: deployment consumes the buyer's pool");
            expect(std::any_of(deployed.begin(), deployed.end(), [&](const auto& unit) {
                return unit.owner == player && unit.type_id == ship_type;
            }), "WPR-32: deployed ship retains the second teammate's ownership/colour key");
            const auto views = world.snapshot()->economy();
            const auto view = std::find_if(views.begin(), views.end(), [player](const auto& value) { return value.player == player; });
            expect(view != views.end() && view->population == 2 && view->population_cap == 2,
                "WPR-41: population is charged to the deploying teammate's cap");
        }
        expect(ledger(world, 1)->pool.size() == 1 && ledger(world, 2)->pool.size() == 1,
            "WPR-32: teammates' deployment leaves both station owners' purchases pooled");
        if (expected_hashes.empty()) expected_hashes = hashes;
        expect(hashes == expected_hashes, "team production/reinforcement hashes agree on 1/2/4/8 workers");
        auto replay = tactical::TacticalSession::from_replay(world.record(), sensors, {}, motion(), std::nullopt, {}, {}, {}, economy);
        expect(static_cast<bool>(replay), "shared producer replay binds four accounts");
        if (!replay) continue;
        for (const auto& hash : hashes) {
            const auto result = replay.value().step(executor);
            expect(result && result.value().state_sha256 == hash, "shared production and deployment replay exactly");
        }
    }
}

void test_credit_grant() {
    auto world = session();
    auto baseline = session();
    if (!world || !baseline) return;
    expect(static_cast<bool>(world->submit({{0, ai, 0}, {}, tactical::CreditGrantPayload{units(6000)}})),
        "SAE-07: positive credit grant submits without a unit");
    const auto events = step_to(*world, 1);
    step_to(*baseline, 1);
    expect(ledger(*world, ai)->credits.raw() - ledger(*baseline, ai)->credits.raw() == units(6000).raw(),
        "SAE-07: grant adds exactly 6000 beyond ordinary income");
    expect(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.kind == tactical::EventKind::order_accepted && event.order == tactical::OrderKind::credit_grant;
    }), "SAE-07: grant publishes the ordinary command receipt");
    expect(!world->submit({{1, ai, 1}, {}, tactical::CreditGrantPayload{Fixed{}}}), "SAE-07: zero grant is malformed");
    expect(!world->submit({{1, ai, 1}, {}, tactical::CreditGrantPayload{units(-1)}}), "SAE-07: negative grant is malformed");
    expect(!world->submit({{1, ai, 1}, {ai_station}, tactical::CreditGrantPayload{units(1)}}), "SAE-07: grant lists no unit");
}

void test_buy() {
    auto world = session();
    if (!world) return;
    expect(static_cast<bool>(world->submit(buy(10, human, 0, human_station, ship_type))), "the buy is submitted");
    auto events = step_to(*world, 11);
    expect(!events.empty() && events.back().kind == tactical::EventKind::order_accepted
            && events.back().order == tactical::OrderKind::buy && events.back().unit == human_station,
        "PC-02: the buy is accepted");
    const auto after_buy = real(ledger(*world, human)->credits);
    expect(std::abs(after_buy - (6000 + 11.0 / 6 - 500)) < 1e-3, "PU-15: the price is paid when queued");
    step_to(*world, 460);
    expect(ledger(*world, human)->pool.empty(), "PU-16: not done before 450 frames");
    step_to(*world, 461);
    expect(ledger(*world, human)->pool == std::vector<tactical::TypeId>{ship_type}, "PC-02: done 450 frames after the buy");
    expect(ledger(*world, human)->queues[0].empty(), "the queue is empty again");
}

// PC-04, PU-11, PU-14, PU-15.
void test_refusals() {
    auto world = session();
    if (!world) return;
    for (std::uint64_t index = 0; index < 6; ++index) {
        expect(static_cast<bool>(world->submit(buy(0, human, index, human_station, ship_type))), "a human buy is submitted");
        expect(static_cast<bool>(world->submit(buy(0, ai, index, ai_station, ship_type))), "an AI buy is submitted");
    }
    expect(static_cast<bool>(world->submit(buy(1, human, 0, ai_station, ship_type))), "a buy at the enemy station");
    expect(static_cast<bool>(world->submit(buy(1, human, 1, human_station, upgrade_type))), "an upgrade buy");
    const auto events = step_to(*world, 2);
    std::size_t human_accepted = 0;
    std::size_t ai_accepted = 0;
    std::vector<tactical::RejectReason> reasons;
    for (const auto& event : events) {
        if (event.kind == tactical::EventKind::order_accepted) (event.player == human ? human_accepted : ai_accepted) += 1;
        if (event.kind == tactical::EventKind::order_rejected) reasons.push_back(event.reason);
    }
    expect(human_accepted == 5 && ai_accepted == 6, "PC-04: a human queues five, the AI more");
    expect(reasons == std::vector<tactical::RejectReason>{tactical::RejectReason::queue_full,
                          tactical::RejectReason::cannot_produce, tactical::RejectReason::cannot_produce},
        "PU-14, PU-11, PU-20: a full queue, an enemy station and an unavailable upgrade are refused");
    auto poor = session(rules(400));
    if (!poor) return;
    expect(static_cast<bool>(poor->submit(buy(0, human, 0, human_station, ship_type))), "a buy too dear is submitted");
    const auto refused = step_to(*poor, 1);
    expect(!refused.empty() && refused.back().reason == tactical::RejectReason::insufficient_credits
            && std::abs(real(ledger(*poor, human)->credits) - (400 + 1.0 / 6)) < 1e-3,
        "PU-15: too few credits: refused and nothing paid");
}

// PC-03, PU-17.
void test_cancel() {
    auto world = session();
    if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, human, 0, human_station, ship_type))), "buy a ship");
    expect(static_cast<bool>(world->submit(buy(0, human, 1, human_station, squadron_type))), "buy a squadron");
    expect(static_cast<bool>(world->submit(cancel(100, human, 0, 0))), "cancel the ship");
    expect(static_cast<bool>(world->submit(cancel(100, human, 1, 5))), "cancel a missing entry");
    const auto events = step_to(*world, 101);
    const auto retained = world->snapshot();
    const auto cues = retained->economy_cues();
    expect(cues.size() == 3 && cues[0].type == ship_type && cues[1].type == squadron_type
        && cues[2].type == ship_type && cues[2].tick == 100
        && cues[2].kind == tactical::BattleEconomyCue::Kind::cancelled,
        "WPR-30/31: accepted buys and cancel retain their type; refused cancel is silent");
    const tactical::TacticalSnapshot silent(0, {}, {}, {}, {});
    const tactical::TacticalSnapshot annotated(0, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
        std::make_shared<const std::vector<tactical::BattleEconomyCue>>(cues.begin(), cues.end()));
    expect(silent.canonical_bytes() == annotated.canonical_bytes() && silent.sha256() == annotated.sha256(),
        "WPR-30/31: presentation notifications never change canonical snapshot bytes or hashes");
    expect(events.size() >= 2 && events[events.size() - 2].kind == tactical::EventKind::order_accepted
            && events.back().reason == tactical::RejectReason::no_queue_entry,
        "PU-17: the cancel is accepted, a missing entry refused");
    expect(std::abs(real(ledger(*world, human)->credits) - (6000 + 101.0 / 6 - 550)) < 1e-3, "PC-03: the ship's 500 is refunded");
    step_to(*world, 610);
    expect(ledger(*world, human)->pool.empty(), "PC-03: the squadron is not done 509 frames after the cancel");
    step_to(*world, 611);
    expect(retained->economy_cues().size() == 3 && world->snapshot()->economy_cues().size() == 3,
        "WPR-31: immutable retained cue history is stable after completion");
    expect(ledger(*world, human)->pool == std::vector<tactical::TypeId>{squadron_type},
        "PC-03: the squadron completes 510 frames after the cancel made it the front");
}

// PU-18: a lost station's entries go; only an AI is refunded.
void test_station_lost() {
    auto world = session();
    if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, human, 0, human_station, ship_type))), "the human buys");
    expect(static_cast<bool>(world->submit(buy(0, ai, 0, ai_station, ship_type))), "the AI buys");
    step_to(*world, 1);
    const auto human_before = ledger(*world, human)->credits;
    expect(static_cast<bool>(world->stage_remove(human_station)) && static_cast<bool>(world->stage_remove(ai_station)),
        "both stations are removed");
    step_to(*world, 2);
    expect(ledger(*world, human)->queues[0].empty() && ledger(*world, human)->credits == human_before,
        "PU-18: the human's entry goes without refund");
    expect(ledger(*world, ai)->queues[0].empty() && std::abs(real(ledger(*world, ai)->credits) - (6000 + 1.0 / 6)) < 1e-3,
        "PU-18: the AI gets its price back");
    const auto cues = world->snapshot()->economy_cues();
    expect(cues.size() == 4 && cues[2].kind == tactical::BattleEconomyCue::Kind::cancelled
        && cues[2].owner == human && cues[3].owner == ai,
        "WPR-20/31: validity removal retains cancellation feedback for each owner");
}

// PC-05, PU-30 to PU-39: a ship.

} // namespace economy_test_support
