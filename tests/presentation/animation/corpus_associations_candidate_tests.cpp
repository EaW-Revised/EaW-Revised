#include "corpus_associations_test_support.hpp"

namespace association_contracts {
void test_shorter_prefix_passes_but_is_not_promoted() {
    Fixture fixture;
    fixture.models["models/unit.alo"] = model_of(base_bones);
    fixture.models["models/unit_heavy.alo"] = model_of(heavy_bones);
    const auto clip = clip_of({{0, "root"}, {1, "arm"}, {2, "hand"}});
    const auto baseline = binding_failure("models/unit_heavy_attack_00.ala", {"models/unit.alo", "models/unit_heavy.alo"});
    expect(baseline.selected_model == "models/unit_heavy.alo", "shorter passes: baseline still selects the longest prefix");
    const auto row = audit_row(baseline, &clip, fixture.lookup());
    expect(row.status == audit::row_status::single_compatible, "shorter passes: single compatible alternative");
    expect(row.selected && row.selected->status == audit::candidate_status::rejected
        && row.selected->rejection_stage == "binding", "shorter passes: selected model still rejected");
    expect(row.additional_candidates == std::vector<std::string>{"models/unit.alo"}, "shorter passes: one additional candidate");
    expect(row.alternatives.size() == 1 && row.alternatives[0].status == audit::candidate_status::compatible_unapproved,
        "shorter passes: alternative is compatible_unapproved");
    if (row.alternatives.size() == 1) {
        const auto& alternative = row.alternatives[0];
        expect(alternative.player_accepts && alternative.consistent_with_player && !alternative.range_impossible,
            "shorter passes: strict Player accepts and the mirror agrees");
        expect(alternative.samples.size() == 4, "shorter passes: loop 0/mid/duration and clamp terminal sampled");
        bool samples_ok = alternative.samples.size() == 4;
        for (const auto& sample : alternative.samples) samples_ok = samples_ok && sample.ok && sample.finite && sample.deterministic;
        expect(samples_ok, "shorter passes: every sample finite and deterministic");
        expect(alternative.samples.size() == 4 && alternative.samples[3].label == "clamp_terminal"
            && alternative.samples[3].mode == "clamp" && alternative.samples[2].mode == "loop",
            "shorter passes: clamp terminal recorded separately from loop duration");
        expect(alternative.sampled_pose_motion == "moving", "shorter passes: moving clip samples differ");
    }
    expect(row.clip_motion == "moving", "shorter passes: clip disposition moving");
    const std::string json = receipt({row});
    expect(json.find("\"association_approved\": false") != std::string::npos
        && json.find("\"associations_promoted\": 0") != std::string::npos
        && json.find("\"approved\": true") == std::string::npos
        && json.find("\"corpus_acceptance_claimed\": false") != std::string::npos,
        "shorter passes: receipt never approves or promotes");
    corpus::ModelIndex model_index;
    corpus::add_model(model_index, "models/unit.alo");
    corpus::add_model(model_index, "models/unit_heavy.alo");
    expect(corpus::matching_model("models/unit_heavy_attack_00.ala", model_index)
        == std::optional<std::string>("models/unit_heavy.alo"), "shorter passes: baseline selection unchanged after audit");
}

void test_both_pass_is_ambiguous() {
    Fixture fixture;
    fixture.models["models/unit.alo"] = model_of(base_bones);
    fixture.models["models/unit_heavy.alo"] = model_of(base_bones);
    fixture.models["models/unit_heavy_elite.alo"] = model_of(elite_bones);
    const auto clip = clip_of({{0, "root"}, {1, "arm"}, {2, "hand"}});
    const auto baseline = binding_failure("models/unit_heavy_elite_idle_00.ala",
        {"models/unit_heavy_elite.alo", "models/unit.alo", "models/unit_heavy.alo"});
    expect(baseline.selected_model == "models/unit_heavy_elite.alo", "ambiguous: longest prefix selected");
    const auto row = audit_row(baseline, &clip, fixture.lookup());
    expect(row.status == audit::row_status::ambiguous, "ambiguous: two compatible alternatives");
    expect(count_status(row.alternatives, audit::candidate_status::compatible_unapproved) == 2,
        "ambiguous: both recorded compatible_unapproved");
    const auto summary = audit::summarize({row});
    expect(count_of(summary.rows_by_status, audit::row_status::ambiguous) == 1
        && count_of(summary.alternatives_by_status, audit::candidate_status::compatible_unapproved) == 2,
        "ambiguous: counted separately");
}

void test_neither_passes() {
    Fixture fixture;
    fixture.models["models/unit.alo"] = model_of(elite_bones);
    fixture.models["models/unit_heavy.alo"] = model_of(heavy_bones);
    fixture.models["models/unit_heavy_elite.alo"] = model_of({"root", "Arm", "hand"});
    const auto clip = clip_of({{0, "root"}, {1, "arm"}, {2, "hand"}});
    const auto baseline = binding_failure("models/unit_heavy_elite_idle_00.ala",
        {"models/unit.alo", "models/unit_heavy.alo", "models/unit_heavy_elite.alo"});
    const auto row = audit_row(baseline, &clip, fixture.lookup());
    expect(row.status == audit::row_status::no_compatible, "neither: no compatible alternative");
    expect(count_status(row.alternatives, audit::candidate_status::rejected) == 2, "neither: both rejected");
    for (const auto& alternative : row.alternatives)
        expect(alternative.rejection_stage == "binding" && alternative.diagnosis
            && corpus::predicate_signature(*alternative.diagnosis) == std::string(corpus::predicates::bone_name_equal),
            "neither: full name-mismatch diagnosis recorded");
}

void test_unsupported_and_missing_candidates() {
    Fixture fixture;
    fixture.models["models/unit_heavy.alo"] = model_of(heavy_bones);
    audit::CandidateModelView unsupported;
    unsupported.found = true;
    unsupported.identity = identity("models/unit.alo");
    unsupported.error_code = "EAWR-ASSET-0007";
    unsupported.error_message = "unsupported particle root";
    fixture.special["models/unit.alo"] = unsupported;
    const auto clip = clip_of({{0, "root"}, {1, "arm"}, {2, "hand"}});
    const auto baseline = binding_failure("models/unit_heavy_attack_00.ala",
        {"models/unit_heavy.alo", "models/unit.alo"});
    // A listed candidate the effective VFS cannot open is "missing".
    auto with_missing = baseline;
    with_missing.r0_candidates.push_back("models/unit_ghost.alo");
    const auto row = audit_row(with_missing, &clip, fixture.lookup());
    expect(row.status == audit::row_status::no_compatible, "unsupported/missing: no compatible alternative");
    expect(count_status(row.alternatives, audit::candidate_status::parse_failed) == 1
        && count_status(row.alternatives, audit::candidate_status::missing) == 1,
        "unsupported/missing: parse_failed and missing counted separately");
    for (const auto& alternative : row.alternatives) {
        expect(!alternative.diagnosis && alternative.samples.empty() && !alternative.player_accepts,
            "unsupported/missing: Player never ran");
        if (alternative.status == audit::candidate_status::parse_failed)
            expect(alternative.error_code == "EAWR-ASSET-0007" && alternative.found, "unsupported: parser code kept");
    }
    const auto summary = audit::summarize({row});
    expect(count_of(summary.alternatives_by_status, audit::candidate_status::parse_failed) == 1
        && count_of(summary.alternatives_by_status, audit::candidate_status::missing) == 1
        && count_of(summary.alternatives_by_status, audit::candidate_status::compatible_unapproved) == 0,
        "unsupported/missing: summary counts");
}

void test_zero_candidates_and_retained_stages() {
    Fixture fixture;
    fixture.models["models/unit_heavy.alo"] = model_of(heavy_bones);
    const auto clip = clip_of({{0, "root"}, {1, "arm"}, {2, "hand"}});
    const auto single = binding_failure("models/unit_heavy_attack_00.ala", {"models/unit_heavy.alo"});
    const auto row = audit_row(single, &clip, fixture.lookup());
    expect(row.status == audit::row_status::no_additional_candidate && row.alternatives.empty()
        && row.alternatives_evaluated, "zero candidates: distinct status, nothing substituted");
    const auto zero_summary = audit::summarize({row});
    expect(zero_summary.evaluated_rows == 1 && zero_summary.rows_with_alternatives == 0
        && zero_summary.alternative_pairs == 0, "zero candidates: no pair counted");

    audit::BaselineFailure unmatched;
    unmatched.animation = identity("models/padowan_attack_00.ala");
    unmatched.stage = "model_match";
    const auto unmatched_row = audit_row(unmatched, nullptr, fixture.lookup());
    expect(unmatched_row.status == audit::row_status::retained_model_match && !unmatched_row.selected,
        "zero candidates: model_match keeps its cause");
    auto contradictory = unmatched;
    contradictory.r0_candidates = {"models/padowan.alo"};
    expect(audit_row(contradictory, nullptr, fixture.lookup()).status == audit::row_status::baseline_drift,
        "model_match with a candidate is drift");

    audit::BaselineFailure parser_owned;
    parser_owned.animation = identity("models/wing_land_deploy_00.ala");
    parser_owned.stage = "model_parse";
    parser_owned.selected_model = "models/wing_land.alo";
    parser_owned.r0_candidates = {"models/wing_land.alo", "models/wing.alo"};
    audit::CandidateModelView particle;
    particle.found = true;
    particle.identity = identity("models/wing_land.alo");
    particle.error_code = "EAWR-ASSET-0007";
    fixture.special["models/wing_land.alo"] = particle;
    const auto parser_row = audit_row(parser_owned, nullptr, fixture.lookup());
    expect(parser_row.status == audit::row_status::retained_parser_owned && !parser_row.alternatives_evaluated
        && parser_row.alternatives.empty() && parser_row.additional_candidates == std::vector<std::string>{"models/wing.alo"},
        "model_parse keeps parser ownership; its other candidate is listed, not tried");
}

void test_twenty_bone_candidate_rejects_index_twenty() {
    std::vector<std::string> names;
    for (int index = 0; index < 20; ++index) names.push_back("b" + std::to_string(index));
    Fixture fixture;
    fixture.models["models/trooper.alo"] = model_of(names);
    auto heavy = names; heavy[14] = "B_Thigh_L";
    fixture.models["models/trooper_heavy.alo"] = model_of(heavy);
    std::vector<std::pair<std::uint32_t, std::string>> tracks;
    for (std::uint32_t index = 0; index < 20; ++index) tracks.emplace_back(index, "b" + std::to_string(index));
    tracks[13] = {14, "MuzzleB_00"};
    tracks[14] = {20, "MuzzleB_01"};
    const auto clip = clip_of(tracks);
    const auto baseline = binding_failure("models/trooper_heavy_attack_00.ala",
        {"models/trooper_heavy.alo", "models/trooper.alo"});
    const auto row = audit_row(baseline, &clip, fixture.lookup());
    expect(row.status == audit::row_status::no_compatible, "20-bone: shorter fallback is not compatible");
    expect(row.alternatives.size() == 1, "20-bone: one alternative");
    if (row.alternatives.size() != 1) return;
    const auto& alternative = row.alternatives[0];
    expect(alternative.status == audit::candidate_status::rejected && alternative.rejection_stage == "binding",
        "20-bone: strict binding rejects");
    expect(alternative.range_impossible && alternative.bone_count == std::size_t{20}
        && alternative.max_track_bone_index == std::uint32_t{20}, "20-bone: index 20 is range-impossible");
    const bool has_range = alternative.diagnosis && std::any_of(alternative.diagnosis->failures.begin(),
        alternative.diagnosis->failures.end(), [](const corpus::PredicateFailure& failure) {
            return failure.predicate == corpus::predicates::bone_index_in_range && failure.index_out_of_range
                && failure.track_bone_index == std::uint32_t{20};
        });
    expect(has_range, "20-bone: diagnosis records index 20 out of range");
    const auto summary = audit::summarize({row});
    expect(summary.range_impossible_pairs == 1 && summary.range_impossible_rejected == 1,
        "20-bone: range-impossible pair counted as rejected");
}

void test_selected_drift_fails_closed() {
    Fixture fixture;
    fixture.models["models/unit.alo"] = model_of(base_bones);
    fixture.models["models/unit_heavy.alo"] = model_of(base_bones);
    const auto clip = clip_of({{0, "root"}, {1, "arm"}, {2, "hand"}});
    const auto baseline = binding_failure("models/unit_heavy_attack_00.ala", {"models/unit.alo", "models/unit_heavy.alo"});
    const auto row = audit_row(baseline, &clip, fixture.lookup());
    expect(row.status == audit::row_status::baseline_drift && !row.alternatives_evaluated,
        "drift: selected model that now binds is not audited");
    const auto no_clip = audit_row(baseline, nullptr, fixture.lookup());
    expect(no_clip.status == audit::row_status::baseline_drift, "drift: binding row whose clip did not reload");
    expect(audit::summarize({row, no_clip}).drift_rows == 2, "drift: counted for the caller to refuse the receipt");
}

void test_frozen_input_gate() {
    const std::string pinned(audit::pinned_frozen_ledger_sha256);
    expect(!audit::check_frozen_input(pinned, pinned), "frozen gate: pinned and re-emitted ledger accepted");
    expect(audit::check_frozen_input(std::nullopt, pinned).has_value(), "frozen gate: missing ledger refused");
    expect(audit::check_frozen_input(std::string{}, pinned).has_value(), "frozen gate: empty hash refused");
    const std::string stale(64, '0');
    expect(audit::check_frozen_input(stale, stale).has_value(), "frozen gate: stale ledger refused even if re-emitted");
    expect(audit::check_frozen_input(pinned, stale).has_value(), "frozen gate: re-emitted drift refused");
    expect(audit::check_frozen_input(pinned, std::string{}).has_value(), "frozen gate: unreadable re-emitted ledger refused");
}

void test_static_clip_disposition() {
    Fixture fixture;
    fixture.models["models/unit.alo"] = model_of(base_bones);
    fixture.models["models/unit_heavy.alo"] = model_of(heavy_bones);
    auto clip = clip_of({{0, "root"}, {1, "arm"}});
    for (auto& item : clip.tracks) item.samples = {item.samples[0], item.samples[0], item.samples[0]};
    const auto row = audit_row(
        binding_failure("models/unit_heavy_idle_00.ala", {"models/unit.alo", "models/unit_heavy.alo"}), &clip, fixture.lookup());
    expect(row.clip_motion == "static", "static: identical samples give a static clip");
    expect(row.alternatives.size() == 1 && row.alternatives[0].sampled_pose_motion == "static",
        "static: compatible candidate poses do not vary");
}

// Enumeration, candidate and row order must not change a single byte.
void test_shuffled_enumeration_is_deterministic() {
    const std::vector<std::string> model_paths{"models/unit.alo", "models/unit_heavy.alo", "models/unit_heavy_elite.alo",
        "models/tank.alo", "models/tank_heavy.alo"};
    Fixture fixture;
    fixture.models["models/unit.alo"] = model_of(base_bones);
    fixture.models["models/unit_heavy.alo"] = model_of(base_bones);
    fixture.models["models/unit_heavy_elite.alo"] = model_of(elite_bones);
    fixture.models["models/tank.alo"] = model_of(elite_bones);
    fixture.models["models/tank_heavy.alo"] = model_of(heavy_bones);
    const auto clip = clip_of({{0, "root"}, {1, "arm"}, {2, "hand"}});
    const std::vector<std::string> animations{"models/unit_heavy_elite_idle_00.ala", "models/tank_heavy_move_00.ala",
        "models/unit_heavy_elite_die_00.ala"};
    const auto build = [&](std::vector<std::string> models_order, std::vector<std::string> animation_order) {
        std::vector<audit::AuditRow> rows;
        for (const auto& ala : animation_order) {
            auto baseline = binding_failure(ala, models_order, 0);
            baseline.baseline_failure_index = static_cast<std::size_t>(
                std::find(animations.begin(), animations.end(), ala) - animations.begin()) + 1;
            rows.push_back(audit_row(baseline, &clip, fixture.lookup()));
        }
        return receipt(rows);
    };
    const std::string reference = build(model_paths, animations);
    expect(reference.find("tank_heavy_move_00") < reference.find("unit_heavy_elite_die_00")
        && reference.find("unit_heavy_elite_die_00") < reference.find("unit_heavy_elite_idle_00"),
        "shuffle: rows sorted by ALA path");
    const std::size_t first_alternative = reference.find("\"alternatives\": [{\"path\": \"models/unit.alo\"");
    expect(first_alternative != std::string::npos, "shuffle: alternatives sorted by candidate path");
    // The writer sorts alternatives itself, even when handed them reversed.
    {
        auto ambiguous = audit_row(binding_failure(animations[0], model_paths), &clip, fixture.lookup());
        const std::string sorted_receipt = receipt({ambiguous});
        std::reverse(ambiguous.alternatives.begin(), ambiguous.alternatives.end());
        expect(ambiguous.alternatives.size() == 2 && receipt({ambiguous}) == sorted_receipt,
            "shuffle: writer sorts reversed alternatives");
    }
    std::mt19937 random(24U);
    for (int round = 0; round < 32; ++round) {
        auto models_order = model_paths;
        auto animation_order = animations;
        std::shuffle(models_order.begin(), models_order.end(), random);
        std::shuffle(animation_order.begin(), animation_order.end(), random);
        expect(build(models_order, animation_order) == reference, "shuffle: identical receipt bytes");
    }
}

void test_receipt_escaping() {
    Fixture fixture;
    fixture.models["models/we\"ird\tunit.alo"] = model_of(base_bones);
    fixture.models["models/we\"ird\tunit_heavy.alo"] = model_of(heavy_bones);
    const auto clip = clip_of({{0, "root"}, {1, "arm"}, {2, "hand"}});
    const auto row = audit_row(binding_failure("models/we\"ird\tunit_heavy_\x01x.ala",
        {"models/we\"ird\tunit.alo", "models/we\"ird\tunit_heavy.alo"}), &clip, fixture.lookup());
    const std::string json = receipt({row});
    expect(json.find("we\\\"ird\\tunit_heavy_\\u0001x.ala") != std::string::npos, "escaping: quote, tab and control escaped");
    const bool no_raw_controls = std::none_of(json.begin(), json.end(), [](const char character) {
        return static_cast<unsigned char>(character) < 0x20U && character != '\n';
    });
    expect(no_raw_controls, "escaping: no raw control byte other than record newlines");
    expect(json.find("\"time_seconds\": 1,") != std::string::npos, "escaping: midpoint time is a JSON number");
}

} // namespace association_contracts
