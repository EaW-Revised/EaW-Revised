#include "corpus_associations_test_support.hpp"

namespace association_contracts {
// ---------------------------------------------------------------------------
// F1: frozen metadata, not just the v1 ledger, pins model identity and R0.
// ---------------------------------------------------------------------------

// The trooper-shaped fixture used by the frozen-identity tests: the selected
// heavy model fails on a name, the shorter-stem model binds.
struct IdentityCase final {
    Fixture fixture;
    assets::Animation clip = clip_of({{0, "root"}, {1, "arm"}, {2, "hand"}});
    audit::BaselineFailure baseline;
    audit::FrozenMetadata frozen;
    IdentityCase() {
        fixture.models["models/unit.alo"] = model_of(base_bones);
        fixture.models["models/unit_heavy.alo"] = model_of(heavy_bones);
        baseline = binding_failure("models/unit_heavy_attack_00.ala", {"models/unit.alo", "models/unit_heavy.alo"});
        frozen = frozen_of({baseline}, fixture.lookup());
    }
    [[nodiscard]] audit::AuditRow run() const { return audit::audit_failure(baseline, &clip, fixture.lookup(), frozen); }
};

void test_frozen_identity_unchanged_is_accepted() {
    const IdentityCase item;
    const auto row = item.run();
    expect(row.status == audit::row_status::single_compatible, "frozen identity: unchanged fixture audits normally");
    expect(row.selected && row.selected->frozen_identity == audit::frozen_identity::matched,
        "frozen identity: selected model matched");
    expect(row.alternatives.size() == 1 && row.alternatives[0].frozen_identity == audit::frozen_identity::matched,
        "frozen identity: recorded alternative matched");
    const auto summary = audit::summarize({row});
    expect(summary.selected_frozen_identity_matched == 1
        && count_of(summary.alternatives_by_frozen_identity, audit::frozen_identity::matched) == 1
        && count_of(summary.alternatives_by_frozen_identity, audit::frozen_identity::unrecorded) == 0,
        "frozen identity: summary counts matched identities");
    expect(!audit::check_frozen_failures({item.baseline}, item.frozen), "frozen identity: failure set accepted");
}

// The independent review's reproduction: a different selected model that still
// fails with the same Player code and message.
void test_changed_selected_model_same_error_is_drift() {
    IdentityCase item;
    const assets::Model changed = model_of({"root", "gun", "hand", "tail"});
    audit::CandidateModelView view;
    view.found = true;
    view.identity = {"models/unit_heavy.alo", "sha-changed", "expansion", "archive", "expansion:Data/Models.meg",
        "DATA\\ART\\MODELS\\UNIT_HEAVY.ALO", 200};
    view.model = &changed;
    item.fixture.special["models/unit_heavy.alo"] = view;
    // Without the frozen identity check the Player-level check cannot see it:
    // re-freezing the changed identity makes the row audit normally.
    auto refrozen = item.frozen;
    refrozen.models["models/unit_heavy.alo"] = {view.identity, changed.bones.size()};
    const auto unguarded = audit::audit_failure(item.baseline, &item.clip, item.fixture.lookup(), refrozen);
    expect(unguarded.status == audit::row_status::single_compatible && unguarded.selected
        && unguarded.selected->player_code == item.baseline.code && unguarded.selected->player_message == item.baseline.cause,
        "changed selected: same Player code and message as the frozen failure");
    const auto row = item.run();
    expect(row.status == audit::row_status::baseline_drift && contains(row.drift_reason, "sha256")
        && !row.alternatives_evaluated && row.alternatives.empty(), "changed selected: hash drift fails closed");
    expect(audit::summarize({row}).drift_rows == 1, "changed selected: counted as drift for the caller");
}

void test_selected_provenance_drift() {
    const std::vector<std::pair<std::string, std::function<void(audit::AssetIdentity&)>>> fields{
        {"layer_id", [](audit::AssetIdentity& id) { id.layer_id = "base"; }},
        {"origin", [](audit::AssetIdentity& id) { id.origin = "archive"; }},
        {"source_id", [](audit::AssetIdentity& id) { id.source_id = "base:Data/Models.meg"; }},
        {"original_path", [](audit::AssetIdentity& id) { id.original_path = "DATA\\ART\\MODELS\\UNIT_HEAVY.ALO"; }},
        {"size", [](audit::AssetIdentity& id) { id.size = 101; }},
    };
    for (const auto& [name, mutate] : fields) {
        IdentityCase item;
        auto view = item.fixture.lookup()("models/unit_heavy.alo");
        mutate(view.identity);
        item.fixture.special["models/unit_heavy.alo"] = view;
        const auto row = item.run();
        expect(row.status == audit::row_status::baseline_drift && contains(row.drift_reason, "selected")
            && contains(row.drift_reason, name), "selected provenance drift fails closed: " + name);
    }
    IdentityCase bones;
    bones.frozen.models["models/unit_heavy.alo"].bone_count = 99;
    expect(bones.run().status == audit::row_status::baseline_drift, "selected bone count drift fails closed");
    IdentityCase unrecorded;
    unrecorded.frozen.models.erase("models/unit_heavy.alo");
    unrecorded.frozen.unrecorded = {"models/unit_heavy.alo"};
    expect(unrecorded.run().status == audit::row_status::baseline_drift,
        "selected model without a frozen identity fails closed");
    IdentityCase vanished;
    vanished.fixture.models.erase("models/unit_heavy.alo");
    expect(vanished.run().status == audit::row_status::baseline_drift, "selected model no longer found fails closed");
}

void test_alternative_identity_drift_and_unrecorded() {
    IdentityCase changed;
    auto view = changed.fixture.lookup()("models/unit.alo");
    view.identity.sha256 = "sha-other";
    changed.fixture.special["models/unit.alo"] = view;
    const auto row = changed.run();
    expect(row.status == audit::row_status::baseline_drift && contains(row.drift_reason, "alternative")
        && contains(row.drift_reason, "sha256"), "recorded alternative hash drift fails closed");
    IdentityCase vanished;
    vanished.fixture.models.erase("models/unit.alo");
    expect(vanished.run().status == audit::row_status::baseline_drift, "recorded alternative now missing fails closed");

    // Run-4 recorded no identity for a candidate it never selected: accepted,
    // marked unrecorded and counted, never silently "matched".
    IdentityCase unrecorded;
    unrecorded.frozen.models.erase("models/unit.alo");
    unrecorded.frozen.unrecorded = {"models/unit.alo"};
    const auto accepted = unrecorded.run();
    expect(accepted.status == audit::row_status::single_compatible && accepted.alternatives.size() == 1
        && accepted.alternatives[0].frozen_identity == audit::frozen_identity::unrecorded,
        "unrecorded alternative evaluated and marked unrecorded");
    const auto summary = audit::summarize({accepted});
    expect(count_of(summary.alternatives_by_frozen_identity, audit::frozen_identity::unrecorded) == 1
        && count_of(summary.alternatives_by_frozen_identity, audit::frozen_identity::matched) == 0,
        "unrecorded alternative counted separately");
    expect(contains(receipt({accepted}), "\"frozen_identity\": \"unrecorded\""), "unrecorded identity written to receipt");
    IdentityCase unknown;
    unknown.frozen.models.erase("models/unit.alo");
    expect(unknown.run().status == audit::row_status::baseline_drift,
        "alternative neither recorded nor listed unrecorded fails closed");
}

void test_candidate_enumeration_drift() {
    Fixture fixture;
    fixture.models["models/unit.alo"] = model_of(base_bones);
    fixture.models["models/unit_heavy.alo"] = model_of(heavy_bones);
    const auto clip = clip_of({{0, "root"}, {1, "arm"}, {2, "hand"}});
    const auto both = binding_failure("models/unit_heavy_attack_00.ala", {"models/unit.alo", "models/unit_heavy.alo"});
    const auto only_heavy = binding_failure("models/unit_heavy_attack_00.ala", {"models/unit_heavy.alo"});
    expect(both.selected_model == only_heavy.selected_model && both.code == only_heavy.code && both.cause == only_heavy.cause,
        "enumeration drift: selected path, code and cause unchanged (the v1 ledger cannot see it)");
    // A shorter candidate appears that the frozen R0 list does not name.
    const auto added = frozen_of({only_heavy}, fixture.lookup());
    const auto added_row = audit::audit_failure(both, &clip, fixture.lookup(), added);
    expect(added_row.status == audit::row_status::baseline_drift && contains(added_row.drift_reason, "r0_candidates")
        && added_row.alternatives.empty(), "enumeration drift: added candidate fails closed");
    const auto added_error = audit::check_frozen_failures({both}, added);
    expect(added_error && contains(*added_error, "r0_candidates"), "enumeration drift: failure-set check sees an addition");
    // A frozen candidate disappears.
    const auto removed = frozen_of({both}, fixture.lookup());
    expect(audit::audit_failure(only_heavy, &clip, fixture.lookup(), removed).status == audit::row_status::baseline_drift,
        "enumeration drift: removed candidate fails closed");
    expect(audit::check_frozen_failures({only_heavy}, removed).has_value(), "enumeration drift: failure-set check sees a removal");
    auto reordered = both;
    std::reverse(reordered.r0_candidates.begin(), reordered.r0_candidates.end());
    expect(audit::check_frozen_failures({reordered}, removed).has_value(), "enumeration drift: R0 order is frozen");
}

void test_failure_set_drift() {
    const IdentityCase item;
    const auto mutated = [&item](const std::function<void(audit::BaselineFailure&)>& mutate) {
        auto failure = item.baseline;
        mutate(failure);
        return audit::check_frozen_failures({failure}, item.frozen);
    };
    expect(mutated([](audit::BaselineFailure& f) { f.animation.sha256 = "x"; }).has_value(), "failure set: ALA hash");
    expect(mutated([](audit::BaselineFailure& f) { f.animation.source_id = "base:Data/Models.meg"; }).has_value(),
        "failure set: ALA provenance");
    expect(mutated([](audit::BaselineFailure& f) { f.stage = "sampling"; }).has_value(), "failure set: stage");
    expect(mutated([](audit::BaselineFailure& f) { f.code = "X"; }).has_value(), "failure set: code");
    expect(mutated([](audit::BaselineFailure& f) { f.cause = "X"; }).has_value(), "failure set: cause");
    expect(mutated([](audit::BaselineFailure& f) { f.selected_model = "models/unit.alo"; }).has_value(),
        "failure set: selected model path");
    expect(mutated([](audit::BaselineFailure& f) { f.baseline_failure_index = 2; }).has_value(), "failure set: index");
    expect(audit::check_frozen_failures({}, item.frozen).has_value(), "failure set: a frozen failure disappeared");
    expect(audit::check_frozen_failures({item.baseline, item.baseline}, item.frozen).has_value(),
        "failure set: an extra failure appeared");
    auto frozen_two = item.frozen;
    auto second = item.baseline;
    second.baseline_failure_index = 2;
    frozen_two.failures[2] = second;
    expect(audit::check_frozen_failures({item.baseline, item.baseline}, frozen_two).has_value(),
        "failure set: duplicate index refused");
    expect(audit::audit_failure(second, &item.clip, item.fixture.lookup(), item.frozen).status
        == audit::row_status::baseline_drift, "failure set: row without a frozen record fails closed");
}

// ---------------------------------------------------------------------------
// Frozen metadata manifest parsing and authentication.
// ---------------------------------------------------------------------------

std::string hex(const char digit) { return std::string(64, digit); }

std::string manifest_text() {
    const std::string source(audit::pinned_frozen_metadata_source_sha256);
    const std::string ledger(audit::pinned_frozen_ledger_sha256);
    return std::string(audit::frozen_metadata_schema) + "\t1\n"
        + "rule\t" + std::string(corpus::r0_rule_id) + "\n"
        + "source\tdiagnostics_sha256\t" + source + "\n"
        + "source\tledger_sha256\t" + ledger + "\n"
        + "counts\t3\t1\t2\n"
        + "failure\t1\tbinding\tEAWR-ANIMATION-0002\tname mismatch\tmodels/unit_heavy_attack_00.ala\t" + hex('a')
        + "\tmod\tloose\tmod:loose:x\tx\t100\tmodels/unit_heavy.alo\tmodels/unit_heavy.alo\tmodels/unit.alo\n"
        + "failure\t2\tmodel_match\tEAWR-ANIMATION-CORPUS-0001\tno prefix\tmodels/pad_00.ala\t" + hex('b')
        + "\tbase\tarchive\tbase:Data/Models.meg\tDATA\\PAD_00.ALA\t10\t\n"
        + "model\tmodels/unit_heavy.alo\t" + hex('c') + "\tmod\tloose\tmod:loose:y\ty\t5\t4\n"
        + "unrecorded\tmodels/unit.alo\n";
}

audit::FrozenMetadataLoad load_text(const std::string& text) {
    return audit::load_frozen_metadata(text, std::string("pin"), "pin");
}

void test_frozen_metadata_parsing() {
    const auto load = load_text(manifest_text());
    expect(load.metadata.has_value(), "manifest: canonical synthetic manifest accepted: " + load.error);
    if (load.metadata) {
        const auto& metadata = *load.metadata;
        expect(metadata.failures.size() == 2 && metadata.models.size() == 1 && metadata.unrecorded.size() == 1
            && metadata.animation_count == 3 && metadata.playback_passed == 1 && metadata.failure_count == 2,
            "manifest: counts and records");
        const auto& failure = metadata.failures.at(1);
        expect(failure.selected_model == "models/unit_heavy.alo"
            && failure.r0_candidates == std::vector<std::string>{"models/unit_heavy.alo", "models/unit.alo"}
            && failure.animation.sha256 == hex('a') && failure.animation.size == 100,
            "manifest: failure fields in order");
        expect(metadata.failures.at(2).selected_model.empty() && metadata.failures.at(2).r0_candidates.empty()
            && metadata.failures.at(2).animation.original_path == "DATA\\PAD_00.ALA", "manifest: model_match failure");
        expect(metadata.models.at("models/unit_heavy.alo").bone_count == std::size_t{4}
            && metadata.is_unrecorded("models/unit.alo"), "manifest: model and unrecorded records");
        expect(metadata.sha256 == "pin" && metadata.bytes == manifest_text().size(), "manifest: authenticated identity kept");
    }
    // Authentication.
    expect(audit::load_frozen_metadata(std::nullopt, std::nullopt, "pin").error.find("missing") != std::string::npos,
        "manifest: missing refused");
    expect(audit::load_frozen_metadata(std::string{}, std::string("pin"), "pin").error.find("missing") != std::string::npos,
        "manifest: empty refused");
    expect(audit::load_frozen_metadata(manifest_text(), std::string("other"), "pin").error.find("stale") != std::string::npos,
        "manifest: unpinned hash refused as stale");
    expect(!audit::load_frozen_metadata(manifest_text(), std::string("pin"), "pin", hex('0')).metadata,
        "manifest: wrong source diagnostics refused");
    expect(!audit::load_frozen_metadata(manifest_text(), std::string("pin"), "pin",
        audit::pinned_frozen_metadata_source_sha256, hex('0')).metadata, "manifest: wrong source ledger refused");
    expect(audit::load_frozen_metadata(manifest_text(), std::string("pin")).error.find("stale") != std::string::npos,
        "manifest: default pin refuses a synthetic manifest");

    const auto replaced = [](std::string text, const std::string& from, const std::string& to) {
        const auto at = text.find(from);
        if (at == std::string::npos) return std::string("pattern not found: ") + from;
        return text.replace(at, from.size(), to);
    };
    const std::string good = manifest_text();
    const std::string model_line = "model\tmodels/unit_heavy.alo\t" + hex('c') + "\tmod\tloose\tmod:loose:y\ty\t5\t4\n";
    const std::vector<std::pair<std::string, std::string>> malformed{
        {"CRLF", replaced(good, "\n", "\r\n")},
        {"CR inside a field", replaced(good, "name mismatch", "name\rmismatch")},
        {"missing final LF", good.substr(0, good.size() - 1)},
        {"schema version", replaced(good, "\t1\nrule", "\t2\nrule")},
        {"rule", replaced(good, "R0-same", "R1-same")},
        {"uppercase hex", replaced(good, hex('a'), std::string(64, 'A'))},
        {"short hex", replaced(good, hex('a'), hex('a').substr(1))},
        {"index gap", replaced(good, "failure\t2\t", "failure\t3\t")},
        {"failure count", replaced(good, "counts\t3\t1\t2", "counts\t4\t1\t3")},
        {"inconsistent counts", replaced(good, "counts\t3\t1\t2", "counts\t9\t1\t2")},
        {"leading zero size", replaced(good, "\t100\t", "\t0100\t")},
        {"negative size", replaced(good, "\t100\t", "\t-100\t")},
        {"selected not first", replaced(good, "models/unit_heavy.alo\tmodels/unit_heavy.alo\tmodels/unit.alo",
            "models/unit_heavy.alo\tmodels/unit.alo\tmodels/unit_heavy.alo")},
        {"duplicate candidate", replaced(good, "models/unit_heavy.alo\tmodels/unit.alo\n",
            "models/unit_heavy.alo\tmodels/unit.alo\tmodels/unit.alo\n")},
        // Well formed except for the stage rule, so no other check catches it.
        {"model_match with selected", replaced(replaced(good, "\t10\t\n", "\t10\tmodels/pad.alo\tmodels/pad.alo\n"),
            "unrecorded\tmodels/unit.alo\n", "unrecorded\tmodels/pad.alo\nunrecorded\tmodels/unit.alo\n")},
        {"binding without selected", replaced(good, "\t100\tmodels/unit_heavy.alo\tmodels/unit_heavy.alo",
            "\t100\t\tmodels/unit_heavy.alo")},
        {"unrecorded not named", good + "unrecorded\tmodels/zzz.alo\n"},
        {"candidate not covered", replaced(good, "unrecorded\tmodels/unit.alo\n", "")},
        {"unrecorded also recorded", replaced(good, "unrecorded\tmodels/unit.alo\n", "unrecorded\tmodels/unit_heavy.alo\n")},
        {"model before failure", replaced(good, model_line, "") + model_line},
        {"duplicate model", replaced(good, model_line, model_line + model_line)},
        {"bad bone count", replaced(good, "\ty\t5\t4\n", "\ty\t5\tfour\n")},
        {"model field count", replaced(good, "\ty\t5\t4\n", "\ty\t5\t4\textra\n")},
        {"unknown record", replaced(good, "unrecorded\t", "unknown\t")},
        {"empty line", replaced(good, "counts", "\ncounts")},
        {"control byte", replaced(good, "name mismatch", "name\x01mismatch")},
        {"empty layer", replaced(good, "\tmod\tloose\tmod:loose:x", "\t\tloose\tmod:loose:x")},
    };
    for (const auto& [label, text] : malformed) {
        const auto result = load_text(text);
        expect(!result.metadata && !result.error.empty(), "manifest: malformed refused: " + label);
    }
}

#if defined(EAWR_ASSOCIATION_FROZEN_MANIFEST)
// The tracked manifest is metadata only; parse it (its pin is checked by the
// CLI test, which has a SHA-256 implementation).
void test_tracked_manifest_parses() {
    std::ifstream input(EAWR_ASSOCIATION_FROZEN_MANIFEST, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto load = audit::load_frozen_metadata(text, std::string("tracked"), "tracked");
    expect(load.metadata.has_value(), "tracked manifest parses: " + load.error);
    if (!load.metadata) return;
    const auto& metadata = *load.metadata;
    expect(metadata.animation_count == 7685 && metadata.playback_passed == 7329 && metadata.failure_count == 356,
        "tracked manifest: frozen 7,685 / 7,329 / 356");
    expect(metadata.models.size() == 76 && metadata.unrecorded.size() == 4, "tracked manifest: 76 records, 4 unrecorded");
    std::map<std::string, std::size_t> stages;
    for (const auto& [index, failure] : metadata.failures) ++stages[failure.stage];
    expect(stages["binding"] == 328 && stages["model_match"] == 22 && stages["model_parse"] == 6,
        "tracked manifest: 328 / 22 / 6 failure stages");
    const auto trooper = metadata.models.find("data/art/models/ei_armytrooper.alo");
    expect(trooper != metadata.models.end()
        && trooper->second.identity.sha256 == "519cd1914897835a45e6a00907d0e0e95ec78b6e9a939763344d0d839a25ed76"
        && trooper->second.bone_count == std::size_t{24}, "tracked manifest: priority alternative identity");
    const bool no_host_path = text.find(":\\") == std::string::npos && text.find(":/") == std::string::npos;
    expect(no_host_path, "tracked manifest: no host path");
}
#endif

// ---------------------------------------------------------------------------
// F2: no output may alias a frozen input or another output.
// ---------------------------------------------------------------------------

struct TempDirectory final {
    std::filesystem::path path;
    TempDirectory() {
        std::random_device device;
        path = std::filesystem::temp_directory_path() / ("eawr-association-paths-" + std::to_string(device()));
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
};

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_output_path_aliasing() {
    const TempDirectory temp;
    const auto ledger = temp.path / "frozen-ledger.json";
    const auto metadata = temp.path / "frozen-metadata.tsv";
    const std::string ledger_bytes = "frozen ledger bytes\n";
    const std::string metadata_bytes = "frozen metadata bytes\n";
    write_bytes(ledger, ledger_bytes);
    write_bytes(metadata, metadata_bytes);
    std::filesystem::create_directories(temp.path / "sub");
    const auto roles = [&](const std::filesystem::path& report, const std::filesystem::path& diagnostics,
                           const std::filesystem::path& frozen_ledger, const std::filesystem::path& frozen_metadata,
                           const std::filesystem::path& output) {
        return std::vector<audit::PathRole>{{"report", report}, {"diagnostics", diagnostics},
            {"frozen ledger", frozen_ledger}, {"frozen metadata", frozen_metadata}, {"audit", output}};
    };
    const auto report = temp.path / "report.json";
    const auto diagnostics = temp.path / "diagnostics.json";
    const auto output = temp.path / "audit.json";
    expect(!audit::check_distinct_paths(roles(report, diagnostics, ledger, metadata, output)),
        "paths: distinct roles accepted");

    std::vector<std::pair<std::string, std::vector<audit::PathRole>>> aliases{
        {"audit = frozen ledger", roles(report, diagnostics, ledger, metadata, ledger)},
        {"report = frozen ledger", roles(ledger, diagnostics, ledger, metadata, output)},
        {"diagnostics = frozen metadata", roles(report, metadata, ledger, metadata, output)},
        {"audit = frozen metadata via ..", roles(report, diagnostics, ledger, metadata, temp.path / "sub" / ".." / "frozen-metadata.tsv")},
        {"report = frozen ledger via .", roles(temp.path / "." / "frozen-ledger.json", diagnostics, ledger, metadata, output)},
        {"report = diagnostics", roles(report, report, ledger, metadata, output)},
        {"audit = diagnostics (not yet existing)", roles(report, diagnostics, ledger, metadata, diagnostics)},
        {"audit = report via ..", roles(report, diagnostics, ledger, metadata, temp.path / "sub" / ".." / "report.json")},
        {"frozen ledger = frozen metadata", roles(report, diagnostics, ledger, ledger, output)},
        {"trailing separator", roles(report, diagnostics, ledger, metadata, ledger.string() + "/")},
    };
    {
        // Relative spelling of an absolute path, existing and not yet existing.
        const auto relative = std::filesystem::relative(ledger);
        if (!relative.empty() && relative != ledger)
            aliases.push_back({"relative = absolute", roles(report, diagnostics, ledger, metadata, relative)});
        const auto relative_output = std::filesystem::relative(output);
        if (!relative_output.empty() && relative_output != output)
            aliases.push_back({"relative = absolute (not yet existing)",
                roles(output, diagnostics, ledger, metadata, relative_output)});
    }
    {
        // Two not-yet-existing outputs reached through a directory link.
        std::error_code error;
        std::filesystem::create_directories(temp.path / "real");
        std::filesystem::create_directory_symlink(temp.path / "real", temp.path / "linked", error);
        if (!error)
            aliases.push_back({"directory link (not yet existing)", roles(temp.path / "real" / "new.json", diagnostics,
                ledger, metadata, temp.path / "linked" / "new.json")});
        else std::cout << "note: directory link unavailable (" << error.message() << ")\n";
    }
#if defined(_WIN32)
    auto upper = ledger.wstring();
    for (auto& character : upper) character = static_cast<wchar_t>(std::towupper(static_cast<std::wint_t>(character)));
    aliases.push_back({"case variant", roles(report, diagnostics, ledger, metadata, upper)});
    auto upper_output = output.wstring();
    for (auto& character : upper_output) character = static_cast<wchar_t>(std::towupper(static_cast<std::wint_t>(character)));
    aliases.push_back({"case variant (not yet existing)", roles(output, diagnostics, ledger, metadata, upper_output)});
    auto backslash = ledger.generic_wstring();
    for (auto& character : backslash) if (character == L'/') character = L'\\';
    aliases.push_back({"backslash separators", roles(report, diagnostics, ledger, metadata, backslash)});
    aliases.push_back({"trailing dot", roles(report, diagnostics, ledger, metadata, ledger.wstring() + L".")});
    aliases.push_back({"trailing dot (not yet existing)",
        roles(output, diagnostics, ledger, metadata, output.wstring() + L".")});
#endif
    std::error_code link_error;
    const auto hard = temp.path / "hard-link.json";
    std::filesystem::create_hard_link(ledger, hard, link_error);
    if (!link_error) aliases.push_back({"hard link to frozen ledger", roles(report, diagnostics, ledger, metadata, hard)});
    else std::cout << "note: hard link unavailable (" << link_error.message() << ")\n";
    const auto soft = temp.path / "symbolic-link.tsv";
    std::filesystem::create_symlink(metadata, soft, link_error);
    if (!link_error) aliases.push_back({"symbolic link to frozen metadata", roles(soft, diagnostics, ledger, metadata, output)});
    else std::cout << "note: symbolic link unavailable (" << link_error.message() << ")\n";

    for (const auto& [label, case_roles] : aliases) {
        const auto error = audit::check_distinct_paths(case_roles);
        expect(error && contains(*error, "name the same file"), "paths: alias refused: " + label);
    }
    expect(audit::check_distinct_paths({{"report", {}}}).has_value(), "paths: empty path refused");
    // Refusal opens nothing: the frozen bytes are intact and no output exists.
    expect(read_bytes(ledger) == ledger_bytes && read_bytes(metadata) == metadata_bytes, "paths: frozen bytes intact");
    expect(!std::filesystem::exists(report) && !std::filesystem::exists(diagnostics) && !std::filesystem::exists(output),
        "paths: no output created");
}
} // namespace association_contracts
