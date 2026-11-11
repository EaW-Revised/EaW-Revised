#include "eawr/mods/save_description.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace eawr::mods;
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)
namespace {
bool has(const std::vector<DescriptionFinding>& f, FindingKind kind) {
    return std::any_of(f.begin(), f.end(), [kind](const auto& row) { return row.kind == kind; });
}
void run() {
    const std::array<std::uint8_t, 32> digest{};
    const DescriptionEntry x{DescriptionKeyKind::workshop, "workshop:1", "X", "1", digest};
    const DescriptionEntry y{DescriptionKeyKind::mods_folder, "mods:Y#variant:v.json", "Y", "2", digest};
    const DescriptionEntry z{DescriptionKeyKind::workshop, "workshop:3", "Z", "3", digest};
    const ModDescription saved{true, {x, y}};
    const auto bytes = encode_description(saved);
    REQUIRE(bytes && decode_description(*bytes) == saved && encode_description(saved) == bytes);
    auto cheap_x = x; cheap_x.portable_tree = digest; cheap_x.checksum.reset();
    auto cheap_y = y; cheap_y.portable_tree = digest; cheap_y.checksum.reset();
    const ModDescription cheap{true, {cheap_x, cheap_y}};
    REQUIRE(matching_portable_trees(cheap, cheap));
    auto changed_tree = cheap; (*changed_tree.entries[0].portable_tree)[0] = 1;
    REQUIRE(!matching_portable_trees(cheap, changed_tree));
    auto changed_version = cheap; changed_version.entries[0].version = "new";
    REQUIRE(!matching_portable_trees(cheap, changed_version));
    auto with_digest = cheap; with_digest.entries[0].checksum = digest;
    auto different_digest = with_digest; (*different_digest.entries[0].checksum)[0] = 1;
    REQUIRE(!matching_portable_trees(with_digest, different_digest));
    REQUIRE(matching_portable_trees(cheap, with_digest));
    REQUIRE(!matching_portable_trees(cheap, {false, cheap.entries}));
    REQUIRE(decode_description(*encode_description(cheap)) == cheap);
    REQUIRE(compare_descriptions(saved, saved).empty());
    REQUIRE(!description_needs_confirmation(saved, saved));
    REQUIRE(has(compare_descriptions(saved, {true, {x}}), FindingKind::missing));
    REQUIRE(has(compare_descriptions(saved, {true, {x, y, z}}), FindingKind::extra));
    REQUIRE(has(compare_descriptions(saved, {true, {y, x}}), FindingKind::reordered));
    REQUIRE(description_needs_confirmation(saved, {true, {x}}));
    REQUIRE(description_needs_confirmation(saved, {true, {x, y, z}}));
    REQUIRE(description_needs_confirmation(saved, {true, {y, x}}));
    auto changed = x; changed.version = "9";
    const auto mixed = compare_descriptions(saved, {true, {y, changed, z}});
    REQUIRE(has(mixed, FindingKind::version_changed) && has(mixed, FindingKind::reordered) && has(mixed, FindingKind::extra));
    REQUIRE(description_needs_confirmation(saved, {true, {changed, y}}));
    changed.version.reset(); REQUIRE(has(compare_descriptions(saved, {true, {changed, y}}), FindingKind::unknown_version));
    REQUIRE(!description_needs_confirmation(saved, {true, {changed, y}}));
    changed = x; (*changed.checksum)[0] = 1;
    REQUIRE(has(compare_descriptions(saved, {true, {changed, y}}), FindingKind::content_changed));
    REQUIRE(description_needs_confirmation(saved, {true, {changed, y}}));
    const ModDescription manual_chain{true, {{DescriptionKeyKind::manual, "", "Local", {}, {}}}};
    REQUIRE(has(compare_descriptions(manual_chain, manual_chain), FindingKind::unknown_identity));
    REQUIRE(!description_needs_confirmation(manual_chain, manual_chain));
    changed.checksum.reset();
    REQUIRE(has(compare_descriptions(saved, {true, {changed, y}}), FindingKind::checksum_unavailable));
    REQUIRE(!has(compare_descriptions(saved, {true, {changed, y}}), FindingKind::content_changed));
    REQUIRE(!description_needs_confirmation(saved, {true, {changed, y}}));
    REQUIRE(has(compare_descriptions(saved, {false, {{DescriptionKeyKind::manual, "", "Local", {}, {}}}}), FindingKind::unknown_identity));
    REQUIRE(!has(compare_descriptions(saved, {true, {{DescriptionKeyKind::manual, "", "Local", {}, {}}}}), FindingKind::missing));
    REQUIRE(!has(compare_descriptions({true, {{DescriptionKeyKind::manual, "", "Local", {}, {}}}}, saved), FindingKind::extra));
    REQUIRE(has(compare_descriptions(saved, {false, {}}), FindingKind::incomplete));
    for (std::size_t n = 0; n < bytes->size(); ++n) REQUIRE(!decode_description(std::span(*bytes).first(n)));
    auto bad = *bytes; bad.push_back(0); REQUIRE(!decode_description(bad));
    bad = *bytes; bad[0] = 1; REQUIRE(!decode_description(bad));
    bad = *bytes; bad[1] = 2; REQUIRE(!decode_description(bad));
    bad = *bytes; bad[2] = 0xff; bad[3] = 0xff; REQUIRE(!decode_description(bad));
    bad = *bytes; bad[6] = 4; REQUIRE(!decode_description(bad));
    bad = *bytes; bad[7] = 0xff; bad[8] = 0xff; REQUIRE(!decode_description(bad));
    auto empty_checksum = encode_description({true, {{DescriptionKeyKind::unknown, "", "", {}, {}}}});
    REQUIRE(empty_checksum && empty_checksum->size() == 18);
    (*empty_checksum)[16] = 2; REQUIRE(!decode_description(*empty_checksum));
    REQUIRE(!encode_description({true, {x, x}}));
    REQUIRE(!encode_description({true, {{DescriptionKeyKind::manual, "path:secret", "M", {}, {}}}}));
    REQUIRE(!encode_description({true, {{DescriptionKeyKind::mods_folder, "mods:C:", "M", {}, {}}}}));
    REQUIRE(!encode_description({true, {{DescriptionKeyKind::unknown, "", std::string(4097, 'a'), {}, {}}}}));
    REQUIRE(!encode_description({true, {{DescriptionKeyKind::unknown, "", "\xc0\x80", {}, {}}}}));
    REQUIRE(!decode_description(std::vector<std::uint8_t>(kMaxDescriptionBytes + 1)));
    REQUIRE(!encode_description({true, std::vector<DescriptionEntry>(kMaxDescriptionLayers + 1)}));
    ModRecord manual; manual.key = "path:private-hash"; manual.kind = ModKind::manual; manual.name = "Local"; manual.root = "C:/private";
    ModRecord known; known.key = x.key; known.kind = ModKind::workshop; known.name = "X";
    known.metadata = *steam::parse_json(R"({"modinfo":{"version":"1"}})");
    Chain c; c.layers = {manual.key, known.key, "missing"};
    const auto d = describe_chain(c, std::vector<ModRecord>{manual, known});
    REQUIRE(!d.complete && d.entries[0].kind == DescriptionKeyKind::manual && d.entries[0].key.empty());
    REQUIRE(d.entries[1].version == "1" && !d.entries[1].checksum && d.entries[2].kind == DescriptionKeyKind::unknown);
    known.content.state = ContentDigestState::ready; known.content.sha256 = std::string(64, 'a');
    const auto ready = describe_chain(c, std::vector<ModRecord>{manual, known});
    REQUIRE(ready.entries[1].checksum && (*ready.entries[1].checksum)[0] == 0xaa);
    known.content.state = ContentDigestState::pending;
    REQUIRE(!describe_chain(c, std::vector<ModRecord>{manual, known}).entries[1].checksum);
    known.content.state = ContentDigestState::ready; known.content.sha256[0] = 'X';
    REQUIRE(!describe_chain(c, std::vector<ModRecord>{manual, known}).entries[1].checksum);
    const auto encoded = encode_description(d); REQUIRE(encoded);
    const std::string encoded_text(encoded->begin(), encoded->end());
    REQUIRE(encoded_text.find("private") == std::string::npos);

    // describe_mounted_profile: priority order, variant key, ready digest only, no paths.
    const std::string hex = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    ModRecord a_rec; a_rec.key = "workshop:10"; a_rec.kind = ModKind::workshop; a_rec.name = "A"; a_rec.root = "C:/secret/a";
    a_rec.alias = "my alias"; a_rec.content.state = ContentDigestState::ready; a_rec.content.sha256 = hex;
    ModRecord b_rec; b_rec.key = "mods:B#variant:b-modinfo.json"; b_rec.kind = ModKind::mods_folder; b_rec.name = "B2"; b_rec.root = "C:/secret/b";
    b_rec.metadata = *steam::parse_json(R"({"variant_file":"b-modinfo.json","modinfo":{"version":"2.5"}})");
    b_rec.content.state = ContentDigestState::pending; b_rec.content.sha256 = hex; // pending must stay null
    ModRecord m_rec; m_rec.key = "path:hash"; m_rec.kind = ModKind::manual; m_rec.name = "M"; m_rec.root = "D:/secret/m";
    m_rec.content.state = ContentDigestState::unavailable;
    Profile profile; profile.id = "p";
    profile.layers = {{b_rec.key, b_rec.root}, {a_rec.key, a_rec.root}, {m_rec.key, m_rec.root}};
    const auto mounted = describe_mounted_profile(profile, std::vector<ModRecord>{a_rec, m_rec, b_rec});
    REQUIRE(mounted.complete && mounted.entries.size() == 3);
    REQUIRE(mounted.entries[0].key == "mods:B#variant:b-modinfo.json" && mounted.entries[0].name == "B2");
    REQUIRE(mounted.entries[0].version == "2.5" && !mounted.entries[0].checksum);
    REQUIRE(mounted.entries[1].key == "workshop:10" && mounted.entries[1].checksum);
    REQUIRE((*mounted.entries[1].checksum)[0] == 0x00 && (*mounted.entries[1].checksum)[1] == 0x11 && (*mounted.entries[1].checksum)[31] == 0xff);
    REQUIRE(mounted.entries[2].kind == DescriptionKeyKind::manual && mounted.entries[2].key.empty() && !mounted.entries[2].checksum);
    const auto mounted_bytes = encode_description(mounted); REQUIRE(mounted_bytes);
    const std::string mounted_text(mounted_bytes->begin(), mounted_bytes->end());
    REQUIRE(mounted_text.find("secret") == std::string::npos && mounted_text.find("alias") == std::string::npos);
    // A variant layer named "<key>:<variant name>" resolves to the variant record; an unknown layer is incomplete.
    Profile by_name; by_name.layers = {{"mods:B:B2", ""}, {"workshop:404", ""}};
    const auto partial = describe_mounted_profile(by_name, std::vector<ModRecord>{b_rec});
    REQUIRE(!partial.complete && partial.entries[0].key == b_rec.key && partial.entries[1].kind == DescriptionKeyKind::unknown);
    REQUIRE(describe_mounted_profile(Profile{}, {}).complete);
    // A named variant that does not exist stays unknown (null), never a guess.
    Profile nope; nope.layers = {{"mods:B:Nope", ""}};
    const auto none = describe_mounted_profile(nope, std::vector<ModRecord>{b_rec});
    REQUIRE(!none.complete && none.entries.size() == 1 && none.entries[0].kind == DescriptionKeyKind::unknown && none.entries[0].key.empty());
    // The same record, pending first and ready later, gains its checksum without any other change.
    Profile one; one.layers = {{a_rec.key, ""}};
    auto pending = a_rec; pending.content.state = ContentDigestState::pending; pending.content.sha256.clear();
    const auto before = describe_mounted_profile(one, std::vector<ModRecord>{pending});
    const auto after = describe_mounted_profile(one, std::vector<ModRecord>{a_rec});
    REQUIRE(before.complete && !before.entries[0].checksum && after.entries[0].checksum);
    REQUIRE(before.entries[0].key == after.entries[0].key && before.entries[0].name == after.entries[0].name);
}
}
int main() { try { run(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
