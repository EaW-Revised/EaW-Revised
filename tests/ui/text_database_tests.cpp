// Text database contracts, rules UI-T1 to UI-T4 (#168). Fixtures are built
// here from the documented layout. With EAWR_EAW_GAME_ROOT set, the FoC
// MasterTextFile_English.dat is also loaded read-only through the FoC VFS.

#include "eawr/data/ui/text_database.hpp"

#include "ui_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using eawr::test::ui::expect;
namespace ui = eawr::data::ui;

void u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
    bytes.push_back(static_cast<std::byte>(value));
}
void u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        u8(bytes, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

struct Record final {
    Record(std::string key_text, std::u16string value_text,
           const std::optional<std::uint32_t> stored_crc = std::nullopt)
        : key(std::move(key_text)), value(std::move(value_text)), crc(stored_crc) {}
    std::string key;
    std::u16string value;
    std::optional<std::uint32_t> crc;
};

// Writes the UI-T1 layout in the order given; callers choose the order.
std::vector<std::byte> encode(const std::vector<Record>& records) {
    std::vector<std::byte> bytes;
    u32(bytes, static_cast<std::uint32_t>(records.size()));
    for (const auto& record : records) {
        u32(bytes, record.crc.value_or(ui::text_key_crc(record.key)));
        u32(bytes, static_cast<std::uint32_t>(record.value.size()));
        u32(bytes, static_cast<std::uint32_t>(record.key.size()));
    }
    for (const auto& record : records) {
        for (const char16_t unit : record.value) {
            u8(bytes, static_cast<std::uint8_t>(unit & 0xffU));
            u8(bytes, static_cast<std::uint8_t>((unit >> 8U) & 0xffU));
        }
    }
    for (const auto& record : records) {
        for (const char character : record.key) u8(bytes, static_cast<std::uint8_t>(character));
    }
    return bytes;
}

std::vector<Record> sorted(std::vector<Record> records) {
    std::stable_sort(records.begin(), records.end(), [](const Record& left, const Record& right) {
        return ui::text_key_crc(left.key) < ui::text_key_crc(right.key);
    });
    return records;
}

eawr::assets::Source source(const std::string& path, const std::size_t size) {
    return {path, "synthetic", "test", eawr::vfs::AssetOrigin::loose, size};
}

eawr::core::Result<ui::TextDatabase> load(const std::vector<std::byte>& bytes) {
    return ui::load_text_database(
        bytes, source("data/text/mastertextfile_test.dat", bytes.size()));
}

bool fails_with(const std::vector<std::byte>& bytes, const std::string_view code) {
    auto loaded = load(bytes);
    return !loaded && loaded.error().code == code;
}

void crc_contracts() {
    using ui::text_key_crc;
    expect(text_key_crc("123456789") == 0xcbf43926U, "key CRC is the IEEE CRC-32 check value");
    expect(text_key_crc("") == 0U, "empty key CRC is zero");
    expect(text_key_crc("TEXT_A") != text_key_crc("text_a"), "key CRC is over the exact bytes");
}

// UI-T1: exact layout, value decoding, CRC check and the sorted flag.
void layout_contracts() {
    using namespace eawr::data::ui;
    const auto records = sorted({
        {"TEXT_ALPHA", u"Alpha"},
        {"TEXT_BETA", u"B\u00e9ta \U0001F600"},
        {"TEXT_EMPTY", u""},
        {"TEXT WITH SPACE", u"Space"},
    });
    const auto bytes = encode(records);
    auto loaded = load(bytes);
    expect(static_cast<bool>(loaded), "sorted fixture loads");
    if (!loaded) return;
    const auto& database = loaded.value();
    expect(database.entries.size() == 4U, "every record is kept");
    expect(database.sorted_by_crc, "CRC-sorted fixture is flagged sorted");
    expect(database.duplicates.empty() && database.diagnostics.empty(), "unique keys report nothing");
    for (std::size_t index = 0U; index < records.size(); ++index) {
        expect(database.entries[index].key == records[index].key, "entries keep file order");
        expect(database.entries[index].value == records[index].value, "UTF-16LE values round-trip");
        expect(database.entries[index].crc == text_key_crc(records[index].key), "stored CRC kept");
    }
    const auto* beta = database.find("TEXT_BETA");
    expect(beta != nullptr && to_utf8(beta->value) == "B\xc3\xa9ta \xf0\x9f\x98\x80",
           "surrogate pairs and Latin-1 decode to UTF-8");
    const auto* empty = database.find("TEXT_EMPTY");
    expect(empty != nullptr && empty->value.empty(), "zero-length value is valid");
    expect(database.find("TEXT WITH SPACE") != nullptr, "printable ASCII keys include spaces");

    auto unsorted = records;
    std::swap(unsorted.front(), unsorted.back());
    auto credits = load(encode(unsorted));
    expect(credits && !credits.value().sorted_by_crc, "display-order file loads unsorted");
    expect(credits && credits.value().find("TEXT_ALPHA") != nullptr, "unsorted file still looks up");

    auto empty_file = load(encode({}));
    expect(empty_file && empty_file.value().entries.empty() && empty_file.value().sorted_by_crc,
           "zero records load");

    expect(fails_with({std::byte{1}, std::byte{0}}, diagnostic_codes::text_database_layout), "short count");
    auto short_table = bytes;
    short_table.resize(4U + 12U * 3U + 2U);
    expect(fails_with(short_table, diagnostic_codes::text_database_layout), "short record table");
    auto short_payload = bytes;
    short_payload.pop_back();
    expect(fails_with(short_payload, diagnostic_codes::text_database_layout), "short key data");
    auto trailing = bytes;
    trailing.push_back(std::byte{0});
    expect(fails_with(trailing, diagnostic_codes::text_database_layout), "trailing bytes are rejected");
    auto bad_crc = records;
    bad_crc[1].crc = text_key_crc(bad_crc[1].key) ^ 1U;
    expect(fails_with(encode(bad_crc), diagnostic_codes::text_database_record), "CRC mismatch");
    expect(fails_with(encode({{"", u"x", 0U}}), diagnostic_codes::text_database_record), "empty key");
    expect(fails_with(encode({{"TEXT_\x7f", u"x"}}), diagnostic_codes::text_database_record),
           "control byte in key");
    expect(fails_with(encode({{"TEXT_\xc3\xa9", u"x"}}), diagnostic_codes::text_database_record),
           "non-ASCII key");
    auto mismatch = load_text_database(bytes, source("data/text/x.dat", bytes.size() + 1U));
    expect(!mismatch && mismatch.error().code == diagnostic_codes::text_database_layout,
           "provenance size mismatch");
}

// UI-T2: exact lookup and duplicate reporting.
void lookup_contracts() {
    using namespace eawr::data::ui;
    auto loaded = load(encode(sorted({
        {"MISSION_Name", u"mixed"},
        {"MISSION_NAME", u"upper"},
        {"TEXT_SAME", u"same"},
        {"TEXT_TWICE", u"first"},
        {"TEXT_SAME", u"same"},
        {"TEXT_TWICE", u"second"},
        {"TEXT_TWICE", u"third"},
    })));
    expect(static_cast<bool>(loaded), "duplicate fixture loads");
    if (!loaded) return;
    const auto& database = loaded.value();
    expect(database.find("MISSION_Name") != nullptr && database.find("MISSION_Name")->value == u"mixed",
           "case-distinct key resolves to its own record");
    expect(database.find("MISSION_NAME") != nullptr && database.find("MISSION_NAME")->value == u"upper",
           "upper-case key resolves to its own record");
    expect(database.find("mission_name") == nullptr, "lookup is case-sensitive");
    expect(database.find("TEXT_TWIC") == nullptr, "lookup is not by prefix");
    expect(database.find("TEXT_TWICE") != nullptr && database.find("TEXT_TWICE")->value == u"first",
           "first duplicate in file order wins");
    expect(database.duplicates.size() == 2U, "each duplicated key is reported once");
    expect(database.diagnostics.size() == 2U, "one diagnostic per duplicated key");
    for (const auto& diagnostic : database.diagnostics) {
        expect(diagnostic.code == diagnostic_codes::text_duplicate_key &&
                   diagnostic.severity == eawr::core::Severity::warning,
               "duplicate diagnostic is a warning");
    }
    for (const auto& duplicate : database.duplicates) {
        if (duplicate.key == "TEXT_TWICE") {
            expect(duplicate.records.size() == 3U && !duplicate.values_equal,
                   "three differing TEXT_TWICE records");
            expect(database.entries[duplicate.records.front()].value == u"first",
                   "duplicate records are listed in file order");
        } else {
            expect(duplicate.key == "TEXT_SAME" && duplicate.records.size() == 2U &&
                       duplicate.values_equal,
                   "two equal TEXT_SAME records");
        }
    }

    // Distinct keys sharing one CRC-32 (a known collision pair) stay distinct.
    expect(text_key_crc("plumless") == text_key_crc("buckeroo"), "collision pair shares a CRC");
    auto collision = load(encode({{"plumless", u"p"}, {"buckeroo", u"b"}}));
    expect(collision && collision.value().duplicates.empty(), "colliding keys are not duplicates");
    expect(collision && collision.value().find("buckeroo") != nullptr &&
               collision.value().find("buckeroo")->value == u"b",
           "the second colliding key resolves to its own record");
}

// UI-T3: a missing key renders its key and is reported once.
void missing_key_contracts() {
    using namespace eawr::data::ui;
    auto loaded = load(encode(sorted({{"TEXT_PRESENT", u"present"}})));
    expect(static_cast<bool>(loaded), "missing-key fixture loads");
    if (!loaded) return;
    TextLookup lookup(loaded.value());
    expect(lookup.text("TEXT_PRESENT") == u"present", "present key renders its value");
    expect(lookup.diagnostics().empty(), "present key reports nothing");
    expect(lookup.text("TEXT_ABSENT") == u"TEXT_ABSENT", "missing key renders the key text");
    expect(lookup.text("TEXT_ABSENT") == u"TEXT_ABSENT", "missing key renders again");
    expect(lookup.diagnostics().size() == 1U, "one diagnostic per missing key");
    expect(lookup.text("TEXT_OTHER") == u"TEXT_OTHER", "second missing key renders");
    expect(lookup.diagnostics().size() == 2U, "second missing key adds one diagnostic");
    for (const auto& diagnostic : lookup.diagnostics()) {
        expect(diagnostic.code == diagnostic_codes::text_missing_key &&
                   diagnostic.severity == eawr::core::Severity::warning,
               "missing-key diagnostic is a warning");
    }
    expect(lookup.diagnostics().front().message.find("TEXT_ABSENT") != std::string::npos,
           "missing-key diagnostic names the key");
}

// UI-T4: the language path, resolved through the VFS so the FoC layer wins.
void path_contracts() {
    using namespace eawr::data::ui;
    auto english = text_database_path("ENGLISH");
    expect(english && english.value() == "Data/Text/MasterTextFile_ENGLISH.dat", "language path");
    auto german = text_database_path("German");
    expect(german && german.value() == "Data/Text/MasterTextFile_German.dat", "language case kept");
    for (const std::string_view bad : {"", "../ENGLISH", "ENG LISH", "ENGLISH.dat", "ENGLISH/x",
                                       "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFG"}) {
        auto rejected = text_database_path(bad);
        expect(!rejected && rejected.error().code == diagnostic_codes::text_language,
               "invalid language is rejected");
    }

    eawr::test::ui::TempTree tree("text");
    const auto expansion = tree.root / "corruption" / "Data";
    const auto base = tree.root / "GameData" / "Data";
    eawr::test::ui::write_bytes(expansion / "Text" / "mastertextfile_english.dat",
                encode({{"TEXT_LAYER", u"expansion"}}));
    eawr::test::ui::write_bytes(base / "Text" / "mastertextfile_english.dat", encode({{"TEXT_LAYER", u"base"}}));
    eawr::test::ui::write_bytes(base / "Text" / "mastertextfile_german.dat", encode({{"TEXT_LAYER", u"base-de"}}));
    const std::array mounts{
        eawr::vfs::MountSpec{"expansion", expansion, "data", {}},
        eawr::vfs::MountSpec{"base", base, "data", {}},
    };
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "fixture VFS mounts");
    if (!filesystem) return;
    auto loaded = load_language_text_database(filesystem.value(), "English");
    expect(loaded && loaded.value().find("TEXT_LAYER")->value == u"expansion",
           "the FoC text folder wins over the base game");
    expect(loaded && loaded.value().source.layer_id == "expansion", "source names the FoC layer");
    auto fallback = load_language_text_database(filesystem.value(), "GERMAN");
    expect(fallback && fallback.value().find("TEXT_LAYER")->value == u"base-de",
           "a language only the base game ships still resolves");
    auto missing = load_language_text_database(filesystem.value(), "KLINGON");
    expect(!missing && missing.error().code == eawr::vfs::diagnostic_codes::not_found,
           "an absent language is a VFS not-found error");
}

// Read-only FoC corpus check (#168 acceptance).
void corpus_contracts() {
    using namespace eawr::data::ui;
    auto filesystem = eawr::test::ui::foc_corpus("text database");
    if (!filesystem) return;

    auto loaded = load_language_text_database(filesystem.value(), "English");
    if (!loaded) std::cerr << eawr::core::format_diagnostic(loaded.error()) << '\n';
    expect(static_cast<bool>(loaded), "FoC English text database loads with every CRC valid");
    if (!loaded) return;
    const auto& database = loaded.value();
    std::size_t excess{};
    for (const auto& duplicate : database.duplicates) excess += duplicate.records.size() - 1U;
    std::cout << "text database corpus: " << database.source.logical_path << " ("
              << database.source.layer_id << ") " << database.entries.size() << " records, "
              << (database.sorted_by_crc ? "sorted" : "not sorted") << " by CRC, "
              << database.duplicates.size() << " duplicated key(s), " << excess
              << " excess record(s)\n";
    for (const auto& diagnostic : database.diagnostics) {
        std::cout << "  " << eawr::core::format_diagnostic(diagnostic) << '\n';
    }
    expect(database.source.layer_id == "expansion", "the FoC text file is used");
    expect(database.entries.size() == 19224U, "FoC holds 19,224 records");
    expect(database.sorted_by_crc, "FoC records are sorted by CRC");
    expect(database.duplicates.size() == 1U && database.duplicates.front().key == "TEXT_END_OF_DATA",
           "TEXT_END_OF_DATA is the only duplicated key");
    expect(excess == 6U, "6 excess TEXT_END_OF_DATA records are reported");
    expect(!database.duplicates.empty() && database.duplicates.front().values_equal,
           "the TEXT_END_OF_DATA copies are identical");
    const auto* mixed = database.find("MBTM00_Galactic");
    const auto* upper = database.find("MBTM00_GALACTIC");
    expect(mixed != nullptr && upper != nullptr && mixed != upper,
           "the corpus holds keys that differ only in case");

    auto credits = load_text_database(filesystem.value(), "Data/Text/CreditsText_English.dat");
    expect(static_cast<bool>(credits), "FoC credits text uses the same layout");
    if (credits) {
        std::cout << "credits text corpus: " << credits.value().entries.size() << " records, "
                  << (credits.value().sorted_by_crc ? "sorted" : "not sorted") << " by CRC, "
                  << credits.value().duplicates.size() << " duplicated key(s)\n";
        expect(!credits.value().sorted_by_crc, "credits text is in display order, not CRC order");
    }
}
} // namespace

void text_database_contracts() {
    crc_contracts();
    layout_contracts();
    lookup_contracts();
    missing_key_contracts();
    path_contracts();
    corpus_contracts();
}
