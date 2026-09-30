#include "corpus_diagnostics.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace eawr;
namespace playback = eawr::presentation::animation;
namespace corpus = eawr::tests::animation_corpus;
namespace predicates = corpus::predicates;

int failures{};
void expect(const bool condition, const std::string_view message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; ++failures; }
}

assets::Bone bone(const std::string_view name, const std::int32_t parent) {
    assets::Bone value;
    value.name = name; value.parent = parent; value.visible = true;
    value.relative_transform = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    return value;
}
assets::Model model_data() {
    assets::Model value;
    value.source.logical_path = "synthetic.alo";
    // "tail" is intentionally untracked: Player permits untracked model bones.
    value.bones = {bone("root", -1), bone("arm", 0), bone("hand", 1), bone("tail", 0)};
    return value;
}
assets::AnimationTrack track(const std::uint32_t index, const std::string_view name) {
    assets::AnimationTrack value;
    value.bone_index = index; value.bone_name = name;
    value.samples = {
        {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
        {{1.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
        {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
    };
    return value;
}
assets::Animation animation_data() {
    assets::Animation value;
    value.source.logical_path = "synthetic.ala";
    value.stored_frame_count = 3; value.playable_frame_count = 2;
    value.frames_per_second = 1.0F; value.duration_seconds = 2.0F;
    value.tracks = {track(0, "root"), track(1, "arm"), track(2, "hand")};
    return value;
}

// The diagnosis must match Player::create's accept/reject, code and message;
// its first-failing-track field is a prediction from Player's check order.
corpus::BindingDiagnosis diagnose_consistently(
    const assets::Model& model, const assets::Animation& animation, const std::string_view label) {
    const auto player = playback::Player::create(model, &animation);
    const auto diagnosis = corpus::diagnose_binding(model, animation);
    const bool consistent = diagnosis.player_accepts == static_cast<bool>(player)
        && (player || (diagnosis.player_code == player.error().code
            && diagnosis.player_message == player.error().message));
    expect(consistent, std::string(label) + ": diagnosis matches Player::create accept/reject, code and message");
    return diagnosis;
}

// Safe first-failure accessor so a failed expectation cannot crash the suite.
corpus::PredicateFailure first(const corpus::BindingDiagnosis& diagnosis) {
    return diagnosis.failures.empty() ? corpus::PredicateFailure{} : diagnosis.failures.front();
}

// Exactly one failed predicate, on the named track, with exact identities.
void expect_single(const corpus::BindingDiagnosis& diagnosis, const std::string_view predicate,
    const std::size_t ordinal, const std::optional<std::string>& model_name, const bool out_of_range,
    const std::string_view label) {
    const std::string prefix(label);
    expect(!diagnosis.player_accepts, prefix + ": Player rejects");
    expect(diagnosis.player_code == playback::diagnostic_codes::invalid_animation, prefix + ": baseline code preserved");
    expect(diagnosis.failures.size() == 1, prefix + ": exactly one predicate fails");
    if (diagnosis.failures.size() != 1) return;
    const auto& failure = diagnosis.failures.front();
    expect(failure.predicate == predicate, prefix + ": predicate is " + std::string(predicate));
    expect(failure.track_ordinal == ordinal, prefix + ": track ordinal recorded");
    expect(failure.model_bone_name == model_name, prefix + ": actual model bone name recorded");
    expect(failure.index_out_of_range == out_of_range, prefix + ": out-of-range marker");
    expect(diagnosis.player_failing_track == ordinal, prefix + ": mirror predicts Player's first-failing track");
}

void test_positive_control() {
    const auto diagnosis = diagnose_consistently(model_data(), animation_data(), "matched pair");
    expect(diagnosis.player_accepts && diagnosis.failures.empty(), "matched pair has no failed predicate");
    expect(corpus::predicate_signature(diagnosis) == "none", "matched pair signature is none");
    expect(diagnosis.model_bone_count == 4 && diagnosis.track_count == 3, "counts recorded");
}

void test_each_compound_predicate_in_isolation() {
    const assets::Model model = model_data();
    {
        auto animation = animation_data(); animation.tracks[1].bone_index = 9;
        const auto diagnosis = diagnose_consistently(model, animation, "index out of range");
        expect_single(diagnosis, predicates::bone_index_in_range, 1, std::nullopt, true, "index out of range");
        expect(diagnosis.player_message == corpus::compound_track_message, "index out of range: compound message");
        expect(first(diagnosis).track_bone_name == std::optional<std::string>("arm"), "index out of range: track name kept");
    }
    {
        auto animation = animation_data(); animation.tracks[2].bone_name = "Hand";
        const auto diagnosis = diagnose_consistently(model, animation, "name mismatch");
        expect_single(diagnosis, predicates::bone_name_equal, 2, std::string("hand"), false, "name mismatch");
        expect(first(diagnosis).actual == "Hand" && first(diagnosis).expected == "hand",
            "name mismatch: case is not folded");
    }
    {
        // Names permuted across valid indices: both tracks fail by name.
        auto animation = animation_data();
        animation.tracks[1].bone_name = "hand"; animation.tracks[2].bone_name = "arm";
        const auto diagnosis = diagnose_consistently(model, animation, "permuted names");
        expect(diagnosis.failures.size() == 2 && diagnosis.failures[0].track_ordinal == 1U
            && diagnosis.failures[1].track_ordinal == 2U
            && diagnosis.failures[0].model_bone_name == std::optional<std::string>("arm"),
            "permuted names: both tracks reported with actual model names");
    }
    {
        auto animation = animation_data(); animation.tracks[0].samples.pop_back();
        const auto diagnosis = diagnose_consistently(model, animation, "sample count");
        expect_single(diagnosis, predicates::sample_count, 0, std::string("root"), false, "sample count");
        expect(first(diagnosis).actual == "2" && first(diagnosis).expected == "3",
            "sample count: actual and expected recorded");
    }
    {
        auto animation = animation_data(); animation.tracks.push_back(track(1, "arm"));
        const auto diagnosis = diagnose_consistently(model, animation, "duplicate index");
        expect_single(diagnosis, predicates::unique_bone_index, 3, std::string("arm"), false, "duplicate index");
    }
    const auto unsupported = static_cast<assets::Interpolation>(7);
    {
        auto animation = animation_data(); animation.tracks[0].translation_interpolation = unsupported;
        const auto diagnosis = diagnose_consistently(model, animation, "translation interpolation");
        expect_single(diagnosis, predicates::translation_interpolation, 0, std::string("root"), false, "translation interpolation");
        expect(first(diagnosis).actual == "unsupported(7)", "raw unsupported value recorded");
    }
    {
        auto animation = animation_data(); animation.tracks[1].scale_interpolation = unsupported;
        expect_single(diagnose_consistently(model, animation, "scale interpolation"),
            predicates::scale_interpolation, 1, std::string("arm"), false, "scale interpolation");
    }
    {
        auto animation = animation_data(); animation.tracks[2].rotation_interpolation = unsupported;
        expect_single(diagnose_consistently(model, animation, "rotation interpolation"),
            predicates::rotation_interpolation, 2, std::string("hand"), false, "rotation interpolation");
    }
    {
        auto animation = animation_data(); animation.tracks[2].visibility_interpolation = assets::Interpolation::linear;
        const auto diagnosis = diagnose_consistently(model, animation, "visibility interpolation");
        expect_single(diagnosis, predicates::visibility_interpolation, 2, std::string("hand"), false, "visibility interpolation");
        expect(first(diagnosis).actual == "linear", "visibility: actual mode recorded");
    }
    {
        auto animation = animation_data();
        animation.tracks[1].samples[1].rotation.w = std::numeric_limits<float>::quiet_NaN();
        const auto diagnosis = diagnose_consistently(model, animation, "non-finite sample");
        expect_single(diagnosis, predicates::samples_finite, 1, std::string("arm"), false, "non-finite sample");
        expect(diagnosis.player_message == corpus::non_finite_track_message, "non-finite uses its own Player message");
        expect(first(diagnosis).actual == "sample 1", "non-finite sample ordinal recorded");
    }
}

void test_all_failures_are_recorded_not_only_first() {
    auto animation = animation_data();
    animation.tracks[1].bone_name = "wrong"; animation.tracks[1].samples.pop_back();
    animation.tracks[2].bone_index = 40;
    animation.tracks[2].visibility_interpolation = assets::Interpolation::spherical;
    const auto diagnosis = diagnose_consistently(model_data(), animation, "compound multi-track");
    expect(diagnosis.player_failing_track == 1U && diagnosis.player_failing_bone == 1U,
        "mirror predicts Player's first failing track");
    std::vector<std::string> recorded;
    for (const auto& failure : diagnosis.failures)
        recorded.push_back(std::to_string(*failure.track_ordinal) + ":" + failure.predicate);
    const std::vector<std::string> expected{
        "1:" + std::string(predicates::bone_name_equal), "1:" + std::string(predicates::sample_count),
        "2:" + std::string(predicates::bone_index_in_range), "2:" + std::string(predicates::visibility_interpolation)};
    expect(recorded == expected, "every failed predicate across all tracks is recorded in Player order");
    expect(corpus::predicate_signature(diagnosis) == std::string(predicates::bone_index_in_range) + "+"
        + std::string(predicates::bone_name_equal) + "+" + std::string(predicates::sample_count) + "+"
        + std::string(predicates::visibility_interpolation), "signature is the sorted distinct predicate set");
}

void test_player_short_circuit_on_duplicates() {
    // Player only claims an index whose index/name/sample predicates passed.
    // A name failure on track 0 predicts that Player stops there; the diagnosis
    // still reports that track 1 reuses index 0.
    auto animation = animation_data();
    animation.tracks[0].bone_name = "nope";
    animation.tracks[1] = track(0, "root");
    const auto diagnosis = diagnose_consistently(model_data(), animation, "short-circuit duplicate");
    expect(diagnosis.player_failing_track == 0U, "short-circuit: mirror predicts Player's first failing track");
    expect(diagnosis.failures.size() == 2 && diagnosis.failures[1].predicate == predicates::unique_bone_index,
        "short-circuit: later duplicate still diagnosed");
}

void test_model_and_metadata_predicates() {
    {
        auto model = model_data(); model.bones[2].parent = 3;
        const auto diagnosis = diagnose_consistently(model, animation_data(), "hierarchy");
        expect(!diagnosis.player_accepts && diagnosis.player_code == playback::diagnostic_codes::invalid_model
            && !diagnosis.failures.empty() && first(diagnosis).predicate == predicates::model_hierarchy,
            "non-parent-first hierarchy is reported as a model predicate");
    }
    {
        auto model = model_data(); model.bones[1].relative_transform[3] = std::numeric_limits<float>::infinity();
        const auto diagnosis = diagnose_consistently(model, animation_data(), "bind finite");
        expect(!diagnosis.failures.empty() && first(diagnosis).predicate == predicates::model_bind_finite,
            "non-finite bind is reported as a model predicate");
    }
    {
        auto animation = animation_data(); animation.playable_frame_count = 3;
        const auto diagnosis = diagnose_consistently(model_data(), animation, "metadata");
        expect(!diagnosis.failures.empty() && first(diagnosis).predicate == predicates::duration_metadata,
            "invalid duration metadata is reported");
    }
    {
        auto animation = animation_data(); animation.duration_seconds = 2.5F;
        const auto diagnosis = diagnose_consistently(model_data(), animation, "duration");
        expect(diagnosis.failures.size() == 1 && first(diagnosis).predicate == predicates::duration_consistent,
            "duration mismatch is reported");
    }
}

void test_r0_prefix_rule() {
    const std::vector<std::string> paths{
        "data/art/models/tank.alo", "data/art/models/tank_heavy.alo", "data/art/models/tanker.alo",
        "data/art/models/other/tank.alo", "data/art/models/other2/tank_heavy_die.alo"};
    const std::vector<std::pair<std::string, std::optional<std::string>>> queries{
        {"data/art/models/tank_heavy_idle_00.ala", "data/art/models/tank_heavy.alo"},
        {"data/art/models/tank_idle_00.ala", "data/art/models/tank.alo"},
        {"data/art/models/tanker_idle_00.ala", "data/art/models/tanker.alo"},
        {"data/art/models/tankheavy_idle_00.ala", std::nullopt},
        {"data/art/models/tank.ala", std::nullopt},
        {"data/art/models/tank_.ala", "data/art/models/tank.alo"},
        {"data/art/models/other/tank_heavy_die_00.ala", "data/art/models/other/tank.alo"},
        {"data/art/models/other3/tank_heavy_die_00.ala", std::nullopt},
        {"data/art/models/sub/tank_idle_00.ala", std::nullopt},
        {"data/art/tank_idle_00.ala", std::nullopt},
    };
    const auto evaluate = [&queries](const std::vector<std::string>& order) {
        corpus::ModelIndex index;
        for (const auto& path : order) corpus::add_model(index, path);
        std::vector<std::optional<std::string>> results;
        for (const auto& [query, expected] : queries) {
            (void)expected;
            results.push_back(corpus::matching_model(query, index));
            for (const auto& candidate : corpus::r0_qualifying_models(query, index)) results.push_back(candidate);
            results.push_back(std::string("|"));
        }
        return results;
    };
    corpus::ModelIndex index;
    for (const auto& path : paths) corpus::add_model(index, path);
    for (const auto& [query, expected] : queries)
        expect(corpus::matching_model(query, index) == expected, "R0 selection for " + query);
    expect(corpus::r0_qualifying_models("data/art/models/tank_heavy_idle_00.ala", index)
            == std::vector<std::string>{"data/art/models/tank_heavy.alo", "data/art/models/tank.alo"},
        "R0 lists every qualifying candidate, longest first");
    expect(corpus::r0_qualifying_models("data/art/models/tanker_idle_00.ala", index)
            == std::vector<std::string>{"data/art/models/tanker.alo"},
        "R0 underscore boundary excludes tank for tanker clip");
    expect(corpus::r0_qualifying_models("data/art/models/other3/tank_heavy_die_00.ala", index).empty(),
        "R0 never crosses into a second directory");

    const auto baseline = evaluate(paths);
    std::vector<std::string> order = paths;
    std::reverse(order.begin(), order.end());
    expect(evaluate(order) == baseline, "R0 is independent of reversed enumeration");
    std::mt19937 generator(0x24U);
    for (int trial = 0; trial < 64; ++trial) {
        std::shuffle(order.begin(), order.end(), generator);
        expect(evaluate(order) == baseline, "R0 is independent of shuffled enumeration");
    }
}

// JSON string-syntax decoder for valid UTF-8 inputs: rejects raw control bytes,
// unknown escapes and malformed \u sequences; decodes \u00XX (the only form
// json_string emits for code points) back to one byte.  It does not validate
// UTF-8, so non-UTF-8 cases exercise byte preservation only.  Returns nullopt
// on any syntax violation.
std::optional<std::string> strict_json_decode(const std::string_view text) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') return std::nullopt;
    std::string decoded;
    for (std::size_t index = 1; index + 1 < text.size(); ++index) {
        const char character = text[index];
        if (static_cast<unsigned char>(character) < 0x20U || character == '"') return std::nullopt;
        if (character != '\\') { decoded.push_back(character); continue; }
        if (++index + 1 >= text.size()) return std::nullopt;
        switch (text[index]) {
        case '"': decoded.push_back('"'); break;
        case '\\': decoded.push_back('\\'); break;
        case '/': decoded.push_back('/'); break;
        case 'b': decoded.push_back('\b'); break;
        case 'f': decoded.push_back('\f'); break;
        case 'n': decoded.push_back('\n'); break;
        case 'r': decoded.push_back('\r'); break;
        case 't': decoded.push_back('\t'); break;
        case 'u': {
            if (index + 4 >= text.size() - 1) return std::nullopt; // four digits before the closing quote
            unsigned value{};
            for (std::size_t digit = 1; digit <= 4; ++digit) {
                const char hex = text[index + digit];
                value <<= 4U;
                if (hex >= '0' && hex <= '9') value |= static_cast<unsigned>(hex - '0');
                else if (hex >= 'a' && hex <= 'f') value |= static_cast<unsigned>(hex - 'a' + 10);
                else if (hex >= 'A' && hex <= 'F') value |= static_cast<unsigned>(hex - 'A' + 10);
                else return std::nullopt;
            }
            if (value > 0x7FU) return std::nullopt; // not emitted; out of this decoder's scope
            decoded.push_back(static_cast<char>(value));
            index += 4;
            break;
        }
        default: return std::nullopt;
        }
    }
    return decoded;
}

// The pre-fix helper, kept only to prove that strings without control bytes
// (every string in the frozen v1 ledger) serialize to identical bytes.  Valid
// UTF-8 is required for strict JSON claims; raw non-UTF-8 cases below only
// check byte preservation.
std::string legacy_json(const std::string_view value) {
    std::string result{"\""};
    for (const char character : value) {
        if (character == '\"' || character == '\\') result.push_back('\\');
        if (character == '\n') result.append("\\n");
        else if (character != '\r') result.push_back(character);
    }
    result.push_back('\"');
    return result;
}

void test_json_string_escaping() {
    // Exact encoding of every control byte U+0000..U+001F.
    const std::string_view expected_controls[0x20] = {
        "\\u0000", "\\u0001", "\\u0002", "\\u0003", "\\u0004", "\\u0005", "\\u0006", "\\u0007",
        "\\b", "\\t", "\\n", "\\u000b", "\\f", "\\r", "\\u000e", "\\u000f",
        "\\u0010", "\\u0011", "\\u0012", "\\u0013", "\\u0014", "\\u0015", "\\u0016", "\\u0017",
        "\\u0018", "\\u0019", "\\u001a", "\\u001b", "\\u001c", "\\u001d", "\\u001e", "\\u001f"};
    std::string every_control;
    for (unsigned byte = 0; byte < 0x20U; ++byte) {
        const std::string raw = "synthetic" + std::string(1, static_cast<char>(byte)) + "name";
        const std::string encoded = corpus::json_string(raw);
        const std::string label = "json control " + std::string(expected_controls[byte]);
        expect(encoded == "\"synthetic" + std::string(expected_controls[byte]) + "name\"", label + ": exact escape");
        expect(strict_json_decode(encoded) == raw, label + ": valid-UTF-8 strict lossless round trip");
        every_control.push_back(static_cast<char>(byte));
    }
    expect(corpus::json_string("synthetic\tname") == "\"synthetic\\tname\"", "TAB is escaped, not emitted raw");
    expect(corpus::json_string("synthetic\rname") == "\"synthetic\\rname\"", "CR is escaped, not dropped");
    expect(corpus::json_string("a\r\nb") == "\"a\\r\\nb\"", "CRLF keeps both bytes");
    const std::string all = corpus::json_string(every_control);
    expect(std::none_of(all.begin(), all.end(), [](const char c) { return static_cast<unsigned char>(c) < 0x20U; }),
        "no raw control byte in output");
    expect(strict_json_decode(all) == every_control, "all 32 controls round-trip as strict JSON");
    // NUL inside a string_view is data, not a terminator.
    const std::string embedded_nul("a\0b", 3);
    expect(corpus::json_string(embedded_nul) == "\"a\\u0000b\"", "embedded NUL escaped");

    // Unchanged byte behavior for everything else, including bytes >= 0x80.
    // Valid UTF-8 values exercise strict JSON round trips; raw 0xFF is only a
    // non-UTF-8 byte-preservation case.
    const std::vector<std::string> ordinary = {"", "data/art/models/ei_vader.alo", "IK Chain01",
        "quote\" and back\\slash", "slash/stays", "del\x7F" "byte", "\xC3\x85ngstr\xC3\xB6m",
        "\xE2\x9C\x93 check", "raw\xFFhigh", "Pi_Damage_Elec_Mid_Dense"};
    for (const std::string& value : ordinary) {
        expect(corpus::json_string(value) == legacy_json(value), "control-free string bytes unchanged: " + value);
        expect(strict_json_decode(corpus::json_string(value)) == value,
            "control-free bytes preserved (strict only for valid UTF-8): " + value);
    }
    // The decoder's JSON-string syntax is strict, but it does not validate
    // UTF-8; it must reject what the old helper produced.
    expect(!strict_json_decode(legacy_json("synthetic\tname")), "decoder rejects raw TAB");
    expect(strict_json_decode(legacy_json("synthetic\rname")) != std::string("synthetic\rname"),
        "legacy helper lost CR");
}
} // namespace

int main() {
    test_json_string_escaping();
    test_positive_control();
    test_each_compound_predicate_in_isolation();
    test_all_failures_are_recorded_not_only_first();
    test_player_short_circuit_on_duplicates();
    test_model_and_metadata_predicates();
    test_r0_prefix_rule();
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "animation corpus diagnostics contracts passed\n";
    return EXIT_SUCCESS;
}
