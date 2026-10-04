#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void append_u16(std::vector<std::byte>& out, const std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xffU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void write_bytes(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

// Original test fixture: this manually lays out MEG-v1 bytes and deliberately is
// not the inverse of the production parser.
std::vector<std::byte> one_entry_meg(
    const std::string& name,
    const std::string& data,
    const std::uint32_t name_index = 0,
    const std::uint32_t offset_adjustment = 0
) {
    const auto payload_offset = static_cast<std::uint32_t>(8 + 2 + name.size() + 20);
    std::vector<std::byte> out;
    append_u32(out, 1); // filename count
    append_u32(out, 1); // entry count
    append_u16(out, static_cast<std::uint16_t>(name.size()));
    for (const char ch : name) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    append_u32(out, 0x12345678U); // CRC is opaque to the portable reader
    append_u32(out, 0); // engine index, also opaque
    append_u32(out, static_cast<std::uint32_t>(data.size()));
    append_u32(out, payload_offset + offset_adjustment);
    append_u32(out, name_index);
    for (const char ch : data) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    return out;
}

std::string bytes_as_string(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct TempTree {
    std::filesystem::path root;
    TempTree() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() / ("eawr-vfs-" + std::to_string(stamp));
        std::filesystem::create_directories(root);
    }
    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

void vfs_mod_chain() {
    using namespace eawr::vfs;
    TempTree tree;
    const auto leaf = tree.root / "leaf" / "Data";
    const auto parent = tree.root / "parent" / "Data";
    const auto expansion = tree.root / "expansion";
    const auto base = tree.root / "base";
    for (const auto& root : {leaf, parent, expansion, base})
        std::filesystem::create_directories(root);
    for (const auto& root : {parent, expansion, base})
        write_text(root / "MegaFiles.xml", "<Mega_Files><File>Absent.meg</File></Mega_Files>");
    write_text(leaf / "MegaFiles.xml", "<Mega_Files><File>PARENT.MEG</File><File>Second.meg</File></Mega_Files>");
    write_bytes(parent / "Parent.meg", one_entry_meg("Data/Art/Models/shell.alo", "parent shell"));
    write_bytes(parent / "Second.meg", one_entry_meg("Data/XML/order.xml", "second archive"));
    write_text(parent / "XML/order.xml", "parent loose");
    write_text(expansion / "XML/order.xml", "expansion loose");
    write_text(base / "XML/order.xml", "base loose");
    auto roots = mod_chain_roots(std::filesystem::path((tree.root / "leaf").native() +
        std::filesystem::path(";").native() + (tree.root / "parent").native()));
    roots.emplace_back("expansion", expansion);
    roots.emplace_back("base", base);
    auto chain = resolve_manifest_chain(roots);
    expect(chain && chain.value().size() == 4, "vfs_mod_chain resolves leaf, parent, expansion, base");
    if (!chain) return;
    expect(chain.value()[0].missing_archives.empty(), "leaf declarations resolve parent-only archives");
    expect(chain.value()[0].mount.active_archives.empty(), "parent archives retain parent precedence");
    std::vector<MountSpec> specs;
    for (auto& manifest : chain.value()) specs.push_back(std::move(manifest.mount));
    auto fs = Vfs::mount(specs);
    expect(static_cast<bool>(fs), "vfs_mod_chain mounts");
    if (!fs) return;
    auto shell = fs.value().stat("DATA/ART/MODELS/SHELL.ALO");
    expect(shell && shell.value().layer_id == "mod-parent-1" &&
        shell.value().source_id == "mod-parent-1:Data/PARENT.MEG", "inherited shell names its supplying layer");
    auto order = fs.value().open("data/xml/order.xml");
    expect(order && bytes_as_string(order.value()) == "parent loose", "parent loose beats inherited archive and lower layers");
    write_text(leaf / "XML/order.xml", "leaf loose");
    fs = Vfs::mount(specs);
    order = fs.value().open("data/xml/order.xml");
    expect(order && bytes_as_string(order.value()) == "leaf loose", "leaf overrides parent");
    std::filesystem::remove(parent / "XML/order.xml");
    std::filesystem::remove(leaf / "XML/order.xml");
    fs = Vfs::mount(specs);
    order = fs.value().open("data/xml/order.xml");
    expect(order && bytes_as_string(order.value()) == "second archive", "parent archive beats expansion loose");
    const auto semicolon = tree.root / "leaf;with semicolon";
    std::filesystem::create_directories(semicolon);
    auto single = mod_chain_roots(semicolon);
    expect(single.size() == 1 && single[0].first == "mod" && single[0].second == semicolon,
        "an existing directory named with ';' is one mod root, not a chain");
    auto invalid = mod_chain_roots(std::filesystem::path("leaf;;parent;"));
    expect(invalid.size() == 4 && invalid[1].second.empty() && invalid[3].second.empty(),
        "empty chain entries are invalid roots, never the working directory");
}

} // namespace

int main(const int argc, char** argv) {
    using namespace eawr::vfs;

    if (argc == 4 && std::string_view(argv[1]) == "--inspect-meg") {
        const std::filesystem::path archive(argv[2]);
        const auto root = archive.parent_path();
        const std::array specs{MountSpec{"inspection", root, "data", {ArchiveSpec{archive, "inspection:archive", {}}}}};
        auto inspection = Vfs::mount(specs);
        if (!inspection) {
            std::cerr << eawr::core::format_diagnostic(inspection.error()) << '\n';
            return 2;
        }
        auto bytes = inspection.value().open(argv[3]);
        if (!bytes) {
            std::cerr << eawr::core::format_diagnostic(bytes.error()) << '\n';
            return 2;
        }
        std::cout << "size=" << bytes.value().size() << " header=";
        const auto count = std::min<std::size_t>(4, bytes.value().size());
        constexpr char hex[] = "0123456789abcdef";
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = std::to_integer<unsigned char>(bytes.value()[i]);
            std::cout << hex[value >> 4U] << hex[value & 0x0fU];
        }
        std::cout << '\n';
        return 0;
    }

    auto canonical = canonicalize("Data\\XML/./Units.XML");
    expect(canonical && canonical.value() == "data/xml/units.xml", "slashes, dot, and ASCII case normalize");
    expect(!canonicalize("../secret.xml"), "traversal above the mount is rejected");
    expect(!canonicalize("C:\\game\\data.xml"), "drive-qualified paths are rejected");
    auto non_ascii = canonicalize("DATA/Ä/FILE.XML");
    expect(non_ascii && non_ascii.value() == "data/Ä/file.xml", "non-ASCII bytes are preserved without locale folding");

    TempTree fixture;
    const auto base = fixture.root / "base";
    const auto expansion = fixture.root / "expansion";
    const auto mod = fixture.root / "mod";
    std::filesystem::create_directories(base);
    std::filesystem::create_directories(expansion / "XML");
    std::filesystem::create_directories(mod / "XML");
    write_bytes(base / "base.meg", one_entry_meg("DATA\\XML\\SHARED.XML", "base-archive"));
    write_text(expansion / "XML" / "Shared.xml", "expansion-loose");
    write_bytes(mod / "mod.meg", one_entry_meg("DATA\\XML\\SHARED.XML", "mod-archive"));
    write_bytes(mod / "low.meg", one_entry_meg("DATA\\XML\\ORDER.XML", "low"));
    write_bytes(mod / "high.meg", one_entry_meg("DATA\\XML\\ORDER.XML", "high"));
    write_bytes(mod / "loose.meg", one_entry_meg("DATA\\XML\\LOOSE.XML", "archive"));
    write_text(mod / "XML" / "Loose.xml", "loose");

    std::vector<MountSpec> mounts{
        MountSpec{"mod", mod, "data", {
            ArchiveSpec{mod / "low.meg", "mod:low", {}},
            ArchiveSpec{mod / "high.meg", "mod:high", {}},
            ArchiveSpec{mod / "mod.meg", "mod:main", {}},
            ArchiveSpec{mod / "loose.meg", "mod:loose-archive", {}},
        }},
        MountSpec{"expansion", expansion, "data", {}},
        MountSpec{"base", base, "data", {ArchiveSpec{base / "base.meg", "base:main", {}}}},
    };
    auto mounted = Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "valid layers mount");
    if (mounted) {
        auto shared = mounted.value().open("DATA/XML/shared.xml");
        expect(shared && bytes_as_string(shared.value()) == "mod-archive", "stronger layer archive beats weaker loose file");
        auto loose = mounted.value().open("data\\xml\\LOOSE.xml");
        expect(loose && bytes_as_string(loose.value()) == "loose", "same-layer loose file beats archives");
        auto order = mounted.value().open("data/xml/order.xml");
        expect(order && bytes_as_string(order.value()) == "high", "later active archive wins within a layer");
        auto all = mounted.value().candidates("data/xml/shared.xml");
        expect(all && all.value().size() == 3, "candidate chain exposes all shadowed records");
        if (all && all.value().size() == 3) {
            expect(all.value()[0].layer_id == "mod", "candidate precedence starts at strongest layer");
            expect(all.value()[1].layer_id == "expansion", "candidate precedence retains middle layer");
            expect(all.value()[2].layer_id == "base", "candidate precedence ends at base layer");
        }
        auto xml = mounted.value().enumerate("DATA/XML", "XML");
        expect(xml && std::is_sorted(xml.value().begin(), xml.value().end(), [](const auto& a, const auto& b) {
            return a.canonical_path < b.canonical_path;
        }), "effective enumeration is deterministic and sorted");
        expect(!mounted.value().open("data/xml/missing.xml"), "missing asset returns an error");
    }

    write_bytes(fixture.root / "truncated.meg", std::array<std::byte, 3>{});
    auto bad_index = one_entry_meg("DATA\\BAD.XML", "x", 1);
    write_bytes(fixture.root / "bad-index.meg", bad_index);
    auto bad_bounds = one_entry_meg("DATA\\BAD.XML", "x", 0, 100);
    write_bytes(fixture.root / "bad-bounds.meg", bad_bounds);
    auto truncated = probe_meg_archive(fixture.root / "truncated.meg", "fixture:truncated");
    auto invalid_index = probe_meg_archive(fixture.root / "bad-index.meg", "fixture:bad-index");
    auto invalid_bounds = probe_meg_archive(fixture.root / "bad-bounds.meg", "fixture:bad-bounds");
    expect(!truncated && truncated.error().code == diagnostic_codes::truncated_archive, "truncated archive has stable diagnostic");
    expect(!invalid_index && invalid_index.error().code == diagnostic_codes::archive_index, "bad name index has stable diagnostic");
    expect(!invalid_bounds && invalid_bounds.error().code == diagnostic_codes::archive_bounds, "bad entry bounds have stable diagnostic");

    const auto manifest_root = fixture.root / "manifest";
    std::filesystem::create_directories(manifest_root / "Audio" / "SFX");
    write_text(manifest_root / "MegaFiles.xml", "<Mega_Files><File>Data\\Base.meg</File></Mega_Files>");
    write_bytes(manifest_root / "Base.meg", one_entry_meg("DATA\\BASE.XML", "base"));
    write_bytes(manifest_root / "Patch.meg", one_entry_meg("DATA\\PATCH.XML", "patch"));
    write_bytes(manifest_root / "Patch2.meg", one_entry_meg("DATA\\PATCH.XML", "patch2"));
    write_bytes(manifest_root / "Inactive.meg", one_entry_meg("DATA\\NO.XML", "no"));
    write_bytes(manifest_root / "Audio" / "SFX" / "SFX2D_English.meg", one_entry_meg("VOICE.WAV", "voice"));
    auto resolved = resolve_manifest_mount("fixture", manifest_root);
    expect(resolved && resolved.value().mount.active_archives.size() == 4, "manifest plus exact conventional archives resolve");
    if (resolved) {
        const auto& active = resolved.value().mount.active_archives;
        expect(active[0].source_id.ends_with("Base.meg"), "manifest order is retained");
        expect(active[1].source_id.ends_with("SFX2D_English.meg"), "exact SFX convention follows manifest");
        expect(active[2].source_id.ends_with("Patch.meg") && active[3].source_id.ends_with("Patch2.meg"),
               "patch slots are appended in evidenced order");
        expect(std::none_of(active.begin(), active.end(), [](const auto& archive) {
            return archive.source_id.ends_with("Inactive.meg");
        }), "unlisted archives are not activated by discovery");
    }

    write_text(
        manifest_root / "MegaFiles.xml",
        "<?xml version=\"1.0\"?><Mega_Files>"
        "<!-- <File>Comment.meg</File> -->"
        "<?ignored <File>Instruction.meg</File>?>"
        "<![CDATA[<File>RootCdata.meg</File>]]>"
        "<File>Entity&amp;Name.meg</File>"
        "<File>Split<!-- ignored --><?ignored value?><![CDATA[Name]]>.meg</File>"
        "</Mega_Files>");
    auto structured = resolve_manifest_mount("fixture", manifest_root);
    expect(structured && structured.value().declared_archives == std::vector<std::string>{
        "Data/Entity&Name.meg", "Data/SplitName.meg"},
        "comments, processing instructions and root CDATA do not activate entries; entities and File CDATA decode");

    write_text(
        manifest_root / "MegaFiles.xml",
        "<Mega_Files><Group><File>Nested.meg</File></Group>"
        "<file>WrongCase.meg</file><File>Direct.meg</File></Mega_Files>");
    auto direct_only = resolve_manifest_mount("fixture", manifest_root);
    expect(direct_only && direct_only.value().declared_archives == std::vector<std::string>{"Data/Direct.meg"},
           "only case-sensitive direct File children of Mega_Files are declarations");

    write_text(manifest_root / "MegaFiles.xml", "<Mega_Files><File>Broken.meg</Mega_Files>");
    auto malformed_manifest = resolve_manifest_mount("fixture", manifest_root);
    expect(!malformed_manifest && malformed_manifest.error().code == diagnostic_codes::manifest_invalid,
           "malformed manifest fails with the stable manifest diagnostic");

    write_text(manifest_root / "MegaFiles.xml", "<Other><File>WrongRoot.meg</File></Other>");
    auto wrong_root = resolve_manifest_mount("fixture", manifest_root);
    expect(!wrong_root && wrong_root.error().code == diagnostic_codes::manifest_invalid,
           "manifest requires the intended Mega_Files root");

    write_text(manifest_root / "MegaFiles.xml", "<Mega_Files><File>Bad<Part/>.meg</File></Mega_Files>");
    auto nested_content = resolve_manifest_mount("fixture", manifest_root);
    expect(!nested_content && nested_content.error().code == diagnostic_codes::manifest_invalid,
           "File entries with element children are rejected instead of flattened");

    write_text(
        manifest_root / "MegaFiles.xml",
        "<!DOCTYPE Mega_Files SYSTEM \"https://invalid.example/eawr.dtd\">"
        "<Mega_Files><File>Direct.meg</File></Mega_Files>");
    auto external_doctype = resolve_manifest_mount("fixture", manifest_root);
    expect(!external_doctype && external_doctype.error().code == diagnostic_codes::manifest_invalid,
           "document types are rejected without external entity or network resolution");

    const auto collision = fixture.root / "collision";
    std::filesystem::create_directories(collision);
    write_text(collision / "Case.XML", "a");
    expect(std::filesystem::is_regular_file(collision / "Case.XML"), "case-sensitivity probe file exists");
    std::error_code probe_error;
    const bool case_insensitive = std::filesystem::exists(collision / "case.xml", probe_error);
    expect(!probe_error, "case-sensitivity probe succeeds");
    if (case_insensitive) {
        std::cout << "SKIP: same-layer loose case collision requires a case-sensitive filesystem\n";
    } else if (!probe_error) {
        write_text(collision / "case.xml", "b");
        const std::array collision_mounts{MountSpec{"collision", collision, "data", {}}};
        auto collision_result = Vfs::mount(collision_mounts);
        expect(!collision_result && collision_result.error().code == diagnostic_codes::loose_case_collision,
               "same-layer loose case collision fails deterministically");
    }

    vfs_mod_chain();

    if (failures == 0) std::cout << "vfs contracts passed\n";
    return failures == 0 ? 0 : 1;
}
