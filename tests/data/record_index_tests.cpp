#include "eawr/data/record_index.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../support/temp_dir.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;
using namespace eawr;
int failures = 0;
void expect(bool condition, const char* message) { if (!condition) { std::cerr << message << '\n'; ++failures; } }
void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out.write(text.data(), static_cast<std::streamsize>(text.size()));
}
const data::RecordTagRow* tag(const std::vector<data::RecordTagRow>& rows, std::string_view name) {
    for (const auto& r : rows) if (r.tag == name) return &r;
    return nullptr;
}
int main() {
    const auto root = fs::temp_directory_path() / ("eawr-index-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto base = root / "base", mod = root / "mod", middle = root / "middle";
    write(base / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>objects.xml</File><File>removed.xml</File></Game_Object_Files>");
    write(mod / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>objects.xml</File></Game_Object_Files>");
    write(base / "XML/objects.xml", "<Objects><SpaceUnit Name=\"A\"><Old>old</Old><Keep>v</Keep></SpaceUnit><SpaceUnit Name=\"Base\"><Inherited>base</Inherited><MergeTag>A</MergeTag></SpaceUnit></Objects>");
    write(base / "XML/removed.xml", "<Objects><SpaceUnit Name=\"Removed\"><Old>x</Old></SpaceUnit></Objects>");
    write(mod / "XML/objects.xml", "<Objects><SpaceUnit Name=\"Base\"><Inherited>base</Inherited><MergeTag>A</MergeTag></SpaceUnit><SpaceUnit Name=\"A\"><Variant_Of_Existing_Type>Base</Variant_Of_Existing_Type><MergeTag>B</MergeTag><Empty/><Nested><Child>value</Child></Nested></SpaceUnit><SpaceUnit Name=\"A\"><Old>discard</Old></SpaceUnit></Objects>");
    write(middle / "XML/objects.xml", "<Objects><SpaceUnit");
    write(base / "XML/constants.xml", "<Constants><Alpha>old</Alpha><Removed>x</Removed></Constants>");
    write(mod / "XML/constants.xml", "<Constants><Alpha>new</Alpha><Empty/></Constants>");
    write(base / "XML/bad.xml", "<Constants><Alpha>ok</Alpha></Constants>");
    write(mod / "XML/bad.xml", "<Constants><Alpha>");
    const std::array vanilla_mounts{vfs::MountSpec{"base", base, "data", {}}};
    const std::array profile_mounts{vfs::MountSpec{"mod", mod, "data", {}}, vfs::MountSpec{"middle", middle, "data", {}}, vfs::MountSpec{"base", base, "data", {}}};
    auto vanilla = vfs::Vfs::mount(vanilla_mounts); auto profile = vfs::Vfs::mount(profile_mounts);
    expect(vanilla && profile, "mounts");
    if (vanilla && profile) {
        data::LoadOptions options; options.parse_unregistered_xml = false;
        options.merge_rules.push_back({"GameObjectType", "MergeTag", data::MergeMode::merge, false});
        auto v = data::load_catalog(vanilla.value(), data::Profile::foc, options);
        auto p = data::load_catalog(profile.value(), data::Profile::foc, options);
        expect(v && p, "catalogs");
        if (v && p) {
            auto shadow = data::load_shadowed_definitions(profile.value(), p.value().catalog, options);
            expect(static_cast<bool>(shadow), "shadow pass");
            if (shadow) {
                expect(!shadow.value().diagnostics.empty(), "malformed shadow candidate diagnosed");
                auto index = data::build_record_index(v.value().catalog, p.value().catalog, shadow.value(), profile.value());
                expect(static_cast<bool>(index), "index");
                if (index) {
                    expect(index.value().records.size() == 3, "three records");
                    expect(index.value().registry_dropped_vanilla_files.size() == 1, "one registry dropped file");
                    const auto* a = index.value().find("a", data::Category::game_object);
                    expect(a && a->winner && a->winner->definition_order == 1 && a->definitions.size() == 3, "duplicate winner and shadow retained");
                    if (a && a->definitions.size() == 3) {
                        expect(a->definitions[0].candidate_rank == 2 && a->definitions[1].candidate_rank == 0 &&
                            a->definitions[2].candidate_rank == 0, "lowest priority first with actual VFS ranks");
                        const auto effective = index.value().effective(*a);
                        expect(effective && effective.value().chain.size() == 2, "variant chain");
                        const auto table = index.value().tags(*a);
                        expect(static_cast<bool>(table), "tag table");
                        if (!table) return 1;
                        const auto& rows = table.value();
                        const auto* old = tag(rows, "Old"); const auto* inherited = tag(rows, "Inherited");
                        const auto* merged = tag(rows, "MergeTag"); const auto* empty = tag(rows, "Empty"); const auto* nested = tag(rows, "Nested");
                        expect(old && old->how == data::RecordTagHow::dropped && old->effective.empty(), "dropped");
                        expect(inherited && inherited->how == data::RecordTagHow::inherited, "inherited");
                        expect(merged && merged->how == data::RecordTagHow::merged && merged->effective.front().value.raw_text == "A, B", "merged");
                        expect(empty && !empty->effective.empty() && empty->effective.front().value.raw_text.empty(), "empty occurrence");
                        expect(nested && !nested->effective.empty() && nested->effective.front().value.children.size() == 1, "nested occurrence");
                        expect(a->definitions.front().digest.size() == 64, "definition digest");
                    }
                }
            }
        }
        // The inputs and original index die before the copied index is used.
        auto retained = [&]() -> data::RecordIndex {
            auto local_v = data::load_catalog(vanilla.value(), data::Profile::foc, options);
            auto local_p = data::load_catalog(profile.value(), data::Profile::foc, options);
            auto local_shadow = data::load_shadowed_definitions(profile.value(), local_p.value().catalog, options);
            auto built = data::build_record_index(local_v.value().catalog, local_p.value().catalog,
                local_shadow.value(), profile.value());
            data::RecordIndex copy = built.value();
            return copy;
        }();
        const auto* retained_a = retained.find("A", data::Category::game_object);
        expect(retained_a && retained_a->winner && retained_a->winner->id == "A" &&
            retained_a->definitions.size() == 3, "copied index retains definition storage");
        if (retained_a) {
            const auto retained_tags = retained.tags(*retained_a);
            const auto retained_effective = retained.effective(*retained_a);
            expect(retained_tags && tag(retained_tags.value(), "Nested") && retained_effective &&
                retained_effective.value().chain.size() == 2, "copied index resolves after inputs are destroyed");
        }
        auto escaped = [&]() {
            auto local_v = data::load_catalog(vanilla.value(), data::Profile::foc, options);
            auto local_p = data::load_catalog(profile.value(), data::Profile::foc, options);
            auto local_shadow = data::load_shadowed_definitions(profile.value(), local_p.value().catalog, options);
            auto built = data::build_record_index(local_v.value().catalog, local_p.value().catalog,
                local_shadow.value(), profile.value());
            auto entry = *built.value().find("A", data::Category::game_object);
            auto definition = entry.definitions.front();
            return std::pair{std::move(entry), std::move(definition)};
        }();
        expect(escaped.first.winner && escaped.first.winner->id == "A" &&
            escaped.first.definitions.front().definition->id == "A" && escaped.second.definition->id == "A",
            "escaped entry and definition retain their stores");
        escaped.first = {};
        expect(escaped.second.definition && escaped.second.definition->id == "A",
            "escaped definition retains its store after the entry is destroyed");
        auto whole = data::compare_whole_file(vanilla.value(), profile.value(), "data/xml/constants.xml");
        expect(whole && whole.value().candidates.size() == 2 && whole.value().tags.size() == 3, "whole file candidates and tags");
        if (whole) {
            const auto* alpha = tag(whole.value().tags, "Alpha"); const auto* removed = tag(whole.value().tags, "Removed");
            expect(alpha && alpha->effective.front().value.raw_text == "new" && alpha->vanilla.front().value.raw_text == "old", "whole file winner");
            expect(removed && removed->how == data::RecordTagHow::dropped, "whole file dropped");
        }
        const auto bad = data::compare_whole_file(vanilla.value(), profile.value(), "data/xml/bad.xml");
        expect(bad && bad.value().candidates.size() == 2 && bad.value().candidates[0].error &&
            bad.value().candidates[1].document && bad.value().candidates[0].winner &&
            !bad.value().candidates[1].winner, "malformed VFS winner is retained without promoting later candidates");
        if (v && p) {
            data::ShadowedDefinitions missing;
            data::Definition ghost;
            ghost.id = "A"; ghost.root.source.logical_path = "data/xml/absent.xml";
            missing.definitions.push_back(ghost);
            missing.diagnostics.push_back({"synthetic", core::Severity::warning, "test", std::nullopt, std::nullopt, std::nullopt, std::nullopt});
            const auto index = data::build_record_index(v.value().catalog, p.value().catalog, missing, profile.value());
            const auto* entry = index ? index.value().find("A", data::Category::game_object) : nullptr;
            expect(entry && entry->definitions.size() == 3 && entry->definitions[0].candidate_rank == 0 &&
                entry->definitions[1].candidate_rank == 0 && !entry->definitions.back().candidate_rank &&
                index.value().diagnostics.size() == 2, "unranked candidate sorts last and errors are diagnosed");
        }
    }
    {
        // Variant cycles (self, 2-cycle, a child of one) and a long non-cyclic chain: the index builds, only the
        // cyclic objects fail to resolve, and the failure is the cycle diagnostic that names the chain.
        const auto loops = root / "loops";
        write(loops / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>objects.xml</File></Game_Object_Files>");
        std::string xml = "<Objects><SpaceUnit Name=\"Self\"><Variant_Of_Existing_Type>Self</Variant_Of_Existing_Type></SpaceUnit>"
            "<SpaceUnit Name=\"P\"><Variant_Of_Existing_Type>Q</Variant_Of_Existing_Type></SpaceUnit>"
            "<SpaceUnit Name=\"Q\"><Variant_Of_Existing_Type>P</Variant_Of_Existing_Type></SpaceUnit>"
            "<SpaceUnit Name=\"Kid\"><Variant_Of_Existing_Type>Self</Variant_Of_Existing_Type></SpaceUnit>"
            "<SpaceUnit Name=\"C0\"><Speed>1</Speed></SpaceUnit>";
        for (int i = 1; i <= 300; ++i)
            xml += "<SpaceUnit Name=\"C" + std::to_string(i) + "\"><Variant_Of_Existing_Type>C" + std::to_string(i - 1) + "</Variant_Of_Existing_Type></SpaceUnit>";
        write(loops / "XML/objects.xml", xml + "</Objects>");
        const std::array mounts{vfs::MountSpec{"loops", loops, "data", {}}};
        auto vfs = vfs::Vfs::mount(mounts);
        expect(static_cast<bool>(vfs), "loops mount");
        if (vfs) {
            data::LoadOptions options; options.parse_unregistered_xml = false;
            auto loaded = data::load_catalog(vfs.value(), data::Profile::foc, options);
            expect(static_cast<bool>(loaded), "loops catalog");
            if (loaded) {
                auto shadow = data::load_shadowed_definitions(vfs.value(), loaded.value().catalog, options);
                auto index = data::build_record_index(loaded.value().catalog, loaded.value().catalog, shadow.value(), vfs.value());
                expect(index && index.value().records.size() == 305, "cycles do not stop the index");
                if (index) {
                    for (const char* name : {"Self", "P", "Q", "Kid"}) {
                        const auto* entry = index.value().find(name, data::Category::game_object);
                        expect(entry != nullptr, "cyclic record is indexed");
                        if (!entry) continue;
                        const auto tags = index.value().tags(*entry);
                        expect(!tags && tags.error().code == data::diagnostic_codes::variant_cycle, "cycle is the diagnostic");
                    }
                    const auto* self = index.value().find("Self", data::Category::game_object);
                    const auto self_tags = index.value().tags(*self);
                    expect(!self_tags && self_tags.error().message.find("Self -> Self") != std::string::npos, "self-cycle chain is named");
                    const auto* last = index.value().find("C300", data::Category::game_object);
                    expect(last != nullptr, "long chain record indexed");
                    if (last) {
                        const auto last_tags = index.value().tags(*last);
                        expect(last_tags && tag(last_tags.value(), "Speed") && tag(last_tags.value(), "Speed")->how == data::RecordTagHow::inherited, "long chain resolves");
                    }
                }
            }
        }
    }
    eawr::testing::remove_temp_directory(root);
    return failures ? 1 : 0;
}
