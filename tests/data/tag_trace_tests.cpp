#include "data_test_support.hpp"

#include "eawr/data/tag_trace.hpp"
#include "eawr/data/xml.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// #628: the load-time tag trace (docs/tag-coverage.md).
namespace eawr::tests::data_contracts {
namespace {

namespace trace = eawr::data::tag_trace;

struct TraceTree {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("eawr-tag-trace-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TraceTree() { std::filesystem::create_directories(root); }
    ~TraceTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

void write_file(const std::filesystem::path& path, const std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

// The effective values of an object as text, to compare loads with the trace on and off.
std::vector<std::string> values_of(const eawr::data::EffectiveObject& object) {
    std::vector<std::string> result;
    for (const auto& value : object.values) {
        result.push_back(value.value.name + "=" + value.value.raw_text + "@" + std::to_string(value.value.source.line));
    }
    return result;
}

bool has_entry(const std::vector<trace::Entry>& entries, const trace::Kind kind, const std::string_view element,
               const std::uint64_t line, const std::string_view name = {}) {
    return std::any_of(entries.begin(), entries.end(), [&](const trace::Entry& entry) {
        return entry.kind == kind && entry.element == element && entry.line == line && entry.name == name;
    });
}

} // namespace

void tag_trace_contracts() {
    TraceTree tree;
    write_file(tree.root / "XML" / "GameObjectFiles.xml",
               "<Game_Object_Files><File>units.xml</File></Game_Object_Files>");
    write_file(tree.root / "XML" / "units.xml", "<Units>\n"
               "<SpaceUnit Name=\"Base_Ship\">\n"
               "  <Max_Speed>2.0</Max_Speed>\n"
               "  <Unread_Tag>1</Unread_Tag>\n"
               "</SpaceUnit>\n"
               "<SpaceUnit Name=\"Derived_Ship\">\n"
               "  <Variant_Of_Existing_Type>Base_Ship</Variant_Of_Existing_Type>\n"
               "  <Tactical_Health>100</Tactical_Health>\n"
               "</SpaceUnit>\n"
               "</Units>\n");
    write_file(tree.root / "XML" / "GameConstants.xml",
               "<GameConstants>\n  <Read_Constant>1</Read_Constant>\n  <Other_Constant>2</Other_Constant>\n"
               "</GameConstants>\n");
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "tag trace: fixture VFS mounts");
    if (!mounted) return;

    expect(!trace::enabled(), "tag trace: off by default");
    auto off = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(off), "tag trace: fixture catalog loads");
    if (!off) return;
    auto untraced = off.value().catalog.resolve("Derived_Ship");
    expect(static_cast<bool>(untraced), "tag trace: fixture object resolves");
    if (!untraced) return;
    (void)untraced.value().value("Max_Speed");

    std::vector<trace::Entry> entries;
    std::vector<std::string> traced_values;
    std::size_t traced_diagnostics = 0;
    {
        trace::Recording recording;
        expect(trace::enabled(), "tag trace: a recording turns the trace on");
        auto on = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
        expect(static_cast<bool>(on), "tag trace: the catalog loads while tracing");
        if (!on) return;
        traced_diagnostics = on.value().diagnostics.size();
        auto object = on.value().catalog.resolve("Derived_Ship");
        expect(static_cast<bool>(object), "tag trace: the object resolves while tracing");
        if (!object) return;
        traced_values = values_of(object.value());
        expect(object.value().value("Max_Speed") != nullptr, "tag trace: an inherited value is found");
        expect(object.value().value("Absent_Tag") == nullptr, "tag trace: an absent value is not found");
        {
            const trace::Unrecorded unrecorded;
            (void)object.value().value("Unread_Tag");
        }
        auto constants = eawr::data::load_document(mounted.value(), "data/xml/gameconstants.xml");
        expect(static_cast<bool>(constants), "tag trace: the constants document loads");
        if (!constants) return;
        trace::document(constants.value().root);
        entries = recording.finish();
        expect(!trace::enabled(), "tag trace: finish turns the trace off");
        (void)object.value().value("Tactical_Health");
    }
    expect(!trace::enabled(), "tag trace: off after the recording ends");

    expect(values_of(untraced.value()) == traced_values && off.value().diagnostics.size() == traced_diagnostics,
           "tag trace: the loaded values and diagnostics are the same with the trace on and off");
    expect(std::is_sorted(entries.begin(), entries.end()) &&
               std::adjacent_find(entries.begin(), entries.end()) == entries.end(),
           "tag trace: entries are sorted and unique");
    expect(has_entry(entries, trace::Kind::object, "SpaceUnit", 6, "Derived_Ship") &&
               has_entry(entries, trace::Kind::object, "SpaceUnit", 2, "Derived_Ship"),
           "tag trace: every definition of the variant chain is recorded as touched");
    expect(has_entry(entries, trace::Kind::used, "Variant_Of_Existing_Type", 7),
           "tag trace: resolving reads the variant link");
    expect(has_entry(entries, trace::Kind::used, "Max_Speed", 3),
           "tag trace: a value read is recorded at the definition that authors it");
    expect(!has_entry(entries, trace::Kind::used, "Unread_Tag", 4) &&
               !has_entry(entries, trace::Kind::used, "Tactical_Health", 8),
           "tag trace: values nobody asked for, asked for unrecorded or after finish, are not recorded");
    expect(has_entry(entries, trace::Kind::document, "GameConstants", 1),
           "tag trace: a document a loader uses whole is recorded by its root");
    const auto max_speed = std::find_if(entries.begin(), entries.end(),
        [](const trace::Entry& entry) { return entry.element == "Max_Speed"; });
    expect(max_speed != entries.end() && max_speed->logical_path == "data/xml/units.xml" && max_speed->column == 4,
           "tag trace: an entry carries the node's logical path, line and column");

    {
        trace::Recording recording;
        auto indexed = [&] {
            const trace::Unrecorded index_resolve;
            return off.value().catalog.resolve("Derived_Ship");
        }();
        expect(static_cast<bool>(indexed), "tag trace: a cached index resolve succeeds while unrecorded");
        if (!indexed) return;
        (void)indexed.value().value("Max_Speed");
        const auto index_entries = recording.finish();
        expect(!has_entry(index_entries, trace::Kind::object, "SpaceUnit", 6, "Derived_Ship") &&
                   !has_entry(index_entries, trace::Kind::object, "SpaceUnit", 2, "Derived_Ship") &&
                   !has_entry(index_entries, trace::Kind::used, "Variant_Of_Existing_Type", 7),
               "tag trace: a cached index resolve does not add whole variant roots or links");
        expect(has_entry(index_entries, trace::Kind::used, "Max_Speed", 3),
               "tag trace: value reads after the index resolve scope remain recorded");
    }
    {
        trace::Recording recording;
        auto ordinary = off.value().catalog.resolve("Derived_Ship");
        expect(static_cast<bool>(ordinary), "tag trace: an ordinary cached resolve succeeds after the index scope");
        if (!ordinary) return;
        (void)ordinary.value().value("Tactical_Health");
        const auto ordinary_entries = recording.finish();
        expect(has_entry(ordinary_entries, trace::Kind::object, "SpaceUnit", 6, "Derived_Ship") &&
                   has_entry(ordinary_entries, trace::Kind::object, "SpaceUnit", 2, "Derived_Ship") &&
                   has_entry(ordinary_entries, trace::Kind::used, "Variant_Of_Existing_Type", 7) &&
                   has_entry(ordinary_entries, trace::Kind::used, "Tactical_Health", 8),
               "tag trace: an ordinary cached resolve still records the full chain and subsequent reads");
    }

    const std::vector<trace::Entry> escaped{
        {trace::Kind::attribute, "data/xml/a\"b.xml", "base", 1, 2, "Unit", std::string("N\\a\tme\x01")}};
    const auto json = trace::to_json(escaped);
    expect(json == "{\"schema_version\":1,\"entries\":[\n"
                   "{\"kind\":\"attribute\",\"logical_path\":\"data/xml/a\\\"b.xml\",\"source_id\":\"base\","
                   "\"line\":1,\"column\":2,\"element\":\"Unit\",\"name\":\"N\\\\a\\tme\\u0001\"}\n]}\n",
           "tag trace: the JSON writer escapes strings");
    trace::Recording empty;
    auto untouched = eawr::data::load_document(mounted.value(), "data/xml/gameconstants.xml");
    expect(static_cast<bool>(untouched) && empty.finish().empty(),
           "tag trace: a new recording starts empty, and loading a document alone touches nothing");
}

} // namespace eawr::tests::data_contracts
