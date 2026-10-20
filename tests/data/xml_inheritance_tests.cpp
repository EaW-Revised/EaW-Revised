#include "data_test_support.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/data/xml.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::tests::data_contracts {
void inheritance_contracts(const eawr::data::Catalog& catalog, const eawr::core::Result<eawr::vfs::Vfs>& mounted,
                           const eawr::data::LoadOptions& options) {
    auto effective = catalog.resolve("TOP");
    expect(static_cast<bool>(effective), "three-level inheritance resolves base-first");
    if (effective) {
        expect(effective.value().chain == std::vector<std::string>({"TOP", "MID", "BASE"}),
               "public chain is complete and derived-to-base");
        const auto* replaced = effective.value().value("ReplaceTag");
        expect(replaced && replaced->value.raw_text == "top" && replaced->displaced_value &&
                   replaced->displaced_value->raw_text == "mid" &&
                   replaced->provenance == eawr::data::ValueProvenance::overridden,
               "replace retains winning and immediately displaced provenance");
        const auto* merged = effective.value().value("MergeTag");
        expect(merged && merged->value.raw_text == "A, A, B, C" &&
                   merged->provenance == eawr::data::ValueProvenance::merged,
               "merge appends ordered tokens and keeps repeats");
        std::vector<const eawr::data::EffectiveValue*> tuples;
        for (const auto& value : effective.value().values) {
            if (value.value.name == "Tuple") tuples.push_back(&value);
        }
        struct TupleExpectation {
            const char* raw_text;
            const char* source_object_id;
            eawr::data::ValueProvenance provenance;
            const char* logical_path;
            std::uint64_t line;
        };
        const auto tuple_expectations = std::array<TupleExpectation, 4>{{
            {"one, 1", "BASE", eawr::data::ValueProvenance::inherited, "data/xml/objects-a.xml", 7},
            {"two, 2", "BASE", eawr::data::ValueProvenance::inherited, "data/xml/objects-a.xml", 7},
            {"one, 1", "BASE", eawr::data::ValueProvenance::inherited, "data/xml/objects-a.xml", 7},
            {"three, 3", "MID", eawr::data::ValueProvenance::inherited, "data/xml/objects-a.xml", 15},
        }};
        expect(tuples.size() == tuple_expectations.size(),
               "multiple merge tuples remain independent and ordered (exact count 4)");
        if (tuples.size() == tuple_expectations.size()) {
            bool tuples_ok = true;
            std::string detail;
            for (std::size_t i = 0; i < tuples.size(); ++i) {
                const auto& actual = *tuples[i];
                const auto& expected = tuple_expectations[i];
                const auto ok = actual.value.raw_text == expected.raw_text &&
                                actual.source_object_id == expected.source_object_id &&
                                actual.provenance == expected.provenance &&
                                actual.value.source.logical_path == expected.logical_path &&
                                actual.value.source.line == expected.line;
                if (!ok) {
                    tuples_ok = false;
                    detail += " tuple[" + std::to_string(i) + "] raw=" + actual.value.raw_text +
                              " src=" + actual.source_object_id +
                              " line=" + std::to_string(actual.value.source.line) +
                              " path=" + actual.value.source.logical_path +
                              " (expected raw=" + expected.raw_text + " src=" + expected.source_object_id +
                              " line=" + std::to_string(expected.line) + ")";
                }
            }
            expect(tuples_ok,
                   "merge tuples keep exact ordered EffectiveValue occurrences with provenance/source" + detail);
        }
        expect(effective.value().value("IgnoredNew") == nullptr,
               "ignored mode does not add a derived-only tag");
        const auto* empty = effective.value().value("Empty");
        expect(empty && empty->value.raw_text.empty(), "explicit empty derived value overrides the base");
        // ML-24 (#3631): single() skips blank and TBD occurrences, whichever layer they sit in.
        const auto select = effective.value().single("SFXEvent_Select");
        expect(select && select->value.raw_text == "Unit_Select_Base" && select->source_object_id == "BASE"
                   && select->provenance == eawr::data::ValueProvenance::inherited,
               "single: a TBD then blank variant value inherits the real base value with its source");
        const auto empty_single = effective.value().single("Empty");
        expect(empty_single && empty_single->value.raw_text == "base", "single: a blank variant value inherits the base");
        expect(!effective.value().single("SFXEvent_Lone") && !effective.value().single("Absent_Tag"),
               "single: a lone placeholder and an absent tag have no value");
        const auto own = effective.value().single("ReplaceTag");
        expect(own && own->value.raw_text == "top", "single: a real variant value wins");
        {
            // The trace marks the variant's placeholder element and the inherited real element it falls back to.
            std::vector<std::uint64_t> select_lines;
            for (const auto& item : effective.value().values)
                if (item.value.name == "SFXEvent_Select") select_lines.push_back(item.value.source.line);
            eawr::data::tag_trace::Recording recording;
            const auto traced = effective.value().single("SFXEvent_Select");
            const auto lone = effective.value().single("SFXEvent_Lone");
            const auto entries = recording.finish();
            const auto marked = [&](const std::uint64_t line, const std::string_view element) {
                return std::any_of(entries.begin(), entries.end(), [&](const eawr::data::tag_trace::Entry& entry) {
                    return entry.kind == eawr::data::tag_trace::Kind::used && entry.element == element && entry.line == line;
                });
            };
            expect(traced && !select_lines.empty() && marked(select_lines.back(), "SFXEvent_Select")
                       && marked(traced->value.source.line, "SFXEvent_Select") && traced->value.source.line != select_lines.back(),
                   "single: the trace marks the variant's blank element and the inherited element it reads");
            expect(!lone && std::any_of(entries.begin(), entries.end(), [](const eawr::data::tag_trace::Entry& entry) {
                       return entry.element == "SFXEvent_Lone";
                   }),
                   "single: a lone placeholder is still marked as read");
        }
        // The battle-audio tag() reader is single_text(): trimmed, inherited over blank/TBD, lowercase tbd is a value.
        expect(effective.value().single_text("SFXEvent_Select") == "Unit_Select_Base"
                   && effective.value().single_text("SFXEvent_Lone").empty()
                   && effective.value().single_text("Absent_Tag").empty()
                   && effective.value().single_text("SFXEvent_Lower") == "tbd",
               "single_text: placeholders inherit the real base text; a lone placeholder is empty; lowercase tbd stays a value");
        const auto* ignored = effective.value().value("Ignored");
        expect(ignored && ignored->value.raw_text == "base-only", "ignored mode refuses derived writes");
        const auto* nested = effective.value().value("Nested");
        expect(nested && nested->value.attributes[0].value == "top" &&
                   nested->value.children[0].children[0].raw_text == "top",
               "nested tags and attributes stay attached to the winning occurrence");
        const auto* death = effective.value().value("Death_Clone");
        expect(death && death->value.raw_text == "Damage_Fire, Mid_Clone",
               "pinned repeatable Death_Clone occurrences are independent, not token-unioned");
        expect(effective.value().catalog_generation == catalog.generation(),
               "effective result is scoped to catalog generation");
    }

    auto second_resolve = catalog.resolve("top");
    expect(second_resolve && effective && second_resolve.value().catalog_generation == effective.value().catalog_generation,
           "generation-scoped cache returns the same effective generation");
    auto second_load = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::eaw, options);
    expect(second_load && second_load.value().catalog.generation() != catalog.generation(),
           "new catalog has a distinct cache generation");

    auto plain = catalog.resolve("BASE");
    const auto* plain_death = plain ? plain.value().value("Death_Clone") : nullptr;
    expect(plain_death && plain_death->provenance == eawr::data::ValueProvenance::own,
           "repeatable merge occurrences on a non-variant object retain own provenance");

    auto missing = catalog.resolve("MISSING");
    expect(!missing && missing.error().code == eawr::data::diagnostic_codes::variant_missing_base &&
               missing.error().message.find("MISSING -> NO_BASE") != std::string::npos,
           "missing base diagnostic contains the full attempted chain");
    auto cycle = catalog.resolve("CYCLE_A");
    expect(!cycle && cycle.error().code == eawr::data::diagnostic_codes::variant_cycle &&
               cycle.error().message.find("CYCLE_A -> CYCLE_B -> CYCLE_A") != std::string::npos,
           "cycle diagnostic contains the complete repeated chain");
}
} // namespace eawr::tests::data_contracts
