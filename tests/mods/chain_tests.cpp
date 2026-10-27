#include "eawr/mods/chain.hpp"
#include "eawr/mods/discovery.hpp"
#include "eawr/mods/steam/library_folders.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <tuple>

namespace {
using namespace eawr::mods;
namespace fs = std::filesystem;
using J = steam::JsonValue;
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error("chain check line " + std::to_string(__LINE__) + ": " #x); } while (false)
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("eawr-chain-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directories(root); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    void write(const fs::path& path, const std::string& text) const {
        fs::create_directories((root / path).parent_path());
        std::ofstream file(root / path, std::ios::binary); file << text;
    }
};
ModRecord record(std::string key, std::string info = "") {
    ModRecord r; r.key = std::move(key); r.has_data = true; r.metadata.kind = J::Kind::object;
    r.kind = r.key.starts_with("workshop:") ? ModKind::workshop : ModKind::mods_folder;
    if (!info.empty()) { r.metadata.keys = {"modinfo"}; r.metadata.values = {*steam::parse_json(info)}; }
    return r;
}
bool has(const Chain& c, std::string_view code) {
    return std::any_of(c.diagnostics.begin(), c.diagnostics.end(), [&](const auto& d) { return d.code == code; });
}
bool has(const OrderCheck& c, std::string_view code) {
    return std::any_of(c.diagnostics.begin(), c.diagnostics.end(), [&](const auto& d) { return d.code == code; });
}
bool warned(const std::vector<Warning>& warnings, std::string_view code) {
    return std::any_of(warnings.begin(), warnings.end(), [&](const auto& w) { return w.code == code; });
}
void parsing() {
    auto info = parse_modinfo(R"({"name":"Synthetic", "version":"1.2.3", "dependencies":["ResolveLastItem",{"modtype":0,"identifier":".\\Mods\\Base"},{"modtype":1,"identifier":"100"}],"role":"parent-only"})");
    REQUIRE(info && info->version == "1.2.3" && info->role == ModRole::parent_only);
    REQUIRE(info->dependencies->layout == ResolveLayout::last_item);
    REQUIRE(info->dependencies->parents == (std::vector<std::string>{"mods:Base", "workshop:100"}));
    REQUIRE(parse_modinfo("{/*comment*/\"name\":\"a//b\",\"dependencies\":[\"FullResolved\",],}//tail"));
    REQUIRE(parse_modinfo(R"({"name":"A","dependencies":[]})"));
    const auto titled = parse_modinfo(R"({"steamdata":{"title":"Local Steam title"},"dependencies":[]})");
    REQUIRE(titled && titled->name == "Local Steam title" && titled->dependencies);
    REQUIRE(parse_modinfo(R"({"name":"Explicit","steamdata":{"title":"Lower priority"}})")->name == "Explicit");
    REQUIRE(parse_modinfo(R"({"name":"","steamdata":{"title":"Fallback"}})")->name == "Fallback");
    REQUIRE(!parse_modinfo(R"({"steamdata":{"title":42}})"));
    REQUIRE(!parse_modinfo(R"({"steamdata":{"title":""}})"));
    Catalog named;
    auto title_record = record("workshop:10");
    title_record.name = titled->name;
    title_record.metadata = *steam::parse_json(R"({"modinfo":{"steamdata":{"title":"Local Steam title"}}})");
    named.records.push_back(title_record);
    KnownChains lower_names;
    lower_names.chains.push_back({"10", "Known chain name", {}, ModRole::unknown});
    apply_known_chains(named, lower_names);
    REQUIRE(named.records.front().name == "Local Steam title");
    for (const auto* bad : {"{", "[]", "{}", "{\"name\":null}", "{\"name\":\"\"}",
                           R"({"name":"x","version":1})", R"({"name":"x","dependencies":null})",
                           R"({"name":"x","dependencies":["Unknown"]})", R"({"name":"x","dependencies":[{"modtype":1,"identifier":"abc"}]})",
                           R"({"name":"x","dependencies":[{"modtype":0.5,"identifier":"abc"}]})",
                           R"({"name":"x","dependencies":[{"modtype":0,"identifier":""}]})", "/*broken"}) REQUIRE(!parse_modinfo(bad));
    REQUIRE(!parse_modinfo(std::string(kMaxModText + 1, ' ')));
    const auto known = parse_known_chains(R"({"format":"eawr-known-chains","version":1,"unknown":true,"chains":[{"id":"10","name":"Synthetic","parents":["20","30"],"role":"parent-only","other":1}]})");
    REQUIRE(known.warnings.empty() && known.chains.size() == 1 && known.chains[0].parents[1] == "30");
    for (const auto* bad : {"{}", "{", R"({"format":"eawr-known-chains","version":2,"chains":[]})",
                           R"({"format":"eawr-known-chains","version":1,"chains":[{"id":"x","name":"x","parents":[]}]})",
                           R"({"format":"eawr-known-chains","version":1,"chains":[{"id":"10","name":"x","parents":[1]}]})"}) {
        const auto result = parse_known_chains(bad);
        REQUIRE(result.chains.empty() && warned(result.warnings, codes::malformed_known_chains));
    }
    REQUIRE(launch_script_hints("run STEAMMOD=10 MODPATH=\"Mods/Local Base\" XSTEAMMOD=99 STEAMMOD=oops MODPATH=%VAR%")
            == (std::vector<std::string>{"workshop:10", "mods:Local Base"}));
    REQUIRE(launch_script_hints(std::string(kMaxModText + 1, 'x')).empty());
}
void resolution() {
    std::vector<ModRecord> records{record("workshop:10", R"({"name":"Leaf","dependencies":[{"modtype":1,"identifier":"20"}]})"),
                                   record("workshop:20"), record("workshop:30"), record("mods:Local")};
    KnownChains known; known.chains.push_back({"10", "Known name", {"30"}, ModRole::unknown});
    auto c = resolve_chain("workshop:10", records, known);
    REQUIRE(c.launchable && c.layers == (std::vector<std::string>{"workshop:10", "workshop:20"}) && c.rows[1].why == "modinfo");
    REQUIRE(c.edges.size() == 1 && c.edges[0].child == "workshop:10" && c.edges[0].parent == "workshop:20" && c.edges[0].why == "modinfo");
    c = resolve_chain("workshop:10", records, known, {{"workshop:10", {"mods:Local"}}});
    REQUIRE(c.launchable && c.rows[1].why == "player" && c.layers.back() == "mods:Local");
    REQUIRE(resolve_chain("workshop:10", records, known, {{"workshop:10", {}}}).layers.size() == 1);
    records[0].metadata = J{};
    c = resolve_chain("workshop:10", records, known);
    REQUIRE(c.rows[1].why == "known-chain" && c.layers.back() == "workshop:30");
    records[0].metadata = *steam::parse_json(R"({"launch_script_hints":["workshop:20"]})");
    REQUIRE(resolve_chain("workshop:10", records, known).rows[1].why == "known-chain");
    c = resolve_chain("workshop:10", records, {});
    REQUIRE(c.rows[1].why == "launch-script" && !c.launchable && has(c, codes::script_hint));
    REQUIRE(resolve_chain("workshop:10", records, {}, {{"workshop:10", {"workshop:20"}}}).launchable);
    records[0].metadata = J{};
    c = resolve_chain("workshop:10", records, {}, {{"workshop:10", {"workshop:999"}}});
    REQUIRE(!c.launchable && has(c, codes::parent_not_installed) && !c.diagnostics.back().listed_by_steam.has_value());
    REQUIRE(c.diagnostics.back().message.find("Steam listing is unknown") != std::string::npos);
    records[1].state = ModState::downloading;
    c = resolve_chain("workshop:10", records, {}, {{"workshop:10", {"workshop:20"}}});
    REQUIRE(!c.launchable && has(c, codes::downloading) && c.diagnostics.back().listed_by_steam == true && !c.diagnostics.back().installed);
    records[1].state = ModState::missing;
    c = resolve_chain("workshop:10", records, {}, {{"workshop:10", {"workshop:20"}}});
    REQUIRE(has(c, codes::parent_not_installed) && c.diagnostics.back().listed_by_steam == true);
    records[1].warnings = {{std::string(codes::folder_not_listed), "synthetic unlisted folder"}};
    c = resolve_chain("workshop:10", records, {}, {{"workshop:10", {"workshop:20"}}});
    REQUIRE(c.diagnostics.back().listed_by_steam == false);
    records[1].warnings.push_back({std::string(codes::workshop_manifest_unreadable), "synthetic unreadable list"});
    c = resolve_chain("workshop:10", records, {}, {{"workshop:10", {"workshop:20"}}});
    REQUIRE(!c.diagnostics.back().listed_by_steam.has_value());
    REQUIRE(c.diagnostics.back().message.find("Steam listing is unknown") != std::string::npos);
    records[1].warnings.clear();
    records[1].state = ModState::installed;
    records[1].has_data = false;
    REQUIRE(has(resolve_chain("workshop:10", records, {}, {{"workshop:10", {"workshop:20"}}}), codes::not_playable));
    records[1].has_data = true; records[1].role = ModRole::parent_only;
    REQUIRE(resolve_chain("workshop:10", records, {}, {{"workshop:10", {"workshop:20"}}}).launchable);
    REQUIRE(!resolve_chain("workshop:20", records, {}).launchable);
    records[1].role = ModRole::unknown;
    c = resolve_chain("workshop:10", records, {}, {{"workshop:10", {"workshop:20"}}, {"workshop:20", {"workshop:10"}}});
    REQUIRE(!c.launchable && has(c, codes::chain_cycle));
    c = resolve_chain("workshop:10", records, {}, {{"workshop:10", {"workshop:20", "workshop:20"}}});
    REQUIRE(c.launchable && has(c, codes::chain_duplicate) && c.layers.size() == 2);
    c = resolve_chain("workshop:10", records, {}, {{"workshop:10", {"workshop:20", "workshop:30", "workshop:20"}}});
    REQUIRE(c.launchable && has(c, codes::chain_duplicate) && c.layers.size() == 3);
    // Diamond: all direct parents precede their shared ancestor.
    c = resolve_chain("workshop:10", records, {}, {{"workshop:10", {"workshop:20", "workshop:30"}},
                      {"workshop:20", {"mods:Local"}}, {"workshop:30", {"mods:Local"}}});
    REQUIRE(c.launchable && c.layers == (std::vector<std::string>{"workshop:10", "workshop:20", "workshop:30", "mods:Local"}));
    REQUIRE(!has(c, codes::chain_duplicate));
    REQUIRE(c.edges.size() == 4 && c.edges[2].parent == "mods:Local" && c.edges[3].parent == "mods:Local");
    REQUIRE(c.edges[2].child != c.edges[3].child && c.edges[2].why == "player" && c.edges[3].why == "player");
    const auto hash = c.fingerprint;
    REQUIRE(hash.size() == 64 && chain_fingerprint(c.layers, records) == hash);
    auto changed = records;
    changed[1].fingerprint.acf_manifest = "new";
    REQUIRE(chain_fingerprint(c.layers, changed) != hash);
    changed = records; changed[1].fingerprint.tree = "new";
    REQUIRE(chain_fingerprint(c.layers, changed) != hash);
    auto order = c.layers; std::swap(order[1], order[2]);
    REQUIRE(chain_fingerprint(order, records) != hash);
    records[0] = record("workshop:10", R"({"name":"x","dependencies":["FullResolved",{"modtype":1,"identifier":"20"}]})");
    REQUIRE(resolve_chain("workshop:10", records, {}, {{"workshop:20", {"mods:Local"}}}).layers.size() == 2);
    records[0] = record("workshop:10", R"({"name":"x","dependencies":["ResolveLastItem",{"modtype":1,"identifier":"20"},{"modtype":1,"identifier":"30"}]})");
    c = resolve_chain("workshop:10", records, {}, {{"workshop:20", {"missing"}}, {"workshop:30", {"mods:Local"}}});
    REQUIRE(c.launchable && c.layers.size() == 4);
    Catalog catalog; catalog.records = records;
    apply_known_chains(catalog, known);
    REQUIRE(catalog.records[0].name != "Known name");
    catalog.records[0].metadata = J{}; apply_known_chains(catalog, known);
    REQUIRE(catalog.records[0].name == "Known name");
}
void player_orders() {
    std::vector<ModRecord> records{
        record("workshop:10", R"({"name":"Leaf","dependencies":[{"modtype":1,"identifier":"20"}]})"),
        record("workshop:20"), record("mods:Other")};
    records[1].role = ModRole::parent_only;
    const std::vector<std::string> clean{"workshop:10", "workshop:20", "mods:Other"};
    const std::vector<std::string> accepted{std::string(codes::chain_order), std::string(codes::chain_duplicate),
        std::string(codes::parent_not_installed), std::string(codes::chain_cycle), std::string(codes::downloading),
        std::string(codes::scan_incomplete)};
    auto check = check_order(clean, records, {});
    REQUIRE(check.launchable && check.warnings.empty() && check.diagnostics.empty());
    REQUIRE(check_order({}, records, {}).launchable);
    // A parent-only layer in first position is still not a playable leaf.
    const std::vector<std::string> reversed{"workshop:20", "workshop:10"};
    REQUIRE(!check_order(reversed, records, {}, accepted).launchable);
    records[1].role = ModRole::unknown;
    check = check_order(reversed, records, {});
    REQUIRE(!check.launchable && check.warnings.size() == 1 && check.warnings[0].code == codes::chain_order);
    REQUIRE(check_order(reversed, records, {}, accepted).launchable);
    const std::vector<std::string> duplicate{"workshop:10", "workshop:20", "workshop:20"};
    REQUIRE(!check_order(duplicate, records, {}).launchable);
    REQUIRE(check_order(duplicate, records, {}, accepted).launchable);
    const std::vector<std::string> missing{"workshop:10"}, unknown{"absent"};
    REQUIRE(!check_order(missing, records, {}, accepted).launchable);
    REQUIRE(!check_order(unknown, records, {}, accepted).launchable);
    REQUIRE(check_order(missing, records, {}).diagnostics[0].key == "workshop:20");
    for (const auto state : {ModState::downloading, ModState::missing}) {
        records[1].state = state;
        check = check_order(clean, records, {}, accepted);
        REQUIRE(!check.launchable && has(check, state == ModState::downloading ? codes::downloading : codes::parent_not_installed));
    }
    check = check_order(missing, records, {}, accepted);
    REQUIRE(!check.launchable && check.diagnostics.size() == 1 && has(check, codes::parent_not_installed));
    records[1].state = ModState::needs_update;
    REQUIRE(check_order(clean, records, {}).launchable);
    for (const auto code : {codes::tree_walk_capped, codes::tree_walk_incomplete}) {
        records[1].warnings = {{std::string(code), "synthetic change-check limit"}};
        REQUIRE(resolve_chain("workshop:10", records, {}).launchable);
        REQUIRE(check_order(clean, records, {}).launchable);
    }
    records[1].warnings = {{std::string(codes::scan_incomplete), "synthetic metadata scan incomplete"}};
    REQUIRE(resolve_chain("workshop:10", records, {}).launchable);
    check = check_order(clean, records, {}, accepted);
    REQUIRE(!check.launchable && has(check, codes::scan_incomplete));
    records[1].warnings.clear();
    records[1] = record("workshop:20", R"({"name":"Parent","dependencies":[{"modtype":1,"identifier":"10"}]})");
    check = check_order(clean, records, {}, accepted);
    REQUIRE(!check.launchable && has(check, codes::chain_cycle));
    records[1] = record("workshop:20");
    records[0] = record("workshop:10", R"({"name":"Leaf","dependencies":[{"modtype":1,"identifier":"20"},{"modtype":1,"identifier":"20"}]})");
    check = check_order(clean, records, {});
    REQUIRE(check.launchable && check.warnings.empty() && check.diagnostics.empty());
    records[0] = record("workshop:10");
    KnownChains known; known.chains.push_back({"10", "Known", {"20"}, ModRole::unknown});
    REQUIRE(!check_order(missing, records, known, accepted).launchable);
    REQUIRE(check_order(reversed, records, known, accepted).launchable);
    // Sibling constraints and aliases come from exactly the resolver graph.
    records[0] = record("workshop:10", R"({"name":"Leaf","dependencies":["FullResolved",{"modtype":1,"identifier":"20"},{"modtype":0,"identifier":"Other"}]})");
    const std::vector<std::string> siblings{"workshop:10", "mods:Other", "workshop:20"};
    check = check_order(siblings, records, {});
    REQUIRE(!check.launchable && check.warnings.size() == 1 && check.warnings[0].code == codes::chain_order);
    REQUIRE(check.warnings[0].message.find("declared order") != std::string::npos);
    REQUIRE(check_order(siblings, records, {}, accepted).launchable);
    const std::vector<std::string> aliases{"mods:Other", "mods:other"};
    REQUIRE(!check_order(aliases, records, {}).launchable);
    REQUIRE(check_order(aliases, records, {}, accepted).launchable);
}
void files() {
    Fixture f;
    f.write("Titled/Data/XML/a.xml", "synthetic");
    f.write("Titled/modinfo.json", R"({"steamdata":{"title":"Discovered title"}})");
    DiscoveryInput title_input; title_input.manual_folders = {f.root / "Titled"};
    const auto title_catalog = discover(title_input);
    REQUIRE(title_catalog.records.size() == 1 && title_catalog.records.front().name == "Discovered title");
    f.write("Named/Data/XML/a.xml", "synthetic");
    f.write("Named/modinfo.json", R"({"name":"Explicit main","steamdata":{"title":"Lower main title"}})");
    f.write("Named/variant-modinfo.json", R"({"steamdata":{"title":"Variant title"}})");
    title_input.manual_folders = {f.root / "Named"};
    const auto variant_catalog = discover(title_input);
    REQUIRE(variant_catalog.records.size() == 2);
    REQUIRE(variant_catalog.records[0].name == "Explicit main");
    REQUIRE(variant_catalog.records[1].name == "Variant title");
    REQUIRE(variant_catalog.records[1].metadata.member("variant_file"));
    f.write("Leaf/Data/XML/a.xml", "synthetic");
    f.write("Leaf/modinfo.json", R"({"name":"Main","version":"1.0.0","dependencies":[{"modtype":0,"identifier":"Base"}]})");
    f.write("Leaf/a-modinfo.json", R"({"name":"Variant A"})");
    f.write("Leaf/b-modinfo.json", R"({"name":"Variant B","dependencies":[]})");
    f.write("Leaf/broken-modinfo.json", "{");
    f.write("Leaf/start.cmd", "run STEAMMOD=10 MODPATH=Mods/Base");
    f.write("Leaf/nested/ignored-modinfo.json", R"({"name":"Ignored"})");
    DiscoveryInput input; input.manual_folders = {f.root / "Leaf"};
    const auto catalog = discover(input);
    REQUIRE(catalog.records.size() == 3 && catalog.records[0].name == "Main");
    REQUIRE(catalog.records[1].name == "Variant A" && catalog.records[2].name == "Variant B");
    REQUIRE(catalog.records[1].key.ends_with("#variant:a-modinfo.json"));
    REQUIRE(catalog.records[1].metadata.member("modinfo")->string_member("version") != nullptr);
    REQUIRE(catalog.records[0].has_warning(codes::malformed_modinfo));
    auto rows = catalog.records; rows.push_back(record("mods:Base"));
    REQUIRE(resolve_chain(rows[1].key, rows, {}).layers.back() == "mods:Base");
    REQUIRE(resolve_chain(rows[2].key, rows, {}).layers.size() == 1);
    REQUIRE(resolve_chain(rows[1].key, rows, {}).launchable);
    Catalog again = catalog; read_mod_metadata(again);
    REQUIRE(again.records.size() == 3);
    f.write("Only/Data/a.txt", "synthetic");
    f.write("Only/v-modinfo.json", R"({"name":"Only variant"})");
    input.manual_folders = {f.root / "Only"};
    REQUIRE(discover(input).records.size() == 1 && discover(input).records[0].name == "Only variant");
    f.write("Bad/Data/a.txt", "synthetic"); f.write("Bad/modinfo.json", "{");
    f.write("Bad/v-modinfo.json", std::string(kMaxModText + 1, ' '));
    input.manual_folders = {f.root / "Bad"};
    const auto bad = discover(input);
    REQUIRE(bad.records.size() == 1 && bad.records[0].has_warning(codes::malformed_modinfo));
    REQUIRE(!steam::read_text_file(f.root / "Bad/v-modinfo.json", kMaxModText));
    // A broken Steam manifest cannot establish that a record-less parent is unlisted.
    f.write("Steam/steamapps/workshop/appworkshop_32470.acf", "\"AppWorkshop\" {");
    input.steam_libraries = {f.root / "Steam"};
    const auto unknown_catalog = discover(input);
    REQUIRE(warned(unknown_catalog.warnings, codes::workshop_manifest_unreadable));
    const auto unknown_parent = resolve_chain(unknown_catalog.records[0].key, unknown_catalog.records, {},
                                             {{unknown_catalog.records[0].key, {"workshop:999"}}});
    REQUIRE(has(unknown_parent, codes::parent_not_installed) && !unknown_parent.diagnostics.back().listed_by_steam.has_value());
    REQUIRE(unknown_parent.diagnostics.back().message.find("Steam listing is unknown") != std::string::npos);
    f.write("corruption/Data/XML/retail.xml", "synthetic"); f.write("corruption/Data/retail.meg", "synthetic");
    f.write("corruption/Data/other.txt", "irrelevant");
    const std::vector<std::string> retail{"XML/retail.xml", "retail.meg"};
    REQUIRE(detect_data_overwrite(f.root / "corruption", retail).empty());
    f.write("corruption/Data/XML/extra.XML", "synthetic");
    REQUIRE(warned(detect_data_overwrite(f.root / "corruption", retail), codes::data_overwrite));
    fs::remove(f.root / "corruption/Data/XML/extra.XML");
    f.write("corruption/Data/extra.MEG", "synthetic");
    REQUIRE(warned(detect_data_overwrite(f.root / "corruption", retail), codes::data_overwrite));
    REQUIRE(warned(detect_data_overwrite(f.root / "corruption", retail, 0), codes::scan_incomplete));
    REQUIRE(detect_data_overwrite(f.root / "missing", retail).empty());
    // Default discovery and metadata reads leave every file's size and timestamp unchanged.
    using Snapshot = std::map<std::string, std::tuple<std::uintmax_t, fs::file_time_type>>;
    auto snapshot = [&]() {
        Snapshot s;
        for (const auto& entry : fs::recursive_directory_iterator(f.root))
            s[entry.path().generic_string()] = {fs::is_directory(entry.path()) ? 0 : fs::file_size(entry.path()),
                                              fs::last_write_time(entry.path())};
        return s;
    };
    const auto writable = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
    for (const auto& entry : fs::recursive_directory_iterator(f.root)) if (entry.is_regular_file())
        fs::permissions(entry.path(), writable, fs::perm_options::remove);
    // Attribute setup can change directory times on NTFS. Observe only the reads between snapshots,
    // using fresh stat calls rather than directory_entry's cached timestamps (as in mods_tests.cpp).
    const auto before = snapshot();
    static_cast<void>(discover(input));
    input.manual_folders = {f.root / "Leaf"};
    REQUIRE(discover(input).records[1].name == "Variant A");
    static_cast<void>(detect_data_overwrite(f.root / "corruption", retail));
    const auto after = snapshot();
    for (const auto& [path, value] : before) {
        const auto found = after.find(path);
        if (found == after.end() || found->second != value) std::cerr << "read-only snapshot changed: " << path << '\n';
    }
    REQUIRE(before == after);
    for (const auto& entry : fs::recursive_directory_iterator(f.root)) if (entry.is_regular_file())
        fs::permissions(entry.path(), writable, fs::perm_options::add);
    // Variant references use the public name-qualified identity, while stored keys remain filename-stable.
    auto workshop = record("workshop:90"); workshop.name = "Synthetic variant";
    workshop.key += "#variant:v-modinfo.json";
    workshop.metadata = *steam::parse_json(R"({"variant_file":"v-modinfo.json"})");
    rows = {record("workshop:80", R"({"name":"Sub","dependencies":[{"modtype":1,"identifier":"90:Synthetic variant"}]})"), workshop};
    REQUIRE(resolve_chain("workshop:80", rows, {}).launchable);
}
}
void test_chains() {
    try { parsing(); resolution(); player_orders(); files(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::exit(1); }
}
