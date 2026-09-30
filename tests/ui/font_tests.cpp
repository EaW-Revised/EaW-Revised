// Font provisioning contracts (UI-05 #191): sfnt validation, the font cache
// and the UI-F3 fallback chain. The fonts are synthetic and built here; the
// retail faces are never committed. tests/ui/test_extract_eaw_fonts.py covers
// the extraction tool and the real executable.

#include "eawr/data/ui/sfnt.hpp"
#include "eawr/presentation/ui/fonts.hpp"

#include "eawr/vfs/vfs.hpp"

#include "ui_test_support.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace eawr;
namespace fonts = presentation::ui;
using fonts::FaceSource;

using Bytes = std::vector<std::byte>;
using Tables = std::map<std::string, Bytes>;

void expect(const bool condition, const std::string& message) {
    test::ui::expect(condition, message.c_str());
}

void put_u16(Bytes& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
}

void put_u32(Bytes& bytes, const std::uint32_t value) {
    put_u16(bytes, value >> 16U);
    put_u16(bytes, value & 0xFFFFU);
}

void set_u32(Bytes& bytes, const std::size_t offset, const std::uint32_t value) {
    Bytes word;
    put_u32(word, value);
    std::copy(word.begin(), word.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::uint32_t sum(const Bytes& bytes, const std::size_t offset, const std::size_t length) {
    std::uint32_t total = 0U;
    for (std::size_t word = 0U; word < length; word += 4U) {
        std::uint32_t value = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            const std::size_t at = offset + word + index;
            value = (value << 8U) | (word + index < length ? std::to_integer<std::uint32_t>(bytes[at]) : 0U);
        }
        total += value;
    }
    return total;
}

Bytes name_table(const std::map<std::uint32_t, std::string>& names) {
    Bytes records;
    Bytes strings;
    for (const auto& [id, text] : names) {
        put_u16(records, 3U);
        put_u16(records, 1U);
        put_u16(records, 0x409U);
        put_u16(records, id);
        put_u16(records, static_cast<std::uint32_t>(text.size() * 2U));
        put_u16(records, static_cast<std::uint32_t>(strings.size()));
        for (const char character : text) put_u16(strings, static_cast<unsigned char>(character));
    }
    Bytes table;
    put_u16(table, 0U);
    put_u16(table, static_cast<std::uint32_t>(names.size()));
    put_u16(table, static_cast<std::uint32_t>(6U + records.size()));
    table.insert(table.end(), records.begin(), records.end());
    table.insert(table.end(), strings.begin(), strings.end());
    return table;
}

// The tables a TrueType face needs; only head and name carry real content,
// which is all read_sfnt_face looks at beyond the directory and checksums.
Tables face_tables(const std::string& full_name) {
    Tables tables;
    for (const char* tag : {"cmap", "glyf", "hhea", "hmtx", "loca", "maxp", "post"}) {
        tables[tag] = Bytes(12U, static_cast<std::byte>(tag[0]));
    }
    Bytes head;
    put_u32(head, 0x00010000U);
    put_u32(head, 0x00010000U);
    put_u32(head, 0U);
    put_u32(head, 0x5F0F3CF5U);
    head.resize(54U);
    tables["head"] = head;
    tables["name"] = name_table({{1U, full_name + " Family"}, {2U, "Regular"}, {4U, full_name}, {6U, full_name}});
    return tables;
}

// An sfnt with sorted records, 4-byte aligned tables and correct checksums.
Bytes assemble(const Tables& tables) {
    const auto count = static_cast<std::uint32_t>(tables.size());
    std::uint32_t selector = 0U;
    while ((2U << selector) <= count) ++selector;
    Bytes font;
    put_u32(font, 0x00010000U);
    put_u16(font, count);
    put_u16(font, 16U << selector);
    put_u16(font, selector);
    put_u16(font, count * 16U - (16U << selector));
    Bytes data;
    std::size_t offset = 12U + 16U * tables.size();
    std::size_t head_at = 0U;
    for (const auto& [tag, table] : tables) {
        const std::size_t at = offset + data.size();
        if (tag == "head") head_at = at;
        for (const char character : tag) font.push_back(static_cast<std::byte>(character));
        put_u32(font, sum(table, 0U, table.size()));
        put_u32(font, static_cast<std::uint32_t>(at));
        put_u32(font, static_cast<std::uint32_t>(table.size()));
        data.insert(data.end(), table.begin(), table.end());
        data.resize((data.size() + 3U) / 4U * 4U);
    }
    font.insert(font.end(), data.begin(), data.end());
    if (head_at != 0U) set_u32(font, head_at + 8U, 0xB1B0AFBAU - sum(font, 0U, font.size()));
    return font;
}

Bytes face(const std::string& full_name) {
    return assemble(face_tables(full_name));
}

std::size_t table_offset(const Bytes& font, const std::string& tag) {
    const std::size_t count = (std::to_integer<std::size_t>(font[4]) << 8U) | std::to_integer<std::size_t>(font[5]);
    for (std::size_t index = 0U; index < count; ++index) {
        const std::size_t at = 12U + 16U * index;
        std::string name;
        for (std::size_t character = 0U; character < 4U; ++character) {
            name.push_back(static_cast<char>(std::to_integer<unsigned char>(font[at + character])));
        }
        if (name == tag) {
            std::size_t offset = 0U;
            for (std::size_t byte = 8U; byte < 12U; ++byte) {
                offset = (offset << 8U) | std::to_integer<std::size_t>(font[at + byte]);
            }
            return offset;
        }
    }
    return 0U;
}

std::string rejection(const Bytes& font) {
    const auto result = data::ui::read_sfnt_face(font);
    if (result) return {};
    expect(result.error().code == data::ui::diagnostic_codes::sfnt_invalid, "sfnt errors use EAWR-UI-0401");
    return result.error().message;
}

void sfnt_contracts() {
    const Bytes font = face("EmpireAtWar-Bold");
    const auto names = data::ui::read_sfnt_face(font);
    expect(names.has_value(), "a well-formed synthetic face validates");
    if (names) {
        expect(names.value().full_name == "EmpireAtWar-Bold", "full name is name ID 4");
        expect(names.value().family == "EmpireAtWar-Bold Family", "family is name ID 1");
        expect(names.value().subfamily == "Regular", "subfamily is name ID 2");
        expect(names.value().postscript_name == "EmpireAtWar-Bold", "PostScript name is name ID 6");
    }

    expect(names && names.value().win_ascent == 0U && names.value().win_descent == 0U,
           "a face without OS/2 has no GDI cell metrics");
    Tables measured = face_tables("EmpireAtWar-Medium");
    Bytes& head_table = measured["head"];
    head_table[18] = std::byte{0x03};
    head_table[19] = std::byte{0xE8};
    Bytes hhea_table;
    put_u32(hhea_table, 0x00010000U);
    put_u16(hhea_table, 688U);
    put_u16(hhea_table, 0x10000U - 312U);
    hhea_table.resize(36U);
    measured["hhea"] = hhea_table;
    Bytes os2(78U, std::byte{0});
    os2[74] = std::byte{0x03};
    os2[75] = std::byte{0xA9};
    os2[77] = std::byte{0xC9};
    measured["OS/2"] = os2;
    const auto metrics = data::ui::read_sfnt_face(assemble(measured));
    expect(metrics && metrics.value().units_per_em == 1000U && metrics.value().ascender == 688
               && metrics.value().descender == -312 && metrics.value().win_ascent == 937U
               && metrics.value().win_descent == 201U,
           "vertical metrics come from head, hhea and OS/2");

    Bytes changed = font;
    changed[table_offset(changed, "glyf") + 1U] ^= std::byte{0x10};
    expect(rejection(changed) == "table 'glyf' checksum mismatch", "a changed table byte fails its checksum");

    Bytes adjusted = font;
    const std::size_t head = table_offset(adjusted, "head");
    adjusted[head + 11U] ^= std::byte{0x01};
    expect(rejection(adjusted) == "whole-font checksum adjustment mismatch", "the head adjustment is checked");

    Tables partial = face_tables("EmpireAtWar-Bold");
    partial.erase("cmap");
    partial.erase("loca");
    expect(rejection(assemble(partial)) == "required tables missing: cmap, loca", "required tables are named");

    Tables magicless = face_tables("EmpireAtWar-Bold");
    magicless["head"][12] = std::byte{0};
    expect(rejection(assemble(magicless)) == "head table has no magic number", "head needs its magic number");

    Tables nameless = face_tables("EmpireAtWar-Bold");
    nameless["name"] = name_table({{1U, "Only A Family"}});
    expect(rejection(assemble(nameless)) == "name table has no full name", "a face needs a full name");

    expect(rejection(Bytes(font.begin(), font.end() - 16)) == "the font runs past the end of the file",
           "a truncated face is rejected");
    expect(rejection(Bytes(8U)) == "shorter than a TrueType table directory", "a tiny file is rejected");
    Bytes otto = font;
    otto[0] = std::byte{'O'};
    otto[1] = std::byte{'T'};
    otto[2] = std::byte{'T'};
    otto[3] = std::byte{'O'};
    expect(rejection(otto) == "not a TrueType font", "CFF outlines are not accepted");
    Bytes search = font;
    search[7] ^= std::byte{0x10};
    expect(rejection(search) == "table directory search fields disagree with the table count",
           "the directory search fields are checked");
    Bytes unsorted = font;
    std::swap_ranges(unsorted.begin() + 12, unsorted.begin() + 16, unsorted.begin() + 28);
    expect(rejection(unsorted) == "table tags are not sorted", "table tags must be sorted");
    Bytes overlapping = font;
    for (std::size_t byte = 0U; byte < 4U; ++byte) overlapping[28U + 8U + byte] = overlapping[12U + 8U + byte];
    expect(rejection(overlapping).ends_with("overlap"), "overlapping tables are rejected");
}

// The cache directory mounted as the viewer mounts it: a loose layer at fonts/.
vfs::Vfs mount_cache(const std::filesystem::path& directory) {
    const vfs::MountSpec mount{.layer_id = "font-cache", .data_root = directory,
                               .loose_logical_prefix = std::string(fonts::font_cache_prefix),
                               .active_archives = {}};
    auto mounted = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&mount, 1));
    expect(mounted.has_value(), "the cache directory mounts");
    return mounted ? std::move(mounted.value()) : vfs::Vfs{};
}

void cache_contracts() {
    expect(fonts::font_cache_path("EmpireAtWar-Bold") == "fonts/EmpireAtWar-Bold.ttf",
           "cache files are fonts/<face>.ttf");
    test::ui::TempTree tree("fonts");
    const std::filesystem::path& root = tree.root;
    test::ui::write_bytes(root / "EmpireAtWar-Bold.ttf", face("EmpireAtWar-Bold"));
    // Light holds another face's font, Medium is spelled in lower case inside
    // and on disk, Stencil is garbage; the manifest is ignored.
    test::ui::write_bytes(root / "EmpireAtWar-Light.ttf", face("EmpireAtWar-Bold"));
    test::ui::write_bytes(root / "empireatwar-medium.TTF", face("empireatwar-medium"));
    const Bytes garbage(64U, std::byte{0x5A});
    test::ui::write_bytes(root / "EmpireAtWar-Stencil.ttf", garbage);
    test::ui::write_text(root / "fonts.json", "{}");

    const fonts::FontCache cache = fonts::load_font_cache(mount_cache(root), "out/fonts");
    expect(cache.directory == "out/fonts", "the cache keeps its directory label");
    expect(cache.faces.size() == 2U, "two cached faces are valid");
    const fonts::CachedFace* bold = cache.find("empireatwar-BOLD");
    expect(bold != nullptr && bold->face == "EmpireAtWar-Bold", "cached faces are found case-insensitively");
    if (bold != nullptr) {
        expect(bold->bytes == face("EmpireAtWar-Bold"), "a cached face keeps its bytes");
        expect(bold->logical_path == "fonts/EmpireAtWar-Bold.ttf", "a cached face records its logical path");
        expect(bold->sha256.size() == 64U, "a cached face records its SHA-256");
        expect(bold->names.full_name == "EmpireAtWar-Bold", "a cached face records its names");
    }
    expect(cache.find("EmpireAtWar-Medium") != nullptr, "the name check ignores case");
    expect(cache.find("EmpireAtWar-Light") == nullptr, "a file holding another face is left out");
    expect(cache.find("EmpireAtWar-Stencil") == nullptr, "an invalid file is left out");
    expect(cache.diagnostics.size() == 2U, "one warning per left-out face");
    for (const core::Diagnostic& diagnostic : cache.diagnostics) {
        expect(diagnostic.code == fonts::diagnostic_codes::font_cache_face
                   && diagnostic.severity == core::Severity::warning, "cache problems are EAWR-UI-0402 warnings");
    }
    if (cache.diagnostics.size() == 2U) {
        expect(cache.diagnostics[0].message == "EmpireAtWar-Light cache file holds EmpireAtWar-Bold",
               "the mismatch names the face found");
        expect(cache.diagnostics[1].message.starts_with("EmpireAtWar-Stencil cache file is not a valid TrueType font"),
               "the invalid file says why");
    }

    const fonts::FontCache empty = fonts::load_font_cache(vfs::Vfs{});
    expect(empty.faces.empty() && empty.diagnostics.size() == 4U, "an unmounted cache gives four warnings");
    if (!empty.diagnostics.empty()) {
        expect(empty.diagnostics[0].message
                   == "EmpireAtWar-Bold is not in the font cache; run tools/fonts/extract_eaw_fonts.py",
               "a missing face points at the extraction tool");
    }
}

void system_face_contracts() {
    const std::vector<std::string> families{"Arial", "Arial Black", "Arial Rounded MT Bold", "DejaVu Sans"};
    using fonts::SystemFace;
    expect(fonts::match_system_face("Arial", families) == SystemFace{"Arial", false}, "a family matches as a whole");
    expect(fonts::match_system_face("arial   black", families) == std::nullopt, "inner spaces are not folded");
    expect(fonts::match_system_face("ARIAL BLACK", families) == SystemFace{"Arial Black", false},
           "family names match case-insensitively");
    expect(fonts::match_system_face("Arial Bold", families) == SystemFace{"Arial", true},
           "a trailing Bold is the bold weight of the family");
    expect(fonts::match_system_face("Arial Rounded MT Bold", families) == SystemFace{"Arial Rounded MT Bold", false},
           "a family whose name ends in Bold is taken whole");
    expect(fonts::match_system_face("Arial Unicode MS", families) == std::nullopt, "an absent family has no match");
    expect(fonts::match_system_face("Arial Medium", families) == std::nullopt, "other styles are not families");
    expect(fonts::match_system_face(" Bold", families) == std::nullopt, "a bare style is no face");
}

fonts::FontCache cache_with(const std::vector<std::string>& faces) {
    fonts::FontCache cache;
    for (const std::string& name : faces) {
        fonts::CachedFace cached;
        cached.face = name;
        cache.faces.push_back(cached);
    }
    return cache;
}

struct ChainCase final {
    const char* label;
    std::string face;
    std::int32_t point_size;
    std::string language;
    std::vector<std::string> cached;
    std::vector<std::string> system;
    std::string expected_face;
    FaceSource expected_source;
    std::int32_t expected_size;
    bool substituted;
    std::vector<std::string> unavailable;
};

void fallback_contracts() {
    const std::vector<std::string> all{"EmpireAtWar-Bold", "EmpireAtWar-Light", "EmpireAtWar-Medium",
                                       "EmpireAtWar-Stencil"};
    const std::vector<std::string> windows{"Arial", "Arial Black", "Arial Unicode MS"};
    const std::vector<ChainCase> cases{
        {"a cached face is used as is", "EmpireAtWar-Bold", 10, "ENGLISH", all, windows,
         "EmpireAtWar-Bold", FaceSource::cache, 10, false, {}},
        {"a face spelled in another case is the cached face", "EmpireAtWar-light", 7, "ENGLISH", all, {},
         "EmpireAtWar-Light", FaceSource::cache, 7, false, {}},
        {"a system face is used as is", "Arial Bold", 8, "ENGLISH", all, {"Arial Bold"},
         "Arial Bold", FaceSource::system, 8, false, {}},
        {"a missing face falls back to the Unicode face", "Arial Medium", 14, "ENGLISH", all, windows,
         "Arial Unicode MS", FaceSource::system, 14, true, {"Arial Medium"}},
        {"then to EaW-Medium", "Arial", 7, "ENGLISH", all, {},
         "EmpireAtWar-Medium", FaceSource::cache, 7, true, {"Arial", "Arial Unicode MS"}},
        {"a missing embedded face falls back the same way", "EmpireAtWar-Stencil", 12, "ENGLISH",
         {"EmpireAtWar-Medium"}, {}, "EmpireAtWar-Medium", FaceSource::cache, 12, true,
         {"EmpireAtWar-Stencil", "Arial Unicode MS"}},
        {"embedded faces never come from the system", "EmpireAtWar-Bold", 10, "ENGLISH", {"EmpireAtWar-Medium"},
         {"EmpireAtWar-Bold"}, "EmpireAtWar-Medium", FaceSource::cache, 10, true,
         {"EmpireAtWar-Bold", "Arial Unicode MS"}},
        {"nothing available leaves the engine default", "EmpireAtWar-Bold", 24, "ENGLISH", {}, {},
         "", FaceSource::engine_default, 24, true, {"EmpireAtWar-Bold", "Arial Unicode MS", "EmpireAtWar-Medium"}},
        {"EaW-Medium itself is tried once", "EmpireAtWar-Medium", 7, "ENGLISH", {}, {},
         "", FaceSource::engine_default, 7, true, {"EmpireAtWar-Medium", "Arial Unicode MS"}},
        {"Russian always uses the Unicode face and raises the size", "EmpireAtWar-Bold", 5, "RUSSIAN", all, windows,
         "Arial Unicode MS", FaceSource::system, 7, true, {}},
        {"Russian keeps larger sizes", "EmpireAtWar-Bold", 10, "russian", all, windows,
         "Arial Unicode MS", FaceSource::system, 10, true, {}},
        {"Russian without the Unicode face takes EaW-Medium", "Arial", 6, "Russian", all, {"Arial"},
         "EmpireAtWar-Medium", FaceSource::cache, 7, true, {"Arial Unicode MS"}},
        {"Japanese always uses the Unicode face at its size", "EmpireAtWar-Medium", 5, "JAPANESE", all, windows,
         "Arial Unicode MS", FaceSource::system, 5, true, {}},
        {"a request for the Unicode face is not substituted", "Arial Unicode MS", 5, "JAPANESE", all, windows,
         "Arial Unicode MS", FaceSource::system, 5, false, {}},
        {"other languages keep the requested face", "EmpireAtWar-Bold", 5, "GERMAN", all, windows,
         "EmpireAtWar-Bold", FaceSource::cache, 5, false, {}},
    };
    for (const ChainCase& test : cases) {
        const fonts::FontCache cache = cache_with(test.cached);
        const std::vector<std::string> system = test.system;
        const fonts::SystemFaceProbe probe = [&](const std::string_view face) {
            return fonts::match_system_face(face, system).has_value();
        };
        const fonts::ResolvedFont resolved = fonts::resolve_font({test.face, test.point_size}, test.language,
                                                                 cache, probe);
        const std::string label = std::string("UI-F3: ") + test.label;
        expect(resolved.face == test.expected_face, label + " (face " + resolved.face + ")");
        expect(resolved.source == test.expected_source,
               label + " (source " + std::string(fonts::to_string(resolved.source)) + ")");
        expect(resolved.point_size == test.expected_size,
               label + " (size " + std::to_string(resolved.point_size) + ")");
        expect(resolved.substituted == test.substituted, label + " (substituted)");
        expect(resolved.unavailable == test.unavailable, label + " (unavailable chain)");
    }
    const fonts::ResolvedFont no_probe = fonts::resolve_font({"Arial", 7}, "ENGLISH", cache_with({}), {});
    expect(no_probe.source == FaceSource::engine_default, "UI-F3: without a system probe no system face exists");
}

} // namespace

void font_contracts() {
    sfnt_contracts();
    cache_contracts();
    system_face_contracts();
    fallback_contracts();
}
